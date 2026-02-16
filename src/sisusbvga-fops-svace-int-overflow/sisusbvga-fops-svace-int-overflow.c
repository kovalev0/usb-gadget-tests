// SPDX-License-Identifier: Apache-2.0
//
// Emulates a SiS USB-to-VGA adapter device (VID: 0x0711, PID: 0x0900),
// simulating full device initialization including graphics core setup.
// It uses USB 2.0 protocol over a HIGH_SPEED connection.
// Six bulk endpoints are configured: GFX_OUT, GFX_IN, GFX_BULK_OUT,
// GFX_LBULK_OUT, BRIDGE_OUT, and BRIDGE_IN.
// Handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION).
//
// Implements complete graphics initialization (sisusb->gfxinit = 1):
// - PCI configuration space (header 0x008f)
// - Bridge registers for device control
// - VGA IO registers (type 0x01) for sequencer and graphics setup
// - 8MB VRAM emulation with SDR 1ch/1rank configuration
// - RAM type detection (SDR) and VRAM size reporting via SR registers
// - Falsified VRAM size (1GB) returned via SR[0x14] to simulate a
//   compromised USB device; 'overflow' flag set by bridge handler when
//   out-of-bounds bulk transfer is detected
// - Small/large bulk transfer support for efficient VRAM operations
//
// STATIC ANALYZER WARNING TESTING: SVACE warning validation (1 test)
// Reproduces condition reported by static analyzer Svace:
// - TEST: Integer overflow in 'address + length' (sisusbvga.c:1312)
//   Uses SUCMD_CLRSCR with address=0xFFFFFFF0 and length=0xFFFFFF to trigger
//   the overflow path in sisusb_clear_vram(); 'overflow' flag confirms
//   that out-of-bounds access reached the bridge handler
//
// Vasiliy Kovalev <kovalev@altlinux.org>

#include "../usb_gadget_tests.h"

#include <dirent.h>

/*----------------------------------------------------------------------*/

static bool verbose = false;

#define VLOG(...) do { if (verbose) printf(__VA_ARGS__); } while(0)

static volatile bool keep_running = true;
static volatile bool main_running = true;
static volatile bool device_init = false;

static volatile bool overflow = false;

static bool strict_bounds_check = false;

/*----------------------------------------------------------------------*/

#define SISUSB_TYPE_MEM		0
#define SISUSB_TYPE_IO		1

#define SISUSB_PCI_IOPORTBASE	0x0000d000
#define SISUSB_PCI_MEMBASE	0xd0000000

#define PCI_CONFIG_SIZE		128
#define PCI_BRIDGE_REG_SIZE	1024
#define GFX_REG_SIZE		128

#define VRAM_SIZE		8 * (1024 * 1024)		// 8 Mb
#define VRAM_SIZE_BAD		( 1 * (1024 * 1024 * 1024) )	// 1 Gb

#define RAM_TYPE_SDR		0x1
#define RAM_TYPE_DDR		0x3
#define RAM_TYPE		RAM_TYPE_SDR

static uint32_t pci_config[PCI_CONFIG_SIZE]	= {0};
static uint32_t bridge_reg[PCI_BRIDGE_REG_SIZE]	= {0};
static uint32_t gfx_reg[GFX_REG_SIZE]		= {0};
static uint8_t *vram = NULL;

struct sisusb_packet {
	unsigned short header;
	uint32_t address;
	uint32_t data;
} __attribute__ ((__packed__));


// SISSR 0x14 bits 2-3 (RAM Topology)
#define SISUSB_RAM_1CH_1R	0x00 // 1 channel / 1 rank (1x)
#define SISUSB_RAM_1CH_2R	0x01 // 1 channel / 2 rank (2x)
#define SISUSB_RAM_ASYM		0x02 // Asymmetric (1.5x)
#define SISUSB_RAM_2CH		0x03 // 2 channel (2x)
#define SISUSB_RAM_CONFIG	SISUSB_RAM_1CH_1R

uint8_t get_vram_config_reg(uint32_t size_bytes, uint8_t mode)
{
	uint32_t mb = size_bytes / (1024 * 1024);
	uint8_t power = 0;

	if (mb <= 0) return 0;

	// Adjust base MB depending on topology mode
	if (mode == SISUSB_RAM_ASYM) {
		mb = (mb * 2) / 3; // Reverse 1.5x
	} else if (mode == SISUSB_RAM_1CH_2R || mode == SISUSB_RAM_2CH) {
		mb >>= 1; // Reverse 2x
	}

	// Find log2 of base MB
	while (mb > 1) {
		mb >>= 1;
		power++;
	}

	// Bit 7-4: base size, Bit 3-2: mode, Bit 1-0: bus index
	return (power << 4) | (mode << 2);
}

/*----------------------------------------------------------------------*/
static struct {
	uint32_t address;	// Target VRAM address
	uint32_t length;	// Transfer length
	uint32_t flags;		// Transfer flags
	bool configured;	// Whether bulk transfer is configured
} bulk_state = {0};

