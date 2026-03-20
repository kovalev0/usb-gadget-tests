// SPDX-License-Identifier: Apache-2.0
//
// Emulates an Apple Cinema Display (VID: 0x05ac, PID: 0x9218).
// It uses USB 2.0 protocol over a HS connection.
// One interrupt IN endpoint is configured for button events.
// Handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION) and HID class requests GET_REPORT/SET_REPORT
// for brightness control.
//
// The emulator exercises the appledisplay driver via
// /sys/class/backlight/appledisplay*/ and by sending interrupt IN
// packets that simulate brightness button presses.
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
	case USB_TYPE_CLASS:
		printf("  type = USB_TYPE_CLASS\n");
		switch (ctrl->bRequest) {
		case 0x01:
			printf("  req = GET_REPORT, wValue = 0x%x\n",
				ctrl->wValue);
			break;
		case 0x09:
			printf("  req = SET_REPORT, wValue = 0x%x\n",
				ctrl->wValue);
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
/* Backlight sysfs test */
/*----------------------------------------------------------------------*/

static int find_backlight(char *path, size_t maxlen, int timeout_sec) {
	time_t start = time(NULL);

	while (difftime(time(NULL), start) < timeout_sec) {
		DIR *dir = opendir("/sys/class/backlight");
		if (dir) {
			struct dirent *entry;
			while ((entry = readdir(dir)) != NULL) {
				if (strncmp(entry->d_name,
						"appledisplay", 12) == 0) {
					snprintf(path, maxlen,
						"/sys/class/backlight/%s",
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

static int bl_read(const char *bl_path, const char *attr,
						char *buf, size_t len) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", bl_path, attr);

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

static int bl_write(const char *bl_path, const char *attr,
						const char *value) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", bl_path, attr);

	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	int rv = write(fd, value, strlen(value));
	close(fd);
	return rv;
}

// Signals from ep_int_in_loop to test_thread that a button packet
// has been delivered and the driver has scheduled its work.
static atomic_bool btn_up_sent   = ATOMIC_VAR_INIT(false);
static atomic_bool btn_down_sent = ATOMIC_VAR_INIT(false);
static atomic_bool btn_none_sent = ATOMIC_VAR_INIT(false);

void *test_thread(void *arg) {
	char bl_path[512];
	char buf[64];

	printf("[TEST] Waiting for backlight device...\n");

	if (find_backlight(bl_path, sizeof(bl_path), 15) != 0) {
		printf("[TEST] ERR: backlight not found\n");
		goto done;
	}
	printf("[TEST] Found\n");

	// appledisplay_bl_get_brightness: GET_REPORT on probe
	if (bl_read(bl_path, "actual_brightness", buf, sizeof(buf)) > 0)
		printf("[TEST] actual_brightness = %s\n", buf);

	if (bl_read(bl_path, "max_brightness", buf, sizeof(buf)) > 0)
		printf("[TEST] max_brightness = %s\n", buf);

	// appledisplay_bl_update_status: SET_REPORT
	printf("[TEST] Setting brightness = 200\n");
	if (bl_write(bl_path, "brightness", "200\n") < 0)
		printf("[TEST] ERR: write brightness: %s\n",
							strerror(errno));

	sleep(1);

	// Wait for ep_int_in_loop to send ACD_BTN_BRIGHT_UP (3).
	// Driver: appledisplay_complete -> schedule_delayed_work ->
	//         appledisplay_work -> GET_REPORT -> re-schedule if
	//         button_pressed. Then we send ACD_BTN_NONE to stop.
	printf("[TEST] Waiting for BTN_BRIGHT_UP event...\n");
	while (!atomic_load(&btn_up_sent));
	atomic_store(&btn_up_sent, false);
	sleep(1);
	printf("[TEST] BTN_BRIGHT_UP processed\n");

	// Wait for ACD_BTN_BRIGHT_DOWN (4)
	printf("[TEST] Waiting for BTN_BRIGHT_DOWN event...\n");
	while (!atomic_load(&btn_down_sent));
	atomic_store(&btn_down_sent, false);
	sleep(1);
	printf("[TEST] BTN_BRIGHT_DOWN processed\n");

	// Wait for ACD_BTN_NONE (0) - button_pressed = 0, polling stops
	printf("[TEST] Waiting for BTN_NONE event...\n");
	while (!atomic_load(&btn_none_sent));
	atomic_store(&btn_none_sent, false);
	sleep(1);
	printf("[TEST] BTN_NONE processed\n");

	// appledisplay_bl_get_brightness: final read
	if (bl_read(bl_path, "actual_brightness", buf, sizeof(buf)) > 0)
		printf("[TEST] actual_brightness = %s\n", buf);

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

#define USB_VENDOR		0x05ac	// Apple Inc.
#define USB_PRODUCT		0x9218	// Apple Cinema Display

#define STRING_ID_MANUFACTURER	1
#define STRING_ID_PRODUCT	2
#define STRING_ID_SERIAL	3
#define STRING_ID_CONFIG	4
#define STRING_ID_INTERFACE	5

#define EP_MAX_PACKET_CONTROL	64
#define EP_MAX_PACKET_INT	2	// ACD_URB_BUFFER_LEN

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
	.bInterfaceProtocol =	0x00,
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
#define ACD_BTN_NONE		0
#define ACD_BTN_BRIGHT_UP	3
#define ACD_BTN_BRIGHT_DOWN	4

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
	char				data[EP_MAX_PACKET_INT];
};

int ep_int_in = -1;
pthread_t ep_int_in_thread;
atomic_bool ep_int_in_en = ATOMIC_VAR_INIT(false);

static int send_btn(int fd, struct usb_raw_int_io *io, uint8_t btn) {
	io->inner.data[0] = 0x00;
	io->inner.data[1] = btn;

	int rv = usb_raw_ep_write_may_fail(fd, (struct usb_raw_ep_io *)io);
	if (rv == io->inner.length) {
		// printf("ep_int_in: sent btn=0x%02x\n", btn);
		return 0;
	}
	if (rv < 0 && errno == ESHUTDOWN) {
		printf("ep_int_in: device was likely reset, exiting\n");
		return -1;
	}
	if (rv < 0) {
		perror("usb_raw_ep_write_may_fail()");
		exit(EXIT_FAILURE);
	}
	return 0;
}

// ep_int_in_loop: sends button events that exercise appledisplay_complete():
//   ACD_BTN_BRIGHT_UP   -> button_pressed=1, schedule_delayed_work
//   ACD_BTN_BRIGHT_DOWN -> button_pressed=1, schedule_delayed_work
//   ACD_BTN_NONE        -> button_pressed=0, polling stops
void *ep_int_in_loop(void *arg) {
	int fd = (int)(long)arg;

	struct usb_raw_int_io io;
	io.inner.ep     = ep_int_in;
	io.inner.flags  = 0;
	io.inner.length = EP_MAX_PACKET_INT;

	while (!atomic_load(&ep_int_in_en));

	// ACD_BTN_BRIGHT_UP (urbdata[1]=3)
	if (send_btn(fd, &io, ACD_BTN_BRIGHT_UP) < 0)
		return NULL;
	atomic_store(&btn_up_sent, true);

	// Wait for test_thread to observe the event before next button
	while (atomic_load(&btn_up_sent));
	sleep(1);

	// ACD_BTN_BRIGHT_DOWN (urbdata[1]=4)
	if (send_btn(fd, &io, ACD_BTN_BRIGHT_DOWN) < 0)
		return NULL;
	atomic_store(&btn_down_sent, true);

	while (atomic_load(&btn_down_sent));
	sleep(1);

	// ACD_BTN_NONE (urbdata[1]=0) - stops the work polling loop
	if (send_btn(fd, &io, ACD_BTN_NONE) < 0)
		return NULL;
	atomic_store(&btn_none_sent, true);

	sleep(10);

	return NULL;
}

/*----------------------------------------------------------------------*/
/* EP0 */
/*----------------------------------------------------------------------*/

// Current brightness reported by the emulator (msgdata[1] in the driver)
static uint8_t current_brightness = 0x80;

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
		switch (event->ctrl.bRequest) {
		case 0x01: // GET_REPORT - appledisplay_bl_get_brightness
			io->data[0] = 0x10;
			io->data[1] = current_brightness;
			io->inner.length = 2;
			return true;
		case 0x09: // SET_REPORT - appledisplay_bl_update_status
			// brightness arrives in the OUT data phase (2 bytes);
			// length must match ACD_MSG_BUFFER_LEN so ep0_read
			// has room for the payload.
			io->inner.length = 2;
			return true;
		default:
			printf("ep0: unknown class request\n");
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
			// For SET_REPORT: read the brightness value the
			// driver wrote so we can echo it back on GET_REPORT.
			int r = usb_raw_ep0_read(fd,
					(struct usb_raw_ep_io *)&io);
			if (!test_started)
				printf("ep0: transferred %d bytes (out)\n", r);
			if ((event.ctrl.bRequestType & USB_TYPE_MASK)
					== USB_TYPE_CLASS &&
			    event.ctrl.bRequest == 0x09 && r >= 2)
				current_brightness = io.data[1];
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
