// SPDX-License-Identifier: Apache-2.0
//
// Emulates a Belkin F5U002 ISD-101 USS720 cable (VID: 0x05ab, PID: 0x0002).
// It uses USB 2.0 protocol over a HS connection.
// Three alternate settings are required by probe (num_altsetting == 3).
// Altsetting 2 provides two bulk endpoints (OUT ep1, IN ep2) and one
// optional interrupt IN endpoint.
// Handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION, SET_INTERFACE) and vendor requests:
//   request=4 (SET register, OUT, wLength=0),
//   request=3 (GET register, IN, 7 bytes).
//
// The emulator exercises the uss720 driver via parport and ppdev:
// PPCLAIM, PPFCONTROL, PPWDATA, PPRDATA, PPWCONTROL, PPRSTATUS,
// PPDATADIR, PPRELEASE.
//
// Vasiliy Kovalev <kovalev@altlinux.org>

#include "../usb_gadget_tests.h"

#include <linux/ppdev.h>

/*----------------------------------------------------------------------*/

static volatile bool keep_running = true;
static pthread_t ep0_tid;

static int ep_bulk_out = -1;
static int ep_bulk_in  = -1;

static pthread_t ep_bulk_out_thread;
static pthread_t ep_bulk_in_thread;
static atomic_bool ep_bulk_en = ATOMIC_VAR_INIT(false);

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
			default:
				printf("  desc = unknown = 0x%x\n",
							ctrl->wValue >> 8);
				break;
			}
			break;
		case USB_REQ_SET_CONFIGURATION:
			printf("  req = USB_REQ_SET_CONFIGURATION\n");
			break;
		case USB_REQ_SET_INTERFACE:
			printf("  req = USB_REQ_SET_INTERFACE,"
				" altsetting = %d\n", ctrl->wValue);
			break;
		default:
			printf("  req = unknown = 0x%x\n", ctrl->bRequest);
			break;
		}
		break;
	case USB_TYPE_VENDOR:
		printf("  type = USB_TYPE_VENDOR\n");
		switch (ctrl->bRequest) {
		case 3:
			printf("  req = GET_REGISTER, reg = 0x%x\n",
				ctrl->wValue >> 8);
			break;
		case 4:
			printf("  req = SET_REGISTER, reg = 0x%x,"
				" val = 0x%x\n",
				ctrl->wValue >> 8, ctrl->wValue & 0xff);
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
/* Parport test */
/*----------------------------------------------------------------------*/

static int find_parport(char *path, size_t maxlen, int timeout_sec) {
	time_t start = time(NULL);

	while (difftime(time(NULL), start) < timeout_sec) {
		DIR *dir = opendir("/proc/sys/dev/parport");
		if (dir) {
			struct dirent *entry;
			while ((entry = readdir(dir)) != NULL) {
				if (strncmp(entry->d_name,
						"parport", 7) == 0) {
					snprintf(path, maxlen,
						"/proc/sys/dev/parport/%s",
						entry->d_name);
					closedir(dir);
					return 0;
				}
			}
			closedir(dir);
		}
		usleep(200000);
	}
	return -1;
}

static int open_ppdev(const char *ppath) {
	const char *pname = strrchr(ppath, '/');
	pname = pname ? pname + 1 : ppath;

	char devpath[640];
	snprintf(devpath, sizeof(devpath), "/dev/%s", pname);

	// Wait up to 2 seconds for udev to create the device node
	for (int i = 0; i < 10 && access(devpath, F_OK) != 0; i++)
		usleep(200000);

	return open(devpath, O_RDWR);
}

void *test_thread(void *arg) {
	char ppath[512];

	printf("[TEST] Waiting for parport registration...\n");

	if (find_parport(ppath, sizeof(ppath), 15) != 0) {
		printf("[TEST] ERR: parport not found\n");
		goto done;
	}
	printf("[TEST] Found\n");

	int ppfd = open_ppdev(ppath);
	if (ppfd < 0) {
		printf("[TEST] ERR: ppdev open failed: %s\n",
							strerror(errno));
		goto done;
	}
	printf("[TEST] OK: ppdev open\n");

	if (ioctl(ppfd, PPEXCL) < 0)
		printf("[TEST] note: PPEXCL: %s\n", strerror(errno));
	if (ioctl(ppfd, PPCLAIM) < 0) {
		printf("[TEST] ERR: PPCLAIM: %s\n", strerror(errno));
		close(ppfd);
		goto done;
	}
	printf("[TEST] OK: PPCLAIM\n");

	// parport_uss720_frob_control -> set_1284_register(reg=2)
	struct ppdev_frob_struct frob = { .mask = 0x0f, .val = 0x05 };
	if (ioctl(ppfd, PPFCONTROL, &frob) < 0)
		printf("[TEST] ERR: PPFCONTROL: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: frob_control\n");

	// parport_uss720_write_data -> set_1284_register(reg=0)
	unsigned char wdata = 0x55;
	if (ioctl(ppfd, PPWDATA, &wdata) < 0)
		printf("[TEST] ERR: PPWDATA: %s\n", strerror(errno));
	else
		printf("[TEST] OK: write_data\n");

	// parport_uss720_read_data -> get_1284_register(reg=0)
	unsigned char rdata = 0;
	if (ioctl(ppfd, PPRDATA, &rdata) < 0)
		printf("[TEST] ERR: PPRDATA: %s\n", strerror(errno));
	else
		printf("[TEST] OK: read_data = 0x%02x\n", rdata);

	// parport_uss720_enable_irq -> set_1284_register(reg=2, val|=0x10)
	unsigned char ctrl_irq_en = 0x10;
	if (ioctl(ppfd, PPWCONTROL, &ctrl_irq_en) < 0)
		printf("[TEST] ERR: PPWCONTROL (enable_irq): %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: enable_irq\n");

	// parport_uss720_disable_irq -> set_1284_register(reg=2, val&=~0x10)
	unsigned char ctrl_irq_dis = 0x04;
	if (ioctl(ppfd, PPWCONTROL, &ctrl_irq_dis) < 0)
		printf("[TEST] ERR: PPWCONTROL (disable_irq): %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: disable_irq\n");

	// parport_uss720_read_status -> get_1284_register(reg=1)
	unsigned char status = 0;
	if (ioctl(ppfd, PPRSTATUS, &status) < 0)
		printf("[TEST] ERR: PPRSTATUS: %s\n", strerror(errno));
	else
		printf("[TEST] OK: read_status = 0x%02x\n", status);

	// parport_uss720_data_forward -> set_1284_register(reg=2, val&=~0x20)
	int dir = 0;
	if (ioctl(ppfd, PPDATADIR, &dir) < 0)
		printf("[TEST] ERR: PPDATADIR(forward): %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: data_forward\n");

	// parport_uss720_data_reverse -> set_1284_register(reg=2, val|=0x20)
	dir = 1;
	if (ioctl(ppfd, PPDATADIR, &dir) < 0)
		printf("[TEST] ERR: PPDATADIR(reverse): %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: data_reverse\n");

	// parport_uss720_restore_state called by parport_release
	if (ioctl(ppfd, PPRELEASE) < 0)
		printf("[TEST] ERR: PPRELEASE: %s\n", strerror(errno));
	else
		printf("[TEST] OK: PPRELEASE\n");

	// parport_uss720_save_state called by parport_claim
	if (ioctl(ppfd, PPCLAIM) == 0) {
		printf("[TEST] OK: PPCLAIM\n");
		ioctl(ppfd, PPRELEASE);
	}

	close(ppfd);
	printf("[TEST] Done\n");

done:
	keep_running = false;
	alarm(5);
	pthread_kill(ep0_tid, SIGUSR1);
	return NULL;
}

/*----------------------------------------------------------------------*/
/* USB device descriptors */
/*----------------------------------------------------------------------*/

#define BCD_USB			0x0200

#define USB_VENDOR		0x05ab	// Belkin
#define USB_PRODUCT		0x0002	// F5U002 ISD-101

#define STRING_ID_MANUFACTURER	1
#define STRING_ID_PRODUCT	2
#define STRING_ID_SERIAL	3
#define STRING_ID_CONFIG	4
#define STRING_ID_INTERFACE	5

#define EP_MAX_PACKET_CONTROL	64
#define EP_MAX_PACKET_BULK	512
#define EP_MAX_PACKET_INT	8

#define EP_NUM_BULK_OUT		0x0
#define EP_NUM_BULK_IN		0x0
#define EP_NUM_INT_IN		0x0

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
	.iConfiguration =	STRING_ID_CONFIG,
	.bmAttributes =		USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
	.bMaxPower =		0x32,
};