void init_vram(void) {
	vram = malloc(VRAM_SIZE);
	if (!vram) {
		printf("[ERROR] Failed to allocate VRAM!\n");
		exit(1);
	}
	memset(vram, 0, VRAM_SIZE);
	printf("[VRAM] Allocated %d MB of emulated VRAM\n", VRAM_SIZE / (1024*1024));
}

// Bulk write to VRAM - for SMALL bulk transfers (ep 0x01)
void vram_bulk_write(uint32_t address, const uint8_t *data, uint32_t length) {
	uint32_t base_addr = address - SISUSB_PCI_MEMBASE;
	if (base_addr + length > VRAM_SIZE) {
		VLOG("[WARNING] Bulk write would exceed VRAM bounds, truncating\n");
		length = 0;
	}

	if (length > 0) {
		memcpy(&vram[base_addr], data, length);
		VLOG("  BULK WRITE VRAM[0x%08x] length=%u bytes\n", address, length);
	}
}

uint32_t process_packet_bridge(struct sisusb_packet *pkt, bool is_read) {
	uint16_t header = __le16_to_cpu(pkt->header);
	uint32_t address = __le32_to_cpu(pkt->address);
	uint32_t data = __le32_to_cpu(pkt->data);
	uint32_t result = 0;
	uint32_t reg_offset = address / 4;

	VLOG("[BRIDGE] header=0x%04x, addr=0x%08x, data=0x%08x, %s\n",
	     header, address, data, is_read ? "READ" : "WRITE");

	// Bridge packets typically have header 0x001f
	if (header != 0x001f && header != 0x000f) {
		printf("[WARNING] Unexpected bridge packet header: 0x%04x\n", header);
	}

	if (reg_offset >= PCI_BRIDGE_REG_SIZE) {
		printf("[WARNING] Bridge register offset 0x%x out of bounds\n", reg_offset);
		return 0;
	}

	if (is_read) {
		result = bridge_reg[reg_offset];
		VLOG("  READ BRIDGE[0x%03x] = 0x%08x\n", address, result);
	} else {
		bridge_reg[reg_offset] = data;
		VLOG("  WRITE BRIDGE[0x%03x] = 0x%08x\n", address, data);

		// Handle bulk transfer configuration registers
		switch (address) {
		case 0x194:	// Small bulk: address register
		case 0x1d4:	// Large bulk: address register
			bulk_state.address = data;
			VLOG("  [BULK CONFIG] Address = 0x%08x\n", data);
			break;

		case 0x190:	// Small bulk: length register
		case 0x1d0:	// Large bulk: length register
			bulk_state.length = data;
			VLOG("  [BULK CONFIG] Length = %u bytes\n", data);
			break;

		case 0x180:	// Small bulk: flags/command register
		case 0x1c0:	// Large bulk: flags/command register
			bulk_state.flags = data;
			bulk_state.configured = true;
			VLOG("  [BULK CONFIG] Flags = 0x%08x, ready for transfer\n", data);
			break;
		}

		if (bulk_state.address == 0xfffffff0 && bulk_state.length > 0x10)
			overflow = true;
	}

	return result;
}

