// SPDX-License-Identifier: Apache-2.0
//
// Emulates a Siemens ID Mouse fingerprint sensor (VID: 0x0681, PID: 0x0005).
// It uses USB 2.0 protocol over a HS connection.
// One bulk IN endpoint is configured (bInterfaceClass=0x0A required by probe).
// Handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION) and vendor OUT control requests (FTIP_RELEASE,
// FTIP_BLINK, FTIP_ACQUIRE, FTIP_RESET) sent by idmouse_create_image().
//
// The emulator serves a synthetic fingerprint image (PGM format,
// 225x289) via bulk IN. The image passes the driver's border validation:
// right column = 0x00, bottom row = 0xFF.
//
// Vasiliy Kovalev <kovalev@altlinux.org>

#include "../usb_gadget_tests.h"

/*----------------------------------------------------------------------*/

static volatile bool keep_running = true;

static pthread_t ep0_tid;

/*----------------------------------------------------------------------*/

/* sensor command codes (mirror of driver defines) */
#define FTIP_RESET	0x20
#define FTIP_ACQUIRE	0x21
#define FTIP_RELEASE	0x22
#define FTIP_BLINK	0x23
#define FTIP_SCROLL	0x24

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
		switch (ctrl->bRequest) {
		case FTIP_RESET:
			printf("  req = FTIP_RESET\n");
			break;
		case FTIP_ACQUIRE:
			printf("  req = FTIP_ACQUIRE\n");
			break;
		case FTIP_RELEASE:
			printf("  req = FTIP_RELEASE\n");
			break;
		case FTIP_BLINK:
			printf("  req = FTIP_BLINK, pulse_width = %d\n",
				ctrl->wValue & 0xff);
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
/* Image construction */
/*----------------------------------------------------------------------*/

#define WIDTH		225
#define HEIGHT		289
#define IMGSIZE		(WIDTH * HEIGHT)
#define BULK_PKT	512

// Build a synthetic PGM pixel buffer that passes idmouse_create_image()
// border validation:
//   right column  (pixel WIDTH-1 of each row) = 0x00
//   bottom row    (last HEIGHT-1 row, all but last pixel) = 0xFF
static void build_image(unsigned char *buf) {
	// Fill with mid-grey
	memset(buf, 0x80, IMGSIZE);

	// Right column = 0x00 (already 0 after memset if we set it explicitly)
	for (int row = 0; row < HEIGHT; row++)
		buf[row * WIDTH + (WIDTH - 1)] = 0x00;

	// Bottom row = 0xFF (all pixels in the last row except the very last)
	for (int col = 0; col < WIDTH - 1; col++)
		buf[(HEIGHT - 1) * WIDTH + col] = 0xFF;
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
				if (strncmp(entry->d_name, "idmouse", 7) == 0) {
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

	// idmouse_open: calls idmouse_create_image() which sends vendor
	// commands and reads the full image via bulk IN
	int devfd = open(devpath, O_RDONLY);
	if (devfd < 0) {
		printf("[TEST] ERR: open failed: %s\n",
							strerror(errno));
		goto done;
	}
	printf("[TEST] OK: open succeeded\n");

	// idmouse_read: simple_read_from_buffer from bulk_in_buffer
	// Read the PGM header first (15 bytes)
	char header[16] = { 0 };
	int rv = read(devfd, header, 15);
	if (rv < 0) {
		printf("[TEST] ERR: read header failed: %s\n",
							strerror(errno));
	} else {
		printf("[TEST] OK: header = \"%.15s\"\n", header);
	}

	// Read the pixel data
	unsigned char *pixels = malloc(IMGSIZE);
	if (pixels) {
		rv = read(devfd, pixels, IMGSIZE);
		if (rv < 0)
			printf("[TEST] ERR: read pixels failed:"
				" %s\n", strerror(errno));
		else
			printf("[TEST] OK: read %d pixel bytes\n",
				rv);
		free(pixels);
	}

	// idmouse_release: decrements open count
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

#define USB_VENDOR		0x0681	// Siemens
#define USB_PRODUCT		0x0005	// ID Mouse

#define STRING_ID_MANUFACTURER	1
#define STRING_ID_PRODUCT	2
#define STRING_ID_SERIAL	3
#define STRING_ID_CONFIG	4
#define STRING_ID_INTERFACE	5

#define EP_MAX_PACKET_CONTROL	64
#define EP_MAX_PACKET_BULK	512

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
	.bInterfaceClass =	0x0A,	// required: probe checks for this
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

struct usb_raw_bulk_io {
	struct usb_raw_ep_io		inner;
	char				data[BULK_PKT];
};

int ep_bulk_in = -1;
pthread_t ep_bulk_in_thread;
atomic_bool ep_bulk_in_en = ATOMIC_VAR_INIT(false);

// ep_bulk_in_loop: serves usb_bulk_msg() calls from idmouse_create_image().
// Each call requests dev->bulk_in_size=0x200=512 bytes. We send IMGSIZE
// bytes total in BULK_PKT-sized packets, then stop.
// On the next open() a new image transfer is requested, so we wait for
// the next ep_bulk_in_en signal.
void *ep_bulk_in_loop(void *arg) {
	int fd = (int)(long)arg;

	// Build the synthetic image once
	static unsigned char image[IMGSIZE];
	build_image(image);

	struct usb_raw_bulk_io io;
	io.inner.ep    = ep_bulk_in;
	io.inner.flags = 0;

	while (keep_running) {
		// Wait for idmouse_create_image() to start bulk reading
		while (!atomic_load(&ep_bulk_in_en) && keep_running);
		if (!keep_running)
			break;
		atomic_store(&ep_bulk_in_en, false);

		int sent = 0;
		bool ok = true;

		while (sent < IMGSIZE && keep_running) {
			int chunk = IMGSIZE - sent;
			if (chunk > BULK_PKT)
				chunk = BULK_PKT;

			io.inner.length = chunk;
			memcpy(io.inner.data, image + sent, chunk);

			int rv = usb_raw_ep_write_may_fail(fd,
					(struct usb_raw_ep_io *)&io);
			if (rv < 0 && errno == ESHUTDOWN) {
				printf("ep_bulk_in: device was likely reset,"
					" exiting\n");
				ok = false;
				break;
			}
			if (rv < 0) {
				perror("usb_raw_ep_write_may_fail()");
				exit(EXIT_FAILURE);
			}
			sent += rv;
		}

		if (ok)
			; //printf("ep_bulk_in: sent %d bytes (full image)\n",
			  // 	sent);
	}

	return NULL;
}

/*----------------------------------------------------------------------*/
/* EP0 */
/*----------------------------------------------------------------------*/

// Tracks vendor command sequence so we signal ep_bulk_in_loop at the
// right moment: after the second FTIP_RESET the driver starts bulk reads.
static int ftip_reset_count = 0;

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

			int rv = pthread_create(&ep_bulk_in_thread, 0,
					ep_bulk_in_loop, (void *)(long)fd);
			if (rv != 0) {
				perror("pthread_create(ep_bulk_in)");
				exit(EXIT_FAILURE);
			}

			usb_raw_vbus_draw(fd, usb_config.bMaxPower);
			usb_raw_configure(fd);
			io->inner.length = 0;

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
	case USB_TYPE_VENDOR:
		// All ftip_command() calls are OUT (usb_sndctrlpipe),
		// wLength=0, so we just ACK them.
		io->inner.length = 0;

		// Signal ep_bulk_in_loop after the second FTIP_RESET:
		// that's when the driver enters the bulk read loop.
		if (event->ctrl.bRequest == FTIP_RESET) {
			ftip_reset_count++;
			if (ftip_reset_count >= 2) {
				ftip_reset_count = 0;
				atomic_store(&ep_bulk_in_en, true);
			}
		} else if (event->ctrl.bRequest == FTIP_RELEASE) {
			// FTIP_RELEASE resets the sequence
			ftip_reset_count = 0;
		}
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