struct usb_interface_descriptor usb_iface_alt0 = {
	.bLength =		USB_DT_INTERFACE_SIZE,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bInterfaceNumber =	0,
	.bAlternateSetting =	0,
	.bNumEndpoints =	0,
	.bInterfaceClass =	0,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	0,
	.iInterface =		0,
};

struct usb_interface_descriptor usb_iface_alt1 = {
	.bLength =		USB_DT_INTERFACE_SIZE,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bInterfaceNumber =	0,
	.bAlternateSetting =	1,
	.bNumEndpoints =	0,
	.bInterfaceClass =	0,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	0,
	.iInterface =		0,
};

struct usb_interface_descriptor usb_iface_alt2 = {
	.bLength =		USB_DT_INTERFACE_SIZE,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bInterfaceNumber =	0,
	.bAlternateSetting =	2,
	.bNumEndpoints =	3,
	.bInterfaceClass =	0,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	0,
	.iInterface =		0,
};

struct usb_endpoint_descriptor usb_endpoint_bulk_out = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_OUT | EP_NUM_BULK_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	EP_MAX_PACKET_BULK,
};

struct usb_endpoint_descriptor usb_endpoint_bulk_in = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN | EP_NUM_BULK_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	EP_MAX_PACKET_BULK,
};

struct usb_endpoint_descriptor usb_endpoint_int_in = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN | EP_NUM_INT_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	EP_MAX_PACKET_INT,
	.bInterval =		10,
};