uint32_t process_packet_gfx(struct sisusb_packet *pkt, bool is_read) {
	uint16_t header = __le16_to_cpu(pkt->header);
	uint32_t address = __le32_to_cpu(pkt->address);
	uint32_t data = __le32_to_cpu(pkt->data);

	uint32_t result = 0;

	if (header == 0x008f) {
		address = address & (PCI_CONFIG_SIZE - 1);
		if (is_read) {
			// sisusb_read_pci_config
			result = pci_config[address];
			VLOG("  READ PCI[0x%02x] = 0x%08x\n", address, result);
		} else {
			// sisusb_write_pci_config
			pci_config[address] = data;
			VLOG("  WRITE PCI[0x%02x] = 0x%08x\n", address, data);
		}

		return result;
	}

	// Initialize the graphics core
	// https://elixir.bootlin.com/linux/v6.12.66/source/drivers/usb/misc/sisusbvga/sisusbvga.c#L2185

	int type = (header >> 6) & 0x03;

	if (type == SISUSB_TYPE_IO) {
		uint32_t offset;
		uint8_t low_bits = header & 0x0F;
		static bool ramtype_req = false;
		static bool vramsize_req = false;

		if (low_bits == 8) offset = 3;
		else if (low_bits == 4) offset = 2;
		else if (low_bits == 2) offset = 1;
		else offset = 0;

		address = address & ~SISUSB_PCI_IOPORTBASE;
		address = address + offset;

		data = (data >> ((address & 3) << 3)) & 0xFF;

		if (address < GFX_REG_SIZE) {
			if (is_read) {
				result = gfx_reg[address];
				VLOG("  READ GFX REG IO[0x%02x] = 0x%08x\n", address, result);
			} else {
				gfx_reg[address] = data;
				VLOG("  WRITE GFX REG IO[0x%02x] = 0x%08x\n", address, data);
			}

			// https://elixir.bootlin.com/linux/v6.12.66/source/drivers/usb/misc/sisusbvga/sisusbvga.c#L1894
			if (ramtype_req) {
				result = RAM_TYPE;
				ramtype_req = false;
			}

			// https://elixir.bootlin.com/linux/v6.12.66/source/drivers/usb/misc/sisusbvga/sisusbvga.c#L2035
			if (vramsize_req) {
				result = get_vram_config_reg(VRAM_SIZE_BAD, SISUSB_RAM_CONFIG);
				vramsize_req = false;
			}

			if (address == 0x44) {
				if (data == 0x3a) {
					ramtype_req = true;

				} else if (data == 0x14) {
					vramsize_req = true;
				}
			}

			result = result << ((address & 3) << 3);
		}
	} else if (type == SISUSB_TYPE_MEM) {
		uint8_t be_mask = header & 0x0F;
		uint32_t base_addr = address - SISUSB_PCI_MEMBASE;
		uint32_t vram_mask = VRAM_SIZE - 1;

		if (is_read) {
			result = 0;
			for (int i = 0; i < 4; i++) {
				if (be_mask & (1 << i)) {
					uint32_t curr_addr = base_addr + i;

					if (!strict_bounds_check)
						 curr_addr &= vram_mask;

					if (curr_addr < VRAM_SIZE) {
						result |= (uint32_t)vram[curr_addr] << (i * 8);
					} else {
						printf("READ VRAM: Address [0x%08x] out of bounds\n", address);
					}
				}
			}
			VLOG("  READ VRAM[0x%08x] Mask[0x%x] = 0x%08x\n", address, be_mask, result);
		} else {
			for (int i = 0; i < 4; i++) {
				if (be_mask & (1 << i)) {
					uint32_t curr_addr = base_addr + i;

					if (!strict_bounds_check)
						 curr_addr &= vram_mask;

					if (curr_addr < VRAM_SIZE) {
						vram[curr_addr] = (uint8_t)(data >> (i * 8));
					} else {
						printf("WRITE VRAM: Address [0x%08x] out of bounds\n", address);
					}
				}
			}
			VLOG("  WRITE VRAM[0x%08x] Mask[0x%x] = 0x%08x\n", address, be_mask, data);

			// Corner: (479*640*2) + (639*2) = 0x95FFE.
			// Logic address 0x95FFE is packet 0x95FFC with mask 0xC.
			// Triggers twice: horizontal and vertical line loops.
			if (address == 0xd0095ffc && data == 0xf1000000) {
				static int corner_hits = 0;
				corner_hits++;

				if (corner_hits == 1) {
					printf("[Setup screen] Bottom-Horizontal Line Done\n");
				}
				else if (corner_hits == 2) {
					printf("[Setup screen] Right-Vertical Line Done (Frame Complete)\n");
					main_running = false;
				}
			}
		}
	} else {
		printf("[ERROR] GFX: Unknown SISUSB_TYPE\n");
	}

	return result;
}

/*----------------------------------------------------------------------*/
/* Device File Operations Test */
/*----------------------------------------------------------------------*/

#define SISUSB_PCI_PSEUDO_MEMBASE	0x10000000
#define SISUSB_PCI_PSEUDO_MMIOBASE	0x20000000
#define SISUSB_PCI_PSEUDO_IOPORTBASE	0x0000d000
#define SISUSB_PCI_PSEUDO_PCIBASE	0x00010000

#define SISUSB_ID  0x53495355	// 'SISU'

struct sisusb_info {
	__u32 sisusb_id;
	__u8 sisusb_version;
	__u8 sisusb_revision;
	__u8 sisusb_patchlevel;
	__u8 sisusb_gfxinit;

	__u32 sisusb_vrambase;
	__u32 sisusb_mmiobase;
	__u32 sisusb_iobase;
	__u32 sisusb_pcibase;

	__u32 sisusb_vramsize;
	__u32 sisusb_minor;
	__u32 sisusb_fbdevactive;
	__u32 sisusb_conactive;

	__u8 sisusb_reserved[28];
};

struct sisusb_command {
	__u8 operation;
	__u8 data0;
	__u8 data1;
	__u8 data2;
	__u32 data3;
	__u32 data4;
};

#define SUCMD_GET	0x01
#define SUCMD_SET	0x02
#define SUCMD_SETOR	0x03
#define SUCMD_SETAND	0x04
#define SUCMD_SETANDOR	0x05
#define SUCMD_SETMASK	0x06
#define SUCMD_CLRSCR	0x07

#define SISUSB_COMMAND		_IOWR(0xF3,0x3D,struct sisusb_command)
#define SISUSB_GET_CONFIG_SIZE	_IOR(0xF3,0x3E,__u32)
#define SISUSB_GET_CONFIG	_IOR(0xF3,0x3F,struct sisusb_info)

