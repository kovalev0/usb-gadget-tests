// SPDX-License-Identifier: Apache-2.0
//
// Emulates a CodeMercenaries IOWarrior40 device (VID: 0x07c0, PID: 0x1500).
// It uses USB 2.0 protocol over a HS connection.
// One interrupt IN endpoint is configured; the driver submits an INT URB
// on open() and reads port state via iowarrior_callback().
// Handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION), the HID SET_IDLE class request sent by probe,
// and HID SET_REPORT/GET_REPORT class requests used by write/ioctl.
//
// The emulator exercises the iowarrior driver via /dev/usb/iowarrior*:
// read(), write(), ioctl(IOW_READ), ioctl(IOW_WRITE), ioctl(IOW_GETINFO).
//
// Vasiliy Kovalev <kovalev@altlinux.org>

#include "../usb_gadget_tests.h"

/*----------------------------------------------------------------------*/

static volatile bool keep_running = true;

static pthread_t ep0_tid;

/*----------------------------------------------------------------------*/
/* iowarrior ioctl definitions */

//#include <linux/usb/iowarrior.h>
#define CODEMERCS_MAGIC_NUMBER	0xC0	/* like COde Mercenaries */

/* Define the ioctl commands for reading and writing data */
#define IOW_WRITE	_IOW(CODEMERCS_MAGIC_NUMBER, 1, __u8 *)
#define IOW_READ	_IOW(CODEMERCS_MAGIC_NUMBER, 2, __u8 *)

/*
   A struct for available device info which is read
   with the ioctl IOW_GETINFO.
   To be compatible with 2.4 userspace which didn't have an easy way to get
   this information.
*/
struct iowarrior_info {
	/* vendor id : supposed to be USB_VENDOR_ID_CODEMERCS in all cases */
	__u32 vendor;
	/* product id : depends on type of chip (USB_DEVICE_ID_CODEMERCS_X) */
	__u32 product;
	/* the serial number of our chip (if a serial-number is not available
	 * this is empty string) */
	__u8 serial[9];
	/* revision number of the chip */
	__u32 revision;
	/* USB-speed of the device (0=UNKNOWN, 1=LOW, 2=FULL 3=HIGH) */
	__u32 speed;
	/* power consumption of the device in mA */
	__u32 power;
	/* the number of the endpoint */
	__u32 if_num;
	/* size of the data-packets on this interface */
	__u32 report_size;
};

/*
  Get some device-information (product-id , serial-number etc.)
  in order to identify a chip.
*/
#define IOW_GETINFO _IOR(CODEMERCS_MAGIC_NUMBER, 3, struct iowarrior_info)

/*----------------------------------------------------------------------*/

#define REPORT_SIZE	4	// IOW40: 4 bytes of port data

// HID class requests
#define USB_REQ_SET_IDLE	0x0A
#define USB_REQ_GET_REPORT	0x01
#define USB_REQ_SET_REPORT	0x09

// Port state mirrored by the emulator
static unsigned char port_state[REPORT_SIZE] = { 0xAB, 0xCD, 0xEF, 0x01 };

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
		default:
			printf("  req = unknown = 0x%x\n", ctrl->bRequest);
			break;
		}
		break;
	case USB_TYPE_CLASS:
		printf("  type = USB_TYPE_CLASS\n");
		switch (ctrl->bRequest) {
		case USB_REQ_SET_IDLE:
			printf("  req = SET_IDLE\n");
			break;
		case USB_REQ_GET_REPORT:
			printf("  req = GET_REPORT, wValue = 0x%x\n",
				ctrl->wValue);
			break;
		case USB_REQ_SET_REPORT:
			printf("  req = SET_REPORT, wValue = 0x%x\n",
				ctrl->wValue);
			break;
		default:
			printf("  req = unknown class = 0x%x\n",
				ctrl->bRequest);
			break;
		}
		break;
	default:
		printf("  type = unknown = %d\n", (int)ctrl->bRequestType);
		break;
	}
}

/*----------------------------------------------------------------------*/
/* Device file test */
/*----------------------------------------------------------------------*/

