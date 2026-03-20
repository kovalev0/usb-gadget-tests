// SPDX-License-Identifier: Apache-2.0
//
// Emulates an OpenMoko ChaosKey device (VID: 0x1d50, PID: 0x60c6).
// It uses USB 2.0 protocol over a HS connection.
// One bulk IN endpoint is configured; the driver submits a bulk URB
// to fill its buffer with random data via chaos_read_callback().
// Handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION).
//
// The emulator exercises the chaoskey driver by reading random data
// via /dev/chaoskey* and verifying hwrng registration via /dev/hwrng.
//
// Vasiliy Kovalev <kovalev@altlinux.org>

#include "../usb_gadget_tests.h"

/*----------------------------------------------------------------------*/

static volatile bool keep_running = true;

static pthread_t ep0_tid;

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
				if (strncmp(entry->d_name, "chaoskey", 8) == 0) {
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

	printf("[TEST] Waiting for device...\n");

	if (find_device(devpath, sizeof(devpath), 15) != 0) {
		printf("[TEST] ERR: device not found\n");
		goto done;
	}
	printf("[TEST] Found\n");

	// chaoskey_open: increments open count
	int devfd = open(devpath, O_RDONLY);
	if (devfd < 0) {
		printf("[TEST] ERR: open failed: %s\n",
							strerror(errno));
		goto done;
	}
	printf("[TEST] OK: open succeeded\n");

	// chaoskey_read -> _chaoskey_fill -> usb_submit_urb ->
	// chaos_read_callback -> copy_to_user
	unsigned char buf[64];
	int rv = read(devfd, buf, sizeof(buf));
	if (rv < 0) {
		printf("[TEST] ERR: read failed: %s\n",
							strerror(errno));
	} else {
		printf("[TEST] OK: read\n");
		/*
		printf("[TEST] OK: read %d bytes:"
			" 0x%02x 0x%02x 0x%02x 0x%02x ...\n", rv,
			buf[0], buf[1], buf[2], buf[3]);
		*/
	}

	// Second read: driver reuses buffered bytes (valid > used) or
	// re-submits the URB
	rv = read(devfd, buf, sizeof(buf));
	if (rv < 0)
		printf("[TEST] ERR: second read failed: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: second read\n");
		// printf("[TEST] OK: second read %d bytes\n", rv);

	// chaoskey_release: decrements open count
	close(devfd);
	printf("[TEST] OK: closed\n");

	// chaoskey_rng_read via /dev/hwrng (if chaoskey is the current source)
	int hwrng_fd = open("/dev/hwrng", O_RDONLY | O_NONBLOCK);
	if (hwrng_fd >= 0) {
		unsigned char rng_buf[16];
		rv = read(hwrng_fd, rng_buf, sizeof(rng_buf));
		if (rv > 0)
			printf("[TEST] OK: hwrng read\n");
		/*
			printf("[TEST] OK: hwrng read %d bytes:"
				" 0x%02x 0x%02x 0x%02x 0x%02x ...\n", rv,
				rng_buf[0], rng_buf[1],
				rng_buf[2], rng_buf[3]);
		*/
		else
			printf("[TEST] hwrng read: %s\n",
							strerror(errno));
		close(hwrng_fd);
	} else {
		printf("[TEST] hwrng not available: %s\n",
							strerror(errno));
	}

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

#define USB_VENDOR		0x1d50	// OpenMoko
#define USB_PRODUCT		0x60c6	// ChaosKey

// HS bulk endpoint requires maxpacket 512.
// The driver caps its internal buffer at CHAOSKEY_BUF_LEN (64) and
// fills URBs for exactly 64 bytes, so each write from the emulator
// also sends 64 bytes (CHAOSKEY_SEND_LEN).
#define EP_MAX_PACKET_BULK	512
#define CHAOSKEY_SEND_LEN	64

#define STRING_ID_MANUFACTURER	1
#define STRING_ID_PRODUCT	2
#define STRING_ID_SERIAL	3
#define STRING_ID_CONFIG	4
#define STRING_ID_INTERFACE	5

#define EP_MAX_PACKET_CONTROL	64

// Assigned dynamically
#define EP_NUM_BULK_IN		0x0

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
	}

	assert(usb_endpoint_num(&usb_endpoint_bulk_in) != 0);
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

// Send buffer matches CHAOSKEY_BUF_LEN=64: the driver fills each URB
// for exactly 64 bytes regardless of the endpoint's wMaxPacketSize.
struct usb_raw_bulk_io {
	struct usb_raw_ep_io		inner;
	char				data[CHAOSKEY_SEND_LEN];
};

int ep_bulk_in = -1;
pthread_t ep_bulk_in_thread;
atomic_bool ep_bulk_in_en = ATOMIC_VAR_INIT(false);

// ep_bulk_in_loop: serves each bulk IN URB submitted by _chaoskey_fill().
// Stops as soon as keep_running goes false (test done) or ESHUTDOWN.
void *ep_bulk_in_loop(void *arg) {
	int fd = (int)(long)arg;
	unsigned int counter = 0;

	struct usb_raw_bulk_io io;
	io.inner.ep     = ep_bulk_in;
	io.inner.flags  = 0;
	io.inner.length = CHAOSKEY_SEND_LEN;

	while (!atomic_load(&ep_bulk_in_en));

	while (keep_running) {
		for (int i = 0; i < CHAOSKEY_SEND_LEN; i++)
			io.inner.data[i] = (counter + i) & 0xff;
		counter++;

		int rv = usb_raw_ep_write_may_fail(fd,
					(struct usb_raw_ep_io *)&io);
		if (rv == io.inner.length) {
			// printf("ep_bulk_in: sent %d bytes\n", rv);
			;
		} else if (rv < 0 && errno == ESHUTDOWN) {
			printf("ep_bulk_in: device was likely reset,"
				" exiting\n");
			break;
		} else if (rv < 0) {
			perror("usb_raw_ep_write_may_fail()");
			exit(EXIT_FAILURE);
		} else {
			// Partial write is normal if the driver's URB was
			// smaller than our buffer - treat it as success.
			// printf("ep_bulk_in: sent %d bytes\n", rv);
			;
		}
	}

	return NULL;
}

/*----------------------------------------------------------------------*/
/* EP0 */
/*----------------------------------------------------------------------*/

static const char *string_product = "ChaosKey";
static const char *string_serial  = "001";

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
							STRING_ID_PRODUCT) {
					build_string_descriptor(io,
							string_product);
				} else if ((event->ctrl.wValue & 0xff) ==
							STRING_ID_SERIAL) {
					build_string_descriptor(io,
							string_serial);
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
			ep_bulk_in = usb_raw_ep_enable(fd,
						&usb_endpoint_bulk_in);
			printf("ep0: bulk_in = ep#%d\n", ep_bulk_in);

			int rv = pthread_create(&ep_bulk_in_thread, 0,
					ep_bulk_in_loop, (void *)(long)fd);
			if (rv != 0) {
				perror("pthread_create(ep_bulk_in)");
				exit(EXIT_FAILURE);
			}

			usb_raw_vbus_draw(fd, usb_config.bMaxPower);
			usb_raw_configure(fd);
			io->inner.length = 0;

			atomic_store(&ep_bulk_in_en, true);

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