static int find_device(char *devpath, size_t maxlen) {
	DIR *dir = opendir("/dev");
	if (!dir) return -1;

	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "sisusbvga", 9) == 0) {
			snprintf(devpath, maxlen, "/dev/%s", entry->d_name);
			closedir(dir);
			return 0;
		}
	}
	closedir(dir);
	return -1;
}

void test_static_analyzer_warning(void) {
	char devpath[512];
	int rv;

	printf("\n[VALIDATION] Static Analyzer Warning Testing\n");
	sleep(1);

	if (find_device(devpath, sizeof(devpath)) != 0) {
		printf("[TEST] ERROR: Device not found\n");
		return;
	}

	printf("[TEST] Device found\n");

	int devfd = open(devpath, O_RDWR);
	if (devfd < 0) {
		printf("[TEST] ERROR: Failed to open device: %s\n", strerror(errno));
		return;
	}

	printf("[TEST] Device opened successfully\n\n");

	struct sisusb_command cmd;

	printf("[TEST] address + length overflow\n\n");
	overflow = false;
	memset(&cmd, 0, sizeof(cmd));
	cmd.operation = SUCMD_CLRSCR;

	// Maximum 24-bit length (0xFFFFFF = 16,777,215 bytes)
	uint32_t max_length = 0xFFFFFF;
	cmd.data0 = (max_length >> 16) & 0xFF;
	cmd.data1 = (max_length >> 8) & 0xFF;
	cmd.data2 = max_length & 0xFF;

	// Calculation for data3 to reach address 0xFFFFFFF0:
	// address = data3 - SISUSB_PCI_PSEUDO_MEMBASE + SISUSB_PCI_MEMBASE
	// data3 = 0xFFFFFFF0 + 0x10000000 - 0xD0000000 = 0x3FFFFFF0
	cmd.data3 = 0x3FFFFFF0;

	// Execute the command
	rv = ioctl(devfd, SISUSB_COMMAND, &cmd);

	if (overflow) {
		printf("[TEST] Result: UNSAFE WRITE DETECTED!\n");
		printf("[TEST] Note: Out-of-bounds write occurred in the device memory\n");
		if (rv < 0) {
			printf("[TEST] Status: Driver also suffered/failed (errno=%s)\n", strerror(errno));
		} else {
			printf("[TEST] Status: Driver reported SUCCESS despite the corruption\n");
		}
	} else {
		if (rv >= 0) {
			printf("[TEST] Result: Handled safely (No illegal write detected)\n");
		} else {
			printf("[TEST] Result: Rejected (errno=%s)\n", strerror(errno));
		}
	}

	close(devfd);
}

/*----------------------------------------------------------------------*/
/* USB device descriptors */
/*----------------------------------------------------------------------*/

#define BCD_USB			0x0200

#define USB_VENDOR		0x0711	// Magic Control Technology Corp.
#define USB_PRODUCT		0x0900	// SVGA Adapter

#define STRING_ID_MANUFACTURER	1
#define STRING_ID_PRODUCT	2
#define STRING_ID_SERIAL	3
#define STRING_ID_CONFIG	4
#define STRING_ID_INTERFACE	5

#define EP_MAX_PACKET_CONTROL	64
#define EP_MAX_PACKET_BULK	512

#define EP_NUM_GFX_OUT		0x0e
#define EP_NUM_GFX_IN		0x0e
#define EP_NUM_GFX_BULK_OUT	0x01
#define EP_NUM_GFX_LBULK_OUT	0x03
#define EP_NUM_BRIDGE_OUT	0x0d
#define EP_NUM_BRIDGE_IN	0x0d

struct usb_device_descriptor usb_device = {
	.bLength =		USB_DT_DEVICE_SIZE,
	.bDescriptorType =	USB_DT_DEVICE,
	.bcdUSB =		__constant_cpu_to_le16(BCD_USB),
	.bDeviceClass =		0,
	.bDeviceSubClass =	0,
	.bDeviceProtocol =	0,
	.bMaxPacketSize0 =	EP_MAX_PACKET_CONTROL,
	.idVendor =		__constant_cpu_to_le16(USB_VENDOR),
	.idProduct =		__constant_cpu_to_le16(USB_PRODUCT),
	.bcdDevice =		__constant_cpu_to_le16(0x0100),
	.iManufacturer =	STRING_ID_MANUFACTURER,
	.iProduct =		STRING_ID_PRODUCT,
	.iSerialNumber =	STRING_ID_SERIAL,
	.bNumConfigurations =	1,
};

struct usb_config_descriptor usb_config = {
	.bLength =		USB_DT_CONFIG_SIZE,
	.bDescriptorType =	USB_DT_CONFIG,
	.wTotalLength =		0,
	.bNumInterfaces =	1,
	.bConfigurationValue =	1,
	.iConfiguration = 	STRING_ID_CONFIG,
	.bmAttributes =		USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
	.bMaxPower =		0x32,
};

struct usb_interface_descriptor usb_interface = {
	.bLength =		USB_DT_INTERFACE_SIZE,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bInterfaceNumber =	0,
	.bAlternateSetting =	0,
	.bNumEndpoints =	6,
	.bInterfaceClass =	0,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	0,
	.iInterface =		STRING_ID_INTERFACE,
};