static int find_device(char *devpath, size_t maxlen, int timeout_sec) {
	time_t start = time(NULL);

	while (difftime(time(NULL), start) < timeout_sec) {
		DIR *dir = opendir("/dev/usb");
		if (dir) {
			struct dirent *entry;
			while ((entry = readdir(dir)) != NULL) {
				if (strncmp(entry->d_name,
						"iowarrior", 9) == 0) {
					snprintf(devpath, maxlen,
						"/dev/usb/%s", entry->d_name);
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

void *test_thread(void *arg) {
	char devpath[512];

	printf("[TEST] Waiting for device...\n");

	if (find_device(devpath, sizeof(devpath), 15) != 0) {
		printf("[TEST] ERR: device not found\n");
		goto done;
	}
	printf("[TEST] Found\n");

	// iowarrior_open: submits int_in_urb
	int devfd = open(devpath, O_RDWR);
	if (devfd < 0) {
		printf("[TEST] ERR: open failed: %s\n",
							strerror(errno));
		goto done;
	}
	printf("[TEST] OK: open succeeded\n");

	// iowarrior_read: reads one report (REPORT_SIZE bytes) from the
	// queue filled by iowarrior_callback()
	unsigned char rbuf[REPORT_SIZE];
	int rv = read(devfd, rbuf, REPORT_SIZE);
	if (rv < 0)
		printf("[TEST] ERR: read failed: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: read %d bytes:"
			" 0x%02x 0x%02x 0x%02x 0x%02x\n", rv,
			rbuf[0], rbuf[1], rbuf[2], rbuf[3]);

	// iowarrior_write (IOW40 path): usb_set_report -> SET_REPORT OUT
	unsigned char wbuf[REPORT_SIZE] = { 0x11, 0x22, 0x33, 0x44 };
	rv = write(devfd, wbuf, REPORT_SIZE);
	if (rv < 0)
		printf("[TEST] ERR: write failed: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: write %d bytes\n", rv);

	// ioctl IOW_READ: usb_get_report -> GET_REPORT IN
	unsigned char ioc_rbuf[8] = { 0 };
	rv = ioctl(devfd, IOW_READ, ioc_rbuf);
	if (rv < 0)
		printf("[TEST] ERR: ioctl IOW_READ: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: IOW_READ:"
			" 0x%02x 0x%02x 0x%02x 0x%02x\n",
			ioc_rbuf[0], ioc_rbuf[1],
			ioc_rbuf[2], ioc_rbuf[3]);

	// ioctl IOW_WRITE: usb_set_report -> SET_REPORT OUT
	unsigned char ioc_wbuf[8] = { 0xAA, 0xBB, 0xCC, 0xDD, 0, 0, 0, 0 };
	rv = ioctl(devfd, IOW_WRITE, ioc_wbuf);
	if (rv < 0)
		printf("[TEST] ERR: ioctl IOW_WRITE: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: IOW_WRITE done\n");

	// ioctl IOW_GETINFO: fills iowarrior_info from device descriptor
	struct iowarrior_info info;
	memset(&info, 0, sizeof(info));
	rv = ioctl(devfd, IOW_GETINFO, &info);
	if (rv < 0)
		printf("[TEST] ERR: ioctl IOW_GETINFO: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: IOW_GETINFO vendor=0x%x"
			" product=0x%x report_size=%d serial=%.8s\n",
			info.vendor, info.product,
			info.report_size, info.serial);

	// iowarrior_release
	close(devfd);
	printf("[TEST] OK: closed\n");

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

#define USB_VENDOR		0x07c0	// Code Mercenaries GmbH
#define USB_PRODUCT		0x1500	// IOWarrior40

#define STRING_ID_MANUFACTURER	1
#define STRING_ID_PRODUCT	2
#define STRING_ID_SERIAL	3
#define STRING_ID_CONFIG	4
#define STRING_ID_INTERFACE	5

#define EP_MAX_PACKET_CONTROL	64
#define EP_MAX_PACKET_INT	REPORT_SIZE

// Assigned dynamically
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
	.wTotalLength =		0,  // computed later
	.bNumInterfaces =	1,
	.bConfigurationValue =	1,
	.iConfiguration =	STRING_ID_CONFIG,
	.bmAttributes =		USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
	.bMaxPower =		0x32,
};

struct usb_interface_descriptor usb_interface = {
	.bLength =		USB_DT_INTERFACE_SIZE,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bInterfaceNumber =	0,
	.bAlternateSetting =	0,
	.bNumEndpoints =	1,
	.bInterfaceClass =	USB_CLASS_HID,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	0,
	.iInterface =		STRING_ID_INTERFACE,
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

	assert(length >= sizeof(usb_config));
	memcpy(data, &usb_config, sizeof(usb_config));
	data += sizeof(usb_config);
	total_length += sizeof(usb_config);

	assert(length >= sizeof(usb_interface));
	memcpy(data, &usb_interface, sizeof(usb_interface));
	data += sizeof(usb_interface);
	total_length += sizeof(usb_interface);

	assert(length >= USB_DT_ENDPOINT_SIZE);
	memcpy(data, &usb_endpoint_int_in, USB_DT_ENDPOINT_SIZE);
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
	switch (usb_endpoint_type(ep)) {
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
		if (assign_ep_address(&info.eps[i], &usb_endpoint_int_in))
			continue;
	}

	assert(usb_endpoint_num(&usb_endpoint_int_in) != 0);
}

/*----------------------------------------------------------------------*/
/* Endpoint threads */
/*----------------------------------------------------------------------*/

#define EP0_MAX_DATA	256

struct usb_raw_control_event {
	struct usb_raw_event		inner;
	struct usb_ctrlrequest		ctrl;
};

struct usb_raw_control_io {
	struct usb_raw_ep_io		inner;
	char				data[EP0_MAX_DATA];
};

struct usb_raw_int_io {
	struct usb_raw_ep_io		inner;
	char				data[REPORT_SIZE];
};

int ep_int_in = -1;
pthread_t ep_int_in_thread;
atomic_bool ep_int_in_en = ATOMIC_VAR_INIT(false);

// ep_int_in_loop: serves INT IN URBs submitted by iowarrior_open() and
// re-submitted by iowarrior_callback(). Sends port_state periodically.
void *ep_int_in_loop(void *arg) {
	int fd = (int)(long)arg;

	struct usb_raw_int_io io;
	io.inner.ep    = ep_int_in;
	io.inner.flags = 0;
	io.inner.length = REPORT_SIZE;

	while (!atomic_load(&ep_int_in_en));

	while (keep_running) {
		memcpy(io.inner.data, port_state, REPORT_SIZE);

		int rv = usb_raw_ep_write_may_fail(fd,
					(struct usb_raw_ep_io *)&io);
		if (rv == io.inner.length) {
			// printf("ep_int_in: sent %d bytes\n", rv);
		} else if (rv < 0 && errno == ESHUTDOWN) {
			printf("ep_int_in: device was likely reset, exiting\n");
			break;
		} else if (rv < 0) {
			perror("usb_raw_ep_write_may_fail()");
			exit(EXIT_FAILURE);
		}
	}

	return NULL;
}

/*----------------------------------------------------------------------*/
/* EP0 */
/*----------------------------------------------------------------------*/

// Serial number: must be exactly 8 chars, otherwise probe clears it.
static void build_string_descriptor(struct usb_raw_control_io *io,
						const char *str) {
	int len = strlen(str);
	int total = 2 + len * 2;

	io->data[0] = total;
	io->data[1] = USB_DT_STRING;
	for (int i = 0; i < len; i++) {
		io->data[2 + i * 2]     = str[i];
		io->data[2 + i * 2 + 1] = 0x00;
	}
	io->inner.length = total;
}

pthread_t test_tid;
static bool test_started = false;

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
				if ((event->ctrl.wValue & 0xff) == 0) {
					io->data[0] = 4;
					io->data[1] = USB_DT_STRING;
					io->data[2] = 0x09;
					io->data[3] = 0x04;
					io->inner.length = 4;
				} else if ((event->ctrl.wValue & 0xff) ==
							STRING_ID_SERIAL) {
					// Exactly 8 chars required by probe
					build_string_descriptor(io, "ABCD1234");
				} else {
					io->data[0] = 4;
					io->data[1] = USB_DT_STRING;
					io->data[2] = 'A';
					io->data[3] = 0x00;
					io->inner.length = 4;
				}
				return true;
			default:
				printf("ep0: unknown descriptor\n");
				return false;
			}
			break;
		case USB_REQ_SET_CONFIGURATION:
			ep_int_in = usb_raw_ep_enable(fd,
						&usb_endpoint_int_in);
			printf("ep0: int_in = ep#%d\n", ep_int_in);

			int rv = pthread_create(&ep_int_in_thread, 0,
					ep_int_in_loop, (void *)(long)fd);
			if (rv != 0) {
				perror("pthread_create(ep_int_in)");
				exit(EXIT_FAILURE);
			}

			usb_raw_vbus_draw(fd, usb_config.bMaxPower);
			usb_raw_configure(fd);
			io->inner.length = 0;

			atomic_store(&ep_int_in_en, true);

			if (!test_started) {
				test_started = true;
				rv = pthread_create(&test_tid, NULL,
							test_thread, NULL);
				if (rv != 0) {
					perror("pthread_create(test)");
					exit(EXIT_FAILURE);
				}
			}
			return true;
		default:
			printf("ep0: unknown standard request\n");
			return false;
		}
		break;
	case USB_TYPE_CLASS:
		switch (event->ctrl.bRequest) {
		case USB_REQ_SET_IDLE:
			// Sent by probe for interface 0, just ACK
			io->inner.length = 0;
			return true;
		case USB_REQ_GET_REPORT:
			// IOW_READ ioctl: usb_get_report() -> IN control
			memcpy(io->data, port_state, REPORT_SIZE);
			io->inner.length = REPORT_SIZE;
			return true;
		case USB_REQ_SET_REPORT:
			// iowarrior_write / IOW_WRITE: usb_set_report() -> OUT
			// Data arrives in the OUT data phase
			io->inner.length = REPORT_SIZE;
			return true;
		default:
			printf("ep0: unknown class request 0x%02x\n",
				event->ctrl.bRequest);
			return false;
		}
		break;
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
			// Update port_state from SET_REPORT payload
			if ((event.ctrl.bRequestType & USB_TYPE_MASK) ==
					USB_TYPE_CLASS &&
			    event.ctrl.bRequest == USB_REQ_SET_REPORT &&
			    r == REPORT_SIZE)
				memcpy(port_state, io.data, REPORT_SIZE);
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

	usb_setup_signals(&ep0_tid);

	int fd = usb_raw_open();
	usb_raw_init(fd, USB_SPEED_HIGH, driver, device);
	usb_raw_run(fd);

	ep0_loop(fd);

	close(fd);

	return 0;
}