/*----------------------------------------------------------------------*/
/* Configuration builder */
/*----------------------------------------------------------------------*/

int build_config(char *data, int length) {
	struct usb_config_descriptor *config =
		(struct usb_config_descriptor *)data;
	int total_length = 0;

#define APPEND(src, sz) do { \
	assert(length >= (int)(sz)); \
	memcpy(data, (src), (sz)); \
	data += (sz); length -= (sz); total_length += (sz); \
} while (0)

	APPEND(&usb_config, sizeof(usb_config));
	APPEND(&usb_iface_alt0, sizeof(usb_iface_alt0));
	APPEND(&usb_iface_alt1, sizeof(usb_iface_alt1));
	APPEND(&usb_iface_alt2, sizeof(usb_iface_alt2));
	APPEND(&usb_endpoint_bulk_out, USB_DT_ENDPOINT_SIZE);
	APPEND(&usb_endpoint_bulk_in,  USB_DT_ENDPOINT_SIZE);
	APPEND(&usb_endpoint_int_in,   USB_DT_ENDPOINT_SIZE);

#undef APPEND

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
	switch (usb_endpoint_type(ep)) {
	case USB_ENDPOINT_XFER_BULK:
		if (!info->caps.type_bulk)
			return false;
		break;
	case USB_ENDPOINT_XFER_INT:
		if (!info->caps.type_int)
			return false;
		break;
	default:
		assert(false);
	}
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
		if (assign_ep_address(&info.eps[i], &usb_endpoint_bulk_out))
			continue;
		if (assign_ep_address(&info.eps[i], &usb_endpoint_bulk_in))
			continue;
		if (assign_ep_address(&info.eps[i], &usb_endpoint_int_in))
			continue;
	}

	assert(usb_endpoint_num(&usb_endpoint_bulk_out) != 0);
	assert(usb_endpoint_num(&usb_endpoint_bulk_in)  != 0);
	assert(usb_endpoint_num(&usb_endpoint_int_in)   != 0);
}

/*----------------------------------------------------------------------*/
/* Bulk endpoint threads */
/*----------------------------------------------------------------------*/

#define BULK_BUF	512

struct usb_raw_bulk_io {
	struct usb_raw_ep_io		inner;
	char				data[BULK_BUF];
};

