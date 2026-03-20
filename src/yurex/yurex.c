// SPDX-License-Identifier: Apache-2.0
//
// Emulates a Meywa-Denki & KAYAC YUREX device (VID: 0x0c45, PID: 0x1010).
// It uses USB 2.0 protocol over a HS connection.
// One interrupt IN endpoint is configured.
// Handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION) and HID SET_REPORT class OUT requests used by
// yurex_write() to send commands.
//
// Command flow:
//   yurex_write() submits cntl_urb (HID SET_REPORT OUT on EP0),
//   then waits on waitq for CMD_ACK via interrupt IN.
//   ep0_loop signals ep_int_in_loop which sends the ACK packet,
//   then sends a CMD_READ packet with BBU counter data.
//
// The emulator exercises the yurex driver via /dev/yurex*:
// open(), read(), write(CMD_READ), write(CMD_SET), write(CMD_VERSION),
// write(CMD_LED), release().
//
// Vasiliy Kovalev <kovalev@altlinux.org>

#include "../usb_gadget_tests.h"

/*----------------------------------------------------------------------*/

static volatile bool keep_running = true;

static pthread_t ep0_tid;

/*----------------------------------------------------------------------*/

#define CMD_ACK		'!'
#define CMD_COUNT	'C'
#define CMD_LED		'L'
#define CMD_READ	'R'
#define CMD_SET		'S'
#define CMD_VERSION	'V'
#define CMD_EOF		0x0d
#define CMD_PADDING	0xff