struct usb_endpoint_descriptor usb_endpoint_gfx_out = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_OUT | EP_NUM_GFX_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	EP_MAX_PACKET_BULK,
};

struct usb_endpoint_descriptor usb_endpoint_gfx_in = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN | EP_NUM_GFX_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	EP_MAX_PACKET_BULK,
};

struct usb_endpoint_descriptor usb_endpoint_gfx_bulk_out = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_OUT | EP_NUM_GFX_BULK_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	EP_MAX_PACKET_BULK,
};

struct usb_endpoint_descriptor usb_endpoint_gfx_lbulk_out = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_OUT | EP_NUM_GFX_LBULK_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	EP_MAX_PACKET_BULK,
};

struct usb_endpoint_descriptor usb_endpoint_bridge_out = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_OUT | EP_NUM_BRIDGE_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	EP_MAX_PACKET_BULK,
};

struct usb_endpoint_descriptor usb_endpoint_bridge_in = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN | EP_NUM_BRIDGE_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	EP_MAX_PACKET_BULK,
};

/*----------------------------------------------------------------------*/
/* Configuration builder */
/*----------------------------------------------------------------------*/

int build_config(char *data, int length) {
	struct usb_config_descriptor *config =
		(struct usb_config_descriptor *)data;
	int total_length = 0;

	assert(length >= sizeof(usb_config));
	memcpy(data, &usb_config, sizeof(usb_config));
	data += sizeof(usb_config);
	total_length += sizeof(usb_config);

	assert(length >= sizeof(usb_interface));
	memcpy(data, &usb_interface, sizeof(usb_interface));
	data += sizeof(usb_interface);
	total_length += sizeof(usb_interface);

	memcpy(data, &usb_endpoint_gfx_out, USB_DT_ENDPOINT_SIZE);
	data += USB_DT_ENDPOINT_SIZE;
	total_length += USB_DT_ENDPOINT_SIZE;

	memcpy(data, &usb_endpoint_gfx_in, USB_DT_ENDPOINT_SIZE);
	data += USB_DT_ENDPOINT_SIZE;
	total_length += USB_DT_ENDPOINT_SIZE;

	memcpy(data, &usb_endpoint_gfx_bulk_out, USB_DT_ENDPOINT_SIZE);
	data += USB_DT_ENDPOINT_SIZE;
	total_length += USB_DT_ENDPOINT_SIZE;

	memcpy(data, &usb_endpoint_gfx_lbulk_out, USB_DT_ENDPOINT_SIZE);
	data += USB_DT_ENDPOINT_SIZE;
	total_length += USB_DT_ENDPOINT_SIZE;

	memcpy(data, &usb_endpoint_bridge_in, USB_DT_ENDPOINT_SIZE);
	data += USB_DT_ENDPOINT_SIZE;
	total_length += USB_DT_ENDPOINT_SIZE;

	memcpy(data, &usb_endpoint_bridge_out, USB_DT_ENDPOINT_SIZE);
	data += USB_DT_ENDPOINT_SIZE;
	total_length += USB_DT_ENDPOINT_SIZE;

	config->wTotalLength = __cpu_to_le16(total_length);
	printf("config->wTotalLength: %d\n", total_length);

	return total_length;
}

/*----------------------------------------------------------------------*/
/* Endpoint assignment */
/*----------------------------------------------------------------------*/

bool assign_ep_address(struct usb_raw_ep_info *info,
				struct usb_endpoint_descriptor *ep) {
	if (usb_endpoint_num(ep) != 0)
		return false;
	if (usb_endpoint_dir_in(ep) && !info->caps.dir_in)
		return false;
	if (usb_endpoint_dir_out(ep) && !info->caps.dir_out)
		return false;
	if (usb_endpoint_maxp(ep) > info->limits.maxpacket_limit)
		return false;
	if (!info->caps.type_bulk)
		return false;

	if (info->addr == USB_RAW_EP_ADDR_ANY) {
		static int addr = 1;
		ep->bEndpointAddress |= addr++;
	} else
		ep->bEndpointAddress |= info->addr;
	return true;
}