// ep_bulk_out_loop: drains bulk OUT transfers
void *ep_bulk_out_loop(void *arg) {
	int fd = (int)(long)arg;

	struct usb_raw_bulk_io io;
	io.inner.ep    = ep_bulk_out;
	io.inner.flags = 0;

	while (!atomic_load(&ep_bulk_en));

	while (keep_running) {
		io.inner.length = BULK_BUF;

		int rv = ioctl(fd, USB_RAW_IOCTL_EP_READ, &io);
		if (rv < 0) {
			if (errno == ESHUTDOWN)
				break;
			perror("ep_bulk_out: ioctl(EP_READ)");
			exit(EXIT_FAILURE);
		}
		printf("ep_bulk_out: received %d bytes\n", rv);
	}

	return NULL;
}

// ep_bulk_in_loop: provides data for bulk IN transfers
void *ep_bulk_in_loop(void *arg) {
	int fd = (int)(long)arg;

	struct usb_raw_bulk_io io;
	io.inner.ep    = ep_bulk_in;
	io.inner.flags = 0;

	while (!atomic_load(&ep_bulk_en));

	while (keep_running) {
		memset(io.inner.data, 0x55, 4);
		io.inner.length = 4;

		int rv = usb_raw_ep_write_may_fail(fd,
					(struct usb_raw_ep_io *)&io);
		if (rv < 0 && errno == ESHUTDOWN)
			break;
		if (rv < 0) {
			perror("ep_bulk_in: write_may_fail");
			exit(EXIT_FAILURE);
		}
	}

	return NULL;
}

/*----------------------------------------------------------------------*/
/* EP0 */
/*----------------------------------------------------------------------*/

#define EP0_MAX_DATA	512

struct usb_raw_control_event {
	struct usb_raw_event		inner;
	struct usb_ctrlrequest		ctrl;
};

struct usb_raw_control_io {
	struct usb_raw_ep_io		inner;
	char				data[EP0_MAX_DATA];
};

// Emulated USS720 register file: 7 bytes returned by GET request.
// SET_REGISTER writes to regindex[reg] to keep GET_REGISTER consistent
// with the driver's regindex[] mapping: {4, 0, 1, 5, 5, 0, 2, 3, 6}.
static uint8_t uss720_regs[7] = {
	0x00,	// pos 0: SPP status
	0x0c,	// pos 1: SPP control
	0x00,	// pos 2: ECR
	0x00,	// pos 3: EPP addr
	0x00,	// pos 4: SPP data
	0x00,	// pos 5: EPP data
	0x00,	// pos 6: ECP FIFO
};

// regindex table from the driver (maps reg number → buffer position)
static const uint8_t regindex[9] = { 4, 0, 1, 5, 5, 0, 2, 3, 6 };

pthread_t test_tid;
static bool test_started = false;
static bool altsetting2_active = false;

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
					io->data[2] = 'A';
					io->data[3] = 0x00;
				}
				io->inner.length = 4;
				return true;
			default:
				printf("ep0: unknown descriptor\n");
				return false;
			}
			break;
		case USB_REQ_SET_CONFIGURATION:
			usb_raw_vbus_draw(fd, usb_config.bMaxPower);
			usb_raw_configure(fd);
			io->inner.length = 0;
			return true;
		case USB_REQ_SET_INTERFACE: {
			int altsetting = event->ctrl.wValue;
			printf("ep0: SET_INTERFACE altsetting=%d\n",
								altsetting);
			if (altsetting == 2 && !altsetting2_active) {
				altsetting2_active = true;
				ep_bulk_out = usb_raw_ep_enable(fd,
						&usb_endpoint_bulk_out);
				ep_bulk_in  = usb_raw_ep_enable(fd,
						&usb_endpoint_bulk_in);
				usb_raw_ep_enable(fd, &usb_endpoint_int_in);
				printf("ep0: altsetting 2 endpoints"
					" (bulk_out=#%d bulk_in=#%d)\n",
					ep_bulk_out, ep_bulk_in);

				int rv = pthread_create(&ep_bulk_out_thread, 0,
						ep_bulk_out_loop,
						(void *)(long)fd);
				if (rv != 0) {
					perror("pthread_create(ep_bulk_out)");
					exit(EXIT_FAILURE);
				}
				rv = pthread_create(&ep_bulk_in_thread, 0,
						ep_bulk_in_loop,
						(void *)(long)fd);
				if (rv != 0) {
					perror("pthread_create(ep_bulk_in)");
					exit(EXIT_FAILURE);
				}
				atomic_store(&ep_bulk_en, true);

				if (!test_started) {
					test_started = true;
					rv = pthread_create(&test_tid,
							NULL, test_thread,
							NULL);
					if (rv != 0) {
						perror("pthread_create(test)");
						exit(EXIT_FAILURE);
					}
				}
			}
			io->inner.length = 0;
			return true;
		}
		default:
			printf("ep0: unknown standard request\n");
			return false;
		}
		break;
	case USB_TYPE_VENDOR:
		switch (event->ctrl.bRequest) {
		case 4: {
			// SET_REGISTER: write to regindex[reg] position
			uint8_t reg = (event->ctrl.wValue >> 8) & 0xff;
			uint8_t val = event->ctrl.wValue & 0xff;
			if (reg < 9)
				uss720_regs[regindex[reg]] = val;
			io->inner.length = 0;
			return true;
		}
		case 3:
			// GET_REGISTER: return all 7 bytes of register state
			memcpy(io->data, uss720_regs, sizeof(uss720_regs));
			io->inner.length = sizeof(uss720_regs);
			return true;
		default:
			printf("ep0: unknown vendor req 0x%02x\n",
				event->ctrl.bRequest);
			return false;
		}
	default:
		printf("ep0: unknown request type\n");
		return false;
	}
}

