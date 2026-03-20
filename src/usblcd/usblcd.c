// SPDX-License-Identifier: Apache-2.0
//
// Emulates a USBLCD device (VID: 0x10d2, PID: 0x0001).
// It uses USB 2.0 protocol over a HS connection.
// One bulk IN and one bulk OUT endpoint are configured.
// Handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION).
//
// The emulator exercises the usblcd driver via /dev/lcd*:
// write(), read(), ioctl(IOCTL_GET_HARD_VERSION),
// ioctl(IOCTL_GET_DRV_VERSION).
//
// Vasiliy Kovalev <kovalev@altlinux.org>

#include "../usb_gadget_tests.h"

/*----------------------------------------------------------------------*/

static volatile bool keep_running = true;

static pthread_t ep0_tid;

/*----------------------------------------------------------------------*/

#define IOCTL_GET_HARD_VERSION	1
#define IOCTL_GET_DRV_VERSION	2

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
				if (strncmp(entry->d_name, "lcd", 3) == 0) {
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

// Signal from ep_bulk_out_loop to test_thread that a write was received.
static atomic_bool write_received = ATOMIC_VAR_INIT(false);

// Signal from test_thread to ep_bulk_in_loop that lcd_read() is about
// to call usb_bulk_msg — the bulk IN URB is now submitted and waiting.
static atomic_bool read_requested = ATOMIC_VAR_INIT(false);

void *test_thread(void *arg) {
	char devpath[512];
	char buf[64];

	printf("[TEST] Waiting for device...\n");

	if (find_device(devpath, sizeof(devpath), 15) != 0) {
		printf("[TEST] ERR: device not found\n");
		goto done;
	}
	printf("[TEST] Found\n");

	// lcd_open: kref_get + usb_autopm_get_interface
	int devfd = open(devpath, O_RDWR);
	if (devfd < 0) {
		printf("[TEST] ERR: open failed: %s\n",
							strerror(errno));
		goto done;
	}
	printf("[TEST] OK: open succeeded\n");

	// lcd_ioctl IOCTL_GET_HARD_VERSION: bcdDevice formatted as "XXYY"
	memset(buf, 0, sizeof(buf));
	if (ioctl(devfd, IOCTL_GET_HARD_VERSION, buf) < 0)
		printf("[TEST] ERR: IOCTL_GET_HARD_VERSION: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: IOCTL_GET_HARD_VERSION\n");
		// printf("[TEST] OK: HARD_VERSION = \"%s\"\n", buf);

	// lcd_ioctl IOCTL_GET_DRV_VERSION: returns DRIVER_VERSION string
	memset(buf, 0, sizeof(buf));
	if (ioctl(devfd, IOCTL_GET_DRV_VERSION, buf) < 0)
		printf("[TEST] ERR: IOCTL_GET_DRV_VERSION: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: IOCTL_GET_DRV_VERSION\n");
		// printf("[TEST] OK: DRV_VERSION = \"%s\"\n", buf);

	// lcd_write: async bulk OUT URB — driver submits urb and returns
	// immediately; ep_bulk_out_loop receives the data
	const char *cmd = "Hello LCD!";
	int rv = write(devfd, cmd, strlen(cmd));
	if (rv < 0)
		printf("[TEST] ERR: write failed: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: write %d bytes\n", rv);

	// Wait for ep_bulk_out_loop to receive the write
	while (!atomic_load(&write_received));
	atomic_store(&write_received, false);

	// Signal ep_bulk_in_loop that lcd_read() is about to submit the
	// bulk IN URB. This must be set BEFORE read() so there is no race
	// between the IN data arriving and the URB being submitted.
	atomic_store(&read_requested, true);

	// lcd_read: blocking usb_bulk_msg from bulk IN
	// ep_bulk_in_loop will send data after write_received was consumed
	unsigned char rbuf[64] = { 0 };
	rv = read(devfd, rbuf, sizeof(rbuf));
	if (rv < 0)
		printf("[TEST] ERR: read failed: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: read %d bytes: \"%.*s\"\n",
							rv, rv, rbuf);

	// lcd_release: usb_autopm_put_interface + kref_put
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
#define BCD_DEVICE		0x0102	// 01.02 -> HARD_VERSION = "0102"

#define USB_VENDOR		0x10d2	// USBLCD
#define USB_PRODUCT		0x0001	// required by probe

#define STRING_ID_MANUFACTURER	1
#define STRING_ID_PRODUCT	2
#define STRING_ID_SERIAL	3
#define STRING_ID_CONFIG	4
#define STRING_ID_INTERFACE	5

#define EP_MAX_PACKET_CONTROL	64
#define EP_MAX_PACKET_BULK	512

#define EP_NUM_BULK_IN		0x0
#define EP_NUM_BULK_OUT		0x0

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
	.bcdDevice =		__constant_cpu_to_le16(BCD_DEVICE),
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
	.bNumEndpoints =	2,
	.bInterfaceClass =	0,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	0,
	.iInterface =		STRING_ID_INTERFACE,
};

struct usb_endpoint_descriptor usb_endpoint_bulk_in = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN | EP_NUM_BULK_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	EP_MAX_PACKET_BULK,
};

