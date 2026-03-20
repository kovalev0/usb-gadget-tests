// SPDX-License-Identifier: Apache-2.0
//
// Emulates a PlayStation 2 Trance Vibrator device (VID: 0x0b49, PID: 0x064f).
// It uses USB 2.0 protocol over a HS connection.
// No bulk/interrupt endpoints are used; all interaction happens
// via EP0 vendor OUT control transfers (bRequest=0x01, wValue=speed).
// Handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION).
//
// The emulator exercises the trancevibrator driver via
// /sys/bus/usb/drivers/trancevibrator/* speed sysfs attribute. \*/
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
	case USB_TYPE_VENDOR:
		printf("  type = USB_TYPE_VENDOR\n");
		printf("  req = set_speed, speed = %d\n", ctrl->wValue);
		break;
	default:
		printf("  type = unknown = %d\n", (int)ctrl->bRequestType);
		break;
	}
}

/*----------------------------------------------------------------------*/
/* Sysfs test */
/*----------------------------------------------------------------------*/

static int find_sysfs_path(char *path, size_t maxlen, int timeout_sec) {
	time_t start = time(NULL);

	while (difftime(time(NULL), start) < timeout_sec) {
		DIR *dir = opendir("/sys/bus/usb/drivers/trancevibrator");
		if (dir) {
			struct dirent *entry;
			while ((entry = readdir(dir)) != NULL) {
				if (strchr(entry->d_name, ':')) {
					snprintf(path, maxlen,
						"/sys/bus/usb/drivers"
						"/trancevibrator/%s",
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

static int sysfs_read(const char *base, const char *attr,
						char *buf, size_t len) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", base, attr);

	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	int rv = read(fd, buf, len - 1);
	if (rv > 0) {
		buf[rv] = '\0';
		if (buf[rv - 1] == '\n')
			buf[rv - 1] = '\0';
	}
	close(fd);
	return rv;
}

static int sysfs_write(const char *base, const char *attr,
						const char *value) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", base, attr);

	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	int rv = write(fd, value, strlen(value));
	close(fd);
	return rv;
}

void *test_thread(void *arg) {
	char dev_path[512];
	char buf[64];

	printf("[TEST] Waiting for sysfs device...\n");

	if (find_sysfs_path(dev_path, sizeof(dev_path), 15) != 0) {
		printf("[TEST] ERR: device not found\n");
		goto done;
	}
	printf("[TEST] Found\n");

	// Wait for probe to finish registering sysfs attributes.
	// The sysfs entry appears before attributes are fully created.
	{
		char attr_path[1024];
		snprintf(attr_path, sizeof(attr_path), "%s/speed", dev_path);
		int waited = 0;
		while (access(attr_path, F_OK) != 0 && waited < 50) {
			usleep(100000);
			waited++;
		}
	}

	// speed_show: reads tv->speed (initial = 0, no USB transfer)
	if (sysfs_read(dev_path, "speed", buf, sizeof(buf)) > 0)
		printf("[TEST] speed = %s\n", buf);

	// speed_store: vendor OUT request bRequest=0x01, wValue=speed
	printf("[TEST] Setting speed = 128\n");
	if (sysfs_write(dev_path, "speed", "128") < 0)
		printf("[TEST] ERR: write speed: %s\n",
							strerror(errno));

	if (sysfs_read(dev_path, "speed", buf, sizeof(buf)) > 0)
		printf("[TEST] speed after write = %s\n", buf);

	// Speed is clamped to [0..255]: write 300 -> stored as 255
	printf("[TEST] Setting speed = 300 (expect clamp to 255)\n");
	if (sysfs_write(dev_path, "speed", "300") < 0)
		printf("[TEST] ERR: write speed 300: %s\n",
							strerror(errno));

	if (sysfs_read(dev_path, "speed", buf, sizeof(buf)) > 0)
		printf("[TEST] speed after clamp = %s\n", buf);

	// Set speed back to 0 (stop vibration)
	printf("[TEST] Setting speed = 0\n");
	if (sysfs_write(dev_path, "speed", "0") < 0)
		printf("[TEST] ERR: write speed 0: %s\n",
							strerror(errno));

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

#define USB_VENDOR		0x0b49	// ASCII Corporation
#define USB_PRODUCT		0x064f	// Trance Vibrator

#define STRING_ID_MANUFACTURER	1
#define STRING_ID_PRODUCT	2
#define STRING_ID_SERIAL	3
#define STRING_ID_CONFIG	4
#define STRING_ID_INTERFACE	5

#define EP_MAX_PACKET_CONTROL	64

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
	.bNumEndpoints =	0,
	.bInterfaceClass =	0,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	0,
	.iInterface =		STRING_ID_INTERFACE,
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

	config->wTotalLength = __cpu_to_le16(total_length);
	printf("config->wTotalLength: %d\n", total_length);

	return total_length;
}

/*----------------------------------------------------------------------*/
/* EP0 */
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
			usb_raw_vbus_draw(fd, usb_config.bMaxPower);
			usb_raw_configure(fd);
			io->inner.length = 0;

			if (!test_started) {
				test_started = true;
				int rv = pthread_create(&test_tid, NULL,
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
	case USB_TYPE_VENDOR:
		// speed_store: usb_control_msg OUT, wLength=0
		io->inner.length = 0;
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

		if (event.inner.type == USB_RAW_EVENT_CONNECT ||
		    event.inner.type == USB_RAW_EVENT_SUSPEND ||
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