void ep0_loop(int fd) {
	while (true) {
		struct usb_raw_control_event event;
		event.inner.type   = 0;
		event.inner.length = sizeof(event.ctrl);

		int rv = ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, &event);
		if (rv < 0) {
			if (errno == EINTR && !keep_running)
				break;
			if (errno == EINTR)
				continue;
			perror("ioctl(USB_RAW_IOCTL_EVENT_FETCH)");
			exit(EXIT_FAILURE);
		}

		if (!test_started)
			log_event((struct usb_raw_event *)&event);

		if (event.inner.type == USB_RAW_EVENT_CONNECT) {
			process_eps_info(fd);
			continue;
		}

		if (event.inner.type == USB_RAW_EVENT_SUSPEND ||
		    event.inner.type == USB_RAW_EVENT_RESUME  ||
		    event.inner.type == USB_RAW_EVENT_RESET)
			continue;

		if (event.inner.type == USB_RAW_EVENT_DISCONNECT)
			break;

		if (event.inner.type != USB_RAW_EVENT_CONTROL)
			continue;

		struct usb_raw_control_io io;
		io.inner.ep     = 0;
		io.inner.flags  = 0;
		io.inner.length = 0;

		bool reply = ep0_request(fd, &event, &io);
		if (!reply) {
			if (!test_started)
				printf("ep0: stalling\n");
			usb_raw_ep0_stall(fd);
			continue;
		}

		if (event.ctrl.wLength < io.inner.length)
			io.inner.length = event.ctrl.wLength;

		if (event.ctrl.bRequestType & USB_DIR_IN) {
			int r = usb_raw_ep0_write(fd,
					(struct usb_raw_ep_io *)&io);
			if (!test_started)
				printf("ep0: transferred %d bytes (in)\n", r);
		} else {
			int r = usb_raw_ep0_read(fd,
					(struct usb_raw_ep_io *)&io);
			if (!test_started)
				printf("ep0: transferred %d bytes (out)\n", r);
		}
	}

	if (test_started)
		pthread_join(test_tid, NULL);
}

/*----------------------------------------------------------------------*/
/* Main */
/*----------------------------------------------------------------------*/

int main(int argc, char **argv) {
	const char *device = "dummy_udc.0";
	const char *driver = "dummy_udc";

	int r = system("modprobe parport 2>/dev/null");
	(void)r;

        r = system("modprobe ppdev 2>/dev/null");
        (void)r;

	usb_setup_signals(&ep0_tid);

	int fd = usb_raw_open();
	usb_raw_init(fd, USB_SPEED_HIGH, driver, device);
	usb_raw_run(fd);

	ep0_loop(fd);

	close(fd);

	return 0;
}