struct usb_endpoint_descriptor usb_endpoint_bulk_out = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_OUT | EP_NUM_BULK_OUT,
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

	assert(length >= USB_DT_ENDPOINT_SIZE);
	memcpy(data, &usb_endpoint_bulk_in, USB_DT_ENDPOINT_SIZE);
	data += USB_DT_ENDPOINT_SIZE;
	total_length += USB_DT_ENDPOINT_SIZE;

	assert(length >= USB_DT_ENDPOINT_SIZE);
	memcpy(data, &usb_endpoint_bulk_out, USB_DT_ENDPOINT_SIZE);
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
	case USB_ENDPOINT_XFER_BULK:
		if (!info->caps.type_bulk)
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
		if (assign_ep_address(&info.eps[i], &usb_endpoint_bulk_in))
			continue;
		if (assign_ep_address(&info.eps[i], &usb_endpoint_bulk_out))
			continue;
	}

	assert(usb_endpoint_num(&usb_endpoint_bulk_in)  != 0);
	assert(usb_endpoint_num(&usb_endpoint_bulk_out) != 0);
}

/*----------------------------------------------------------------------*/
/* Endpoint threads */
/*----------------------------------------------------------------------*/

#define EP0_MAX_DATA	256
#define BULK_BUF	512

struct usb_raw_control_event {
	struct usb_raw_event		inner;
	struct usb_ctrlrequest		ctrl;
};

struct usb_raw_control_io {
	struct usb_raw_ep_io		inner;
	char				data[EP0_MAX_DATA];
};

struct usb_raw_bulk_io {
	struct usb_raw_ep_io		inner;
	char				data[BULK_BUF];
};

int ep_bulk_in  = -1;
int ep_bulk_out = -1;

pthread_t ep_bulk_in_thread;
pthread_t ep_bulk_out_thread;

atomic_bool ep_bulk_threads_en = ATOMIC_VAR_INIT(false);

// ep_bulk_out_loop: receives data written by lcd_write() via async URB.
// Signals test_thread after each received packet so it can proceed to read.
void *ep_bulk_out_loop(void *arg) {
	int fd = (int)(long)arg;

	struct usb_raw_bulk_io io;
	io.inner.ep    = ep_bulk_out;
	io.inner.flags = 0;

	while (!atomic_load(&ep_bulk_threads_en));

	while (keep_running) {
		io.inner.length = BULK_BUF;

		int rv = ioctl(fd, USB_RAW_IOCTL_EP_READ, &io);
		if (rv < 0) {
			if (errno == ESHUTDOWN) {
				printf("ep_bulk_out: device was likely reset,"
					" exiting\n");
				break;
			}
			perror("ioctl(USB_RAW_IOCTL_EP_READ)");
			exit(EXIT_FAILURE);
		}

		printf("ep_bulk_out: received %d bytes: \"%.*s\"\n",
			rv, rv, io.inner.data);

		atomic_store(&write_received, true);
	}

	return NULL;
}

// ep_bulk_in_loop: waits until test_thread has set read_requested
// (i.e. lcd_read() has submitted the bulk IN URB), then sends the response.
void *ep_bulk_in_loop(void *arg) {
	int fd = (int)(long)arg;

	struct usb_raw_bulk_io io;
	io.inner.ep    = ep_bulk_in;
	io.inner.flags = 0;

	while (!atomic_load(&ep_bulk_threads_en));

	// Wait until test_thread is about to call read() so the bulk IN
	// URB is already submitted before we send data.
	while (!atomic_load(&read_requested) && keep_running);

	if (!keep_running)
		return NULL;

	const char *response = "LCD OK";
	io.inner.length = strlen(response);
	memcpy(io.inner.data, response, io.inner.length);

	int rv = usb_raw_ep_write_may_fail(fd, (struct usb_raw_ep_io *)&io);
	if (rv < 0 && errno == ESHUTDOWN) {
		printf("ep_bulk_in: device was likely reset, exiting\n");
		return NULL;
	}
	if (rv < 0) {
		perror("usb_raw_ep_write_may_fail()");
		exit(EXIT_FAILURE);
	}
	printf("ep_bulk_in: sent %d bytes\n", rv);

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
			ep_bulk_in = usb_raw_ep_enable(fd,
						&usb_endpoint_bulk_in);
			printf("ep0: bulk_in = ep#%d\n", ep_bulk_in);
			ep_bulk_out = usb_raw_ep_enable(fd,
						&usb_endpoint_bulk_out);
			printf("ep0: bulk_out = ep#%d\n", ep_bulk_out);

			int rv = pthread_create(&ep_bulk_in_thread, 0,
					ep_bulk_in_loop, (void *)(long)fd);
			if (rv != 0) {
				perror("pthread_create(ep_bulk_in)");
				exit(EXIT_FAILURE);
			}
			rv = pthread_create(&ep_bulk_out_thread, 0,
					ep_bulk_out_loop, (void *)(long)fd);
			if (rv != 0) {
				perror("pthread_create(ep_bulk_out)");
				exit(EXIT_FAILURE);
			}

			usb_raw_vbus_draw(fd, usb_config.bMaxPower);
			usb_raw_configure(fd);
			io->inner.length = 0;

			atomic_store(&ep_bulk_threads_en, true);

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

	usb_setup_signals(&ep0_tid);

	int fd = usb_raw_open();
	usb_raw_init(fd, USB_SPEED_HIGH, driver, device);
	usb_raw_run(fd);

	ep0_loop(fd);

	close(fd);

	return 0;
}
