// SPDX-License-Identifier: Apache-2.0
//
// Emulates a SiS USB-to-VGA adapter device (VID: 0x0711, PID: 0x0900),
// simulating device enumeration, PCI configuration space (header 0x008f),
// and bridge registers.
// It uses USB 2.0 protocol over a HIGH_SPEED connection.
// Six bulk endpoints are configured: GFX_OUT, GFX_IN, GFX_BULK_OUT,
// GFX_LBULK_OUT, BRIDGE_OUT, and BRIDGE_IN.
// Handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION).
//
// Successfully completes gfx device initialization (sisusb->devinit = 1):
// PCI BARs configured, Net2280 bridge initialized. Stops before graphics
// core initialization (sisusb_init_gfxcore), as VGA IO register emulation
// is not implemented.
//
// Vasiliy Kovalev <kovalev@altlinux.org>

#include "../usb_gadget_tests.h"

/*----------------------------------------------------------------------*/

static bool verbose = false;

#define VLOG(...) do { if (verbose) printf(__VA_ARGS__); } while(0)

static volatile bool keep_running = true;
static volatile bool device_init = false;

/*----------------------------------------------------------------------*/

#define PCI_CONFIG_SIZE		128
#define PCI_BRIDGE_REG_SIZE	1024

static uint32_t pci_config[PCI_CONFIG_SIZE]	= {0};
static uint32_t bridge_reg[PCI_BRIDGE_REG_SIZE]	= {0};

struct sisusb_packet {
	unsigned short header;
	uint32_t address;
	uint32_t data;
} __attribute__ ((__packed__));

/*----------------------------------------------------------------------*/

uint32_t process_packet_bridge(struct sisusb_packet *pkt, bool is_read) {
	uint16_t header = __le16_to_cpu(pkt->header);
	uint32_t address = __le32_to_cpu(pkt->address);
	uint32_t data = __le32_to_cpu(pkt->data);

	uint32_t result = 0;

	if (is_read) {
		if (address < sizeof(bridge_reg) / sizeof(bridge_reg[0])) {
			result = bridge_reg[address];
			VLOG("  Bridge READ: addr=0x%08x data=0x%08x\n", address, result);
		} else {
			printf("  WARNING: Bridge READ: addr=0x%08x is out of range 0x%08lx\n",
				address, sizeof(bridge_reg) / sizeof(bridge_reg[0]));
		}

	} else {
		if (address < sizeof(bridge_reg) / sizeof(bridge_reg[0])) {
			bridge_reg[address] = data;
			VLOG("  Bridge WRITE: addr=0x%08x data=0x%08x\n", address, data);
		} else {
			printf("  WARNING: Bridge WRITE: addr=0x%08x is out of range 0x%08lx\n",
				address, sizeof(bridge_reg) / sizeof(bridge_reg[0]));
		}

		// Check https://elixir.bootlin.com/linux/v6.12.66/source/drivers/usb/misc/sisusbvga/sisusbvga.c#L2130
		if (header == 0x001f && address == 0x00000050 && data == 0x000000ff)
			printf("Graphics device initialized\n");
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
	} else {
		printf("Skip graphics core initialization\n");
		// Stop and exit
		keep_running = false;
	}

	return result;
}

/*----------------------------------------------------------------------*/

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

			if (!ep_bridge_thread) {
				pthread_create(&ep_bridge_thread, 0,
					       ep_bridge_loop, (void *)(long)fd);
			}

			if (!ep_gfx_thread) {
				pthread_create(&ep_gfx_thread, 0,
					       ep_gfx_loop, (void *)(long)fd);
			}

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
		if (!keep_running)
			break;
		sleep(1);
	}
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

	int fd = usb_raw_open();
	usb_raw_init(fd, USB_SPEED_HIGH, driver, device);
	usb_raw_run(fd);

	ep0_loop(fd);

	close(fd);

	return 0;
}