#define YUREX_BUF_SIZE	8

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
		printf("  req = 0x%x (HID SET_REPORT)\n", ctrl->bRequest);
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
		DIR *dir = opendir("/dev");
		if (dir) {
			struct dirent *entry;
			while ((entry = readdir(dir)) != NULL) {
				if (strncmp(entry->d_name, "yurex", 5) == 0) {
					snprintf(devpath, maxlen,
						"/dev/%s", entry->d_name);
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
	char buf[32];

	printf("[TEST] Waiting for device...\n");

	if (find_device(devpath, sizeof(devpath), 15) != 0) {
		printf("[TEST] ERR: device not found\n");
		goto done;
	}
	printf("[TEST] Found\n");

	// yurex_open: kref_get
	int devfd = open(devpath, O_RDWR);
	if (devfd < 0) {
		printf("[TEST] ERR: open failed: %s\n",
							strerror(errno));
		goto done;
	}
	printf("[TEST] OK: open succeeded\n");

	// yurex_read: returns bbu as string (initial = -1)
	int rv = read(devfd, buf, sizeof(buf) - 1);
	if (rv < 0)
		printf("[TEST] ERR: read failed: %s\n",
							strerror(errno));
	else {
		buf[rv] = '\0';
		printf("[TEST] OK: initial bbu = %s", buf);
	}

	// yurex_write CMD_READ: cntl_urb OUT, waits for CMD_ACK via INT IN,
	// then ep_int_in_loop sends CMD_READ packet to update bbu
	rv = write(devfd, "R\n", 2);
	if (rv < 0)
		printf("[TEST] ERR: write CMD_READ: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: write CMD_READ (%d bytes)\n", rv);

	// bbu should now be updated by CMD_READ INT IN packet
	lseek(devfd, 0, SEEK_SET);
	rv = read(devfd, buf, sizeof(buf) - 1);
	if (rv < 0)
		printf("[TEST] ERR: read after CMD_READ: %s\n",
							strerror(errno));
	else {
		buf[rv] = '\0';
		printf("[TEST] OK: bbu after CMD_READ = %s", buf);
	}

	// yurex_write CMD_VERSION: same flow, ACK expected
	rv = write(devfd, "V\n", 2);
	if (rv < 0)
		printf("[TEST] ERR: write CMD_VERSION: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: write CMD_VERSION (%d bytes)\n", rv);

	// yurex_write CMD_LED
	rv = write(devfd, "L1\n", 3);
	if (rv < 0)
		printf("[TEST] ERR: write CMD_LED: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: write CMD_LED (%d bytes)\n", rv);

	// yurex_write CMD_SET with value 12345 (sets bbu on success + timeout)
	rv = write(devfd, "S12345\n", 7);
	if (rv < 0)
		printf("[TEST] ERR: write CMD_SET: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: write CMD_SET (%d bytes)\n", rv);

	// yurex_write invalid command -> -EINVAL
	rv = write(devfd, "?\n", 2);
	if (rv < 0 && errno == EINVAL)
		printf("[TEST] OK: invalid cmd returned EINVAL"
			" (expected)\n");
	else
		printf("[TEST] ERR: unexpected result for invalid"
			" cmd: %d\n", rv);

	// yurex_release: kref_put
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

#define USB_VENDOR		0x0c45	// Microdia / Sonix
#define USB_PRODUCT		0x1010	// YUREX

#define STRING_ID_MANUFACTURER	1
#define STRING_ID_PRODUCT	2
#define STRING_ID_SERIAL	3
#define STRING_ID_CONFIG	4
#define STRING_ID_INTERFACE	5

#define EP_MAX_PACKET_CONTROL	64
#define EP_MAX_PACKET_INT	YUREX_BUF_SIZE

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
	.bInterval =		1,
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
	char				data[YUREX_BUF_SIZE];
};

int ep_int_in = -1;
pthread_t ep_int_in_thread;
atomic_bool ep_int_in_en = ATOMIC_VAR_INIT(false);

// Signalled by ep0_loop each time a HID SET_REPORT is received.
// Carries the command byte so ep_int_in_loop knows what ACK to send.
static atomic_int pending_cmd = ATOMIC_VAR_INIT(0);
static atomic_bool cmd_pending = ATOMIC_VAR_INIT(false);

static void send_int_packet(int fd, struct usb_raw_int_io *io) {
	int rv = usb_raw_ep_write_may_fail(fd, (struct usb_raw_ep_io *)io);
	if (rv < 0 && errno == ESHUTDOWN)
		return;
	if (rv < 0) {
		perror("usb_raw_ep_write_may_fail()");
		exit(EXIT_FAILURE);
	}
}

// ep_int_in_loop:
//   - On probe: the driver submits the INT URB immediately, so we keep
//     the endpoint ready but don't send until a command arrives.
//   - For each HID SET_REPORT received on EP0 (cmd_pending set by ep0_loop):
//     1. Send CMD_ACK packet -> wakes driver's waitq
//     2. If command was CMD_READ/CMD_COUNT: also send a data packet with
//        BBU counter so yurex_interrupt updates dev->bbu
void *ep_int_in_loop(void *arg) {
	int fd = (int)(long)arg;

	struct usb_raw_int_io io;
	io.inner.ep    = ep_int_in;
	io.inner.flags = 0;
	io.inner.length = YUREX_BUF_SIZE;

	while (!atomic_load(&ep_int_in_en));

	// Wait for the first URB submission from probe, then sit idle.
	// We only send data in response to commands from ep0_loop.
	while (keep_running) {
		if (!atomic_load(&cmd_pending)) {
			usleep(1000);
			continue;
		}

		int cmd = atomic_load(&pending_cmd);
		atomic_store(&cmd_pending, false);

		// 1. Send CMD_ACK — wakes driver's waitq after SET_REPORT
		memset(io.inner.data, CMD_PADDING, YUREX_BUF_SIZE);
		io.inner.data[0] = CMD_ACK;
		io.inner.data[1] = (char)cmd;
		send_int_packet(fd, &io);
		// printf("ep_int_in: sent CMD_ACK for '%c'\n", cmd);

		// 2. For read-type commands, send a BBU data packet
		if (cmd == CMD_READ || cmd == CMD_COUNT ||
		    cmd == CMD_VERSION) {
			memset(io.inner.data, CMD_PADDING, YUREX_BUF_SIZE);
			io.inner.data[0] = CMD_READ;
			// BBU = 0x0000004D2 = 1234
			io.inner.data[1] = 0x00;
			io.inner.data[2] = 0x00;
			io.inner.data[3] = 0x00;
			io.inner.data[4] = 0x04;
			io.inner.data[5] = 0xD2;
			io.inner.data[6] = CMD_EOF;
			io.inner.data[7] = CMD_PADDING;
			send_int_packet(fd, &io);
			// printf("ep_int_in: sent CMD_READ (bbu=1234)\n");
		}
	}

	return NULL;
}

/*----------------------------------------------------------------------*/
/* EP0 */
/*----------------------------------------------------------------------*/

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
		// HID SET_REPORT OUT: driver sends command via cntl_urb.
		// Read the 8-byte payload, extract command byte, signal
		// ep_int_in_loop to send CMD_ACK.
		io->inner.length = YUREX_BUF_SIZE;
		return true;
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

			// Signal ep_int_in_loop with the command byte
			if ((event.ctrl.bRequestType & USB_TYPE_MASK) ==
					USB_TYPE_CLASS && r > 0) {
				atomic_store(&pending_cmd,
					(unsigned char)io.data[0]);
				atomic_store(&cmd_pending, true);
			}
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