void process_eps_info(int fd) {
	struct usb_raw_eps_info info;
	memset(&info, 0, sizeof(info));

	int num = usb_raw_eps_info(fd, &info);

	for (int i = 0; i < num; i++) {
		if (assign_ep_address(&info.eps[i], &usb_endpoint_gfx_out))
			continue;
		if (assign_ep_address(&info.eps[i], &usb_endpoint_gfx_in))
			continue;
		if (assign_ep_address(&info.eps[i], &usb_endpoint_gfx_bulk_out))
			continue;
		if (assign_ep_address(&info.eps[i], &usb_endpoint_gfx_lbulk_out))
			continue;
		if (assign_ep_address(&info.eps[i], &usb_endpoint_bridge_out))
			continue;
		if (assign_ep_address(&info.eps[i], &usb_endpoint_bridge_in))
			continue;
	}

	int gfx_out_addr = usb_endpoint_num(&usb_endpoint_gfx_out);
	int gfx_in_addr = usb_endpoint_num(&usb_endpoint_gfx_in);
	int bulk_out_addr = usb_endpoint_num(&usb_endpoint_gfx_bulk_out);
	int lbulk_out_addr = usb_endpoint_num(&usb_endpoint_gfx_lbulk_out);
	int bridge_out_addr = usb_endpoint_num(&usb_endpoint_bridge_out);
	int bridge_in_addr = usb_endpoint_num(&usb_endpoint_bridge_in);

	assert(gfx_out_addr != 0);
	assert(gfx_in_addr != 0);
	assert(bulk_out_addr != 0);
	assert(lbulk_out_addr != 0);
	assert(bridge_out_addr != 0);
	assert(bridge_in_addr != 0);
}

/*----------------------------------------------------------------------*/
/* Endpoint threads */
/*----------------------------------------------------------------------*/

struct usb_raw_control_event {
	struct usb_raw_event		inner;
	struct usb_ctrlrequest		ctrl;
};

struct usb_raw_control_io {
	struct usb_raw_ep_io		inner;
	char				data[EP_MAX_PACKET_CONTROL];
};

struct usb_raw_bulk_io {
	struct usb_raw_ep_io		inner;
	char				data[EP_MAX_PACKET_BULK];
};

int ep_gfx_out = -1;
int ep_gfx_in = -1;
int ep_gfx_bulk_out = -1;
int ep_gfx_lbulk_out = -1;
int ep_bridge_out = -1;
int ep_bridge_in = -1;

pthread_t ep_bridge_thread;
pthread_t ep_gfx_thread;
pthread_t ep_bulk_thread;
pthread_t ep_lbulk_thread;

void *ep_bridge_loop(void *arg) {
	int fd = (int)(long)arg;
	struct usb_raw_bulk_io io;

	VLOG("[THREAD] Bridge endpoint thread started\n");

	while (keep_running) {
		assert(ep_bridge_out != -1);
		io.inner.ep = ep_bridge_out;
		io.inner.flags = 0;
		io.inner.length = sizeof(io.data);

		VLOG("[BRIDGE] Waiting for data on ep#%d...\n", ep_bridge_out);
		int rv = usb_raw_ep_read(fd, (struct usb_raw_ep_io *)&io);
		if (rv < 0) {
			if (!keep_running) break;
			VLOG("[BRIDGE] Read error: %d, errno=%d\n", rv, errno);
			continue;
		}

		bool is_read = (rv == 6);

		VLOG("[BRIDGE] *** RECEIVED %d bytes ***\n", rv);
		if (rv >= 6) {
			struct sisusb_packet *pkt = (struct sisusb_packet *)io.inner.data;
			uint32_t result = process_packet_bridge(pkt, is_read);

			if (is_read && ep_bridge_in != -1) {
				io.inner.ep = ep_bridge_in;
				io.inner.length = sizeof(struct sisusb_packet);

				// write response data
				*(int *)(io.inner.data) = result;
				usb_raw_ep_write(fd, (struct usb_raw_ep_io *)&io);
			}
		}
	}

	VLOG("[THREAD] Bridge endpoint thread exiting\n");
	return NULL;
}

void *ep_gfx_loop(void *arg) {
	int fd = (int)(long)arg;
	struct usb_raw_bulk_io io;

	VLOG("[THREAD] GFX endpoint thread started\n");

	while (keep_running) {
		assert(ep_gfx_out != -1);
		io.inner.ep = ep_gfx_out;
		io.inner.flags = 0;
		io.inner.length = sizeof(io.data);

		VLOG("[GFX] Waiting for data on ep#%d...\n", ep_gfx_out);
		int rv = usb_raw_ep_read(fd, (struct usb_raw_ep_io *)&io);
		if (rv < 0) {
			if (!keep_running) break;
			VLOG("[GFX] Read error: %d, errno=%d\n", rv, errno);
			continue;
		}

		bool is_read = (rv == 6);

		VLOG("[GFX] *** RECEIVED %d bytes ***\n", rv);
		if (rv >= 6) {
			struct sisusb_packet *pkt = (struct sisusb_packet *)io.inner.data;
			uint32_t result = process_packet_gfx(pkt, is_read);

			if (is_read && ep_gfx_in != -1) {
				io.inner.ep = ep_gfx_in;
				io.inner.length = sizeof(struct sisusb_packet);

				// write response data
				*(int *)(io.inner.data) = result;
				usb_raw_ep_write(fd, (struct usb_raw_ep_io *)&io);
			}
		}
	}

	VLOG("[THREAD] GFX endpoint thread exiting\n");
	return NULL;
}

void *ep_bulk_loop(void *arg) {
	int fd = (int)(long)arg;
	struct usb_raw_bulk_io io;

	VLOG("[THREAD] Bulk endpoint (ep#%d) thread started\n", ep_gfx_bulk_out);

	while (keep_running) {
		assert(ep_gfx_bulk_out != -1);
		io.inner.ep = ep_gfx_bulk_out;
		io.inner.flags = 0;
		io.inner.length = sizeof(io.data);

		VLOG("[BULK] Waiting for data on ep#%d...\n", ep_gfx_bulk_out);
		int rv = usb_raw_ep_read(fd, (struct usb_raw_ep_io *)&io);
		if (rv < 0) {
			if (!keep_running) break;
			printf("[BULK] Read error: %d, errno=%d\n", rv, errno);
			continue;
		}

		// Write data to VRAM
		vram_bulk_write(bulk_state.address, (uint8_t *)io.inner.data, rv);

		// Update bulk state
		bulk_state.address += rv;
		bulk_state.length -= rv;

		// Reset bulk state
		if (bulk_state.configured && bulk_state.length == 0) {
			bulk_state.configured = false;
			bulk_state.address = 0;
			bulk_state.flags = 0;
			VLOG("   [BULK] Write data to VRAM OK\n");
		}
	}

	VLOG("[THREAD] Bulk endpoint thread exiting\n");
	return NULL;
}

void *ep_lbulk_loop(void *arg) {
	int fd = (int)(long)arg;
	struct usb_raw_bulk_io io;

	VLOG("[THREAD] LBulk endpoint (ep#%d) thread started\n", ep_gfx_lbulk_out);

	while (keep_running) {
		assert(ep_gfx_lbulk_out != -1);
		io.inner.ep = ep_gfx_lbulk_out;
		io.inner.flags = 0;
		io.inner.length = sizeof(io.data);

		VLOG("[LBULK] Waiting for data on ep#%d...\n", ep_gfx_lbulk_out);
		int rv = usb_raw_ep_read(fd, (struct usb_raw_ep_io *)&io);
		if (rv < 0) {
			if (!keep_running) break;
			printf("[LBULK] Read error: %d, errno=%d\n", rv, errno);
			continue;
		}

		// Write data to VRAM
		vram_bulk_write(bulk_state.address, (uint8_t *)io.inner.data, rv);

		// Update bulk state
		bulk_state.address += rv;
		bulk_state.length -= rv;

		// Reset bulk state
		if (bulk_state.configured && bulk_state.length == 0) {
			bulk_state.configured = false;
			bulk_state.address = 0;
			bulk_state.flags = 0;
			VLOG("   [LBULK] Write data to VRAM OK\n");
		}
	}

	VLOG("[THREAD] LBulk endpoint thread exiting\n");
	return NULL;
}

bool ep0_request(int fd, struct usb_raw_control_event *event,
				struct usb_raw_control_io *io) {
	switch (event->ctrl.bRequestType & USB_TYPE_MASK) {
	case USB_TYPE_STANDARD:
		switch (event->ctrl.bRequest) {
		case USB_REQ_GET_DESCRIPTOR:
			switch (event->ctrl.wValue >> 8) {
			case USB_DT_DEVICE:
				memcpy(&io->data[0], &usb_device,
							sizeof(usb_device));
				io->inner.length = sizeof(usb_device);
				return true;
			case USB_DT_CONFIG:
				io->inner.length =
					build_config(&io->data[0],
						sizeof(io->data));
				return true;
			case USB_DT_STRING:
				io->data[0] = 4;
				io->data[1] = USB_DT_STRING;
				if ((event->ctrl.wValue & 0xff) == 0) {
					io->data[2] = 0x09;
					io->data[3] = 0x04;
				} else {
					io->data[2] = 'S';
					io->data[3] = 0x00;
				}
				if (event->ctrl.wValue == 0x305)
					device_init = true;
				io->inner.length = 4;
				return true;
			default:
				printf("ep0: unknown descriptor\n");
				return false;
			}
			break;
		case USB_REQ_SET_CONFIGURATION:
			if (ep_gfx_out == -1) {
				ep_gfx_out = usb_raw_ep_enable(fd,
							&usb_endpoint_gfx_out);
				printf("ep0: gfx_out = ep#%d\n", ep_gfx_out);
			}
			if (ep_gfx_in == -1) {
				ep_gfx_in = usb_raw_ep_enable(fd,
							&usb_endpoint_gfx_in);
				printf("ep0: gfx_in = ep#%d\n", ep_gfx_in);
			}
			if (ep_gfx_bulk_out == -1) {
				ep_gfx_bulk_out = usb_raw_ep_enable(fd,
							&usb_endpoint_gfx_bulk_out);
				printf("ep0: gfx_bulk_out = ep#%d\n", ep_gfx_bulk_out);
			}
			if (ep_gfx_lbulk_out == -1) {
				ep_gfx_lbulk_out = usb_raw_ep_enable(fd,
							&usb_endpoint_gfx_lbulk_out);
				printf("ep0: gfx_lbulk_out = ep#%d\n", ep_gfx_lbulk_out);
			}
			if (ep_bridge_out == -1) {
				ep_bridge_out = usb_raw_ep_enable(fd,
							&usb_endpoint_bridge_out);
				printf("ep0: bridge_out = ep#%d\n", ep_bridge_out);
			}
			if (ep_bridge_in == -1) {
				ep_bridge_in = usb_raw_ep_enable(fd,
							&usb_endpoint_bridge_in);
				printf("ep0: bridge_in = ep#%d\n", ep_bridge_in);
			}

			pthread_create(&ep_bridge_thread, NULL, ep_bridge_loop,
					(void *)(long)fd);
			pthread_create(&ep_gfx_thread, NULL, ep_gfx_loop,
					(void *)(long)fd);
			pthread_create(&ep_bulk_thread, NULL, ep_bulk_loop,
					(void *)(long)fd);
			pthread_create(&ep_lbulk_thread, NULL, ep_lbulk_loop,
					(void *)(long)fd);

			usb_raw_vbus_draw(fd, usb_config.bMaxPower);
			usb_raw_configure(fd);
			io->inner.length = 0;
			return true;
		default:
			printf("ep0: unknown standard request\n");
			return false;
		}
		break;
	default:
		printf("ep0: unknown request type\n");
		return false;
	}
}

void log_control_request(struct usb_ctrlrequest *ctrl) {
	printf("  bRequestType: 0x%x (%s), bRequest: 0x%x, wValue: 0x%x,"
		" wIndex: 0x%x, wLength: %d\n", ctrl->bRequestType,
		(ctrl->bRequestType & USB_DIR_IN) ? "IN" : "OUT",
		ctrl->bRequest, ctrl->wValue, ctrl->wIndex, ctrl->wLength);

	switch (ctrl->bRequestType & USB_TYPE_MASK) {
	case USB_TYPE_STANDARD:
		printf("  type = USB_TYPE_STANDARD\n");
		switch (ctrl->bRequest) {
		case USB_REQ_GET_DESCRIPTOR:
			printf("  req = USB_REQ_GET_DESCRIPTOR\n");
			switch (ctrl->wValue >> 8) {
			case USB_DT_DEVICE:
				printf("  desc = USB_DT_DEVICE\n");
				break;
			case USB_DT_CONFIG:
				printf("  desc = USB_DT_CONFIG\n");
				break;
			case USB_DT_STRING:
				printf("  desc = USB_DT_STRING\n");
				break;
			case USB_DT_DEVICE_QUALIFIER:
				printf("  desc = USB_DT_DEVICE_QUALIFIER\n");
				break;
			default:
				printf("  desc = unknown = 0x%x\n", ctrl->wValue >> 8);
				break;
			}
			break;
		case USB_REQ_SET_CONFIGURATION:
			printf("  req = USB_REQ_SET_CONFIGURATION\n");
			break;
		default:
			printf("  req = unknown = 0x%x\n", ctrl->bRequest);
			break;
		}
		break;
	default:
		printf("  type = unknown = %d\n", (int)ctrl->bRequestType);
		break;
	}
}

void ep0_loop(int fd) {
	while(keep_running && !device_init) {
		struct usb_raw_control_event event;
		event.inner.type = 0;
		event.inner.length = sizeof(event.ctrl);

		usb_raw_event_fetch(fd, (struct usb_raw_event *)&event);
		log_event((struct usb_raw_event *)&event);

		if (event.inner.type == USB_RAW_EVENT_CONNECT) {
			process_eps_info(fd);
			continue;
		}

		if (event.inner.type != USB_RAW_EVENT_CONTROL)
			continue;

		struct usb_raw_control_io io;
		io.inner.ep = 0;
		io.inner.flags = 0;
		io.inner.length = 0;

		bool reply = ep0_request(fd, &event, &io);
		if (!reply) {
			printf("ep0: stalling\n");
			usb_raw_ep0_stall(fd);
			continue;
		}

		if (event.ctrl.wLength < io.inner.length)
			io.inner.length = event.ctrl.wLength;

		if (event.ctrl.bRequestType & USB_DIR_IN) {
			int rv = usb_raw_ep0_write(fd, (struct usb_raw_ep_io *)&io);
			printf("ep0: transferred %d bytes (in)\n", rv);
		} else {
			int rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
			printf("ep0: transferred %d bytes (out)\n", rv);
		}
	}

	while (1) {
		if (!main_running || overflow == false)
			break;
		sleep(1);
	}

	test_static_analyzer_warning();

	sleep(2);
}

/*----------------------------------------------------------------------*/
/* Main */
/*----------------------------------------------------------------------*/

int main(int argc, char **argv) {
	const char *device = "dummy_udc.0";
	const char *driver = "dummy_udc";
	if (argc >= 2)
		if (!strcmp(argv[1], "--verbose"))
			verbose = true;

	init_vram();

	int fd = usb_raw_open();
	usb_raw_init(fd, USB_SPEED_HIGH, driver, device);
	usb_raw_run(fd);

	ep0_loop(fd);

	if (vram) {
		free(vram);
		vram = NULL;
	}

	close(fd);

	return 0;
}
