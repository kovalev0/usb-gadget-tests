// SPDX-License-Identifier: Apache-2.0
//
// Emulates an Ontrak ADU100 device (VID: 0x0a07, PID: 0x0064),
// simulating device enumeration and interrupt IN/OUT transfers.
// It uses USB 2.0 protocol over a HS connection.
// Two interrupt endpoints are configured: INT_IN and INT_OUT.
// Handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION).
//
// Vasiliy Kovalev <kovalev@altlinux.org>

#include "../usb_gadget_tests.h"

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
/* Device File Test */
/*----------------------------------------------------------------------*/

static int find_device(char *devpath, size_t maxlen, int timeout_sec) {
	time_t start = time(NULL);

	while (difftime(time(NULL), start) < timeout_sec) {
		DIR *dir = opendir("/dev");
		if (dir) {
			struct dirent *entry;
			while ((entry = readdir(dir)) != NULL) {
				if (strncmp(entry->d_name, "adutux", 6) == 0) {
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

static atomic_bool test_device_opened = ATOMIC_VAR_INIT(false);
static atomic_bool test_read_done     = ATOMIC_VAR_INIT(false);
static atomic_bool test_write_done    = ATOMIC_VAR_INIT(false);

static void test_read_write(void) {
	char devpath[512];

	printf("[TEST] Waiting for device...\n");

	if (find_device(devpath, sizeof(devpath), 15) != 0) {
		printf("[TEST] ERR: Device not found\n");
		return;
	}
	printf("[TEST] Device found\n");

	int devfd = open(devpath, O_RDWR);
	if (devfd < 0) {
		printf("[TEST] ERR: Open failed: %s\n",
							strerror(errno));
		return;
	}
	printf("[TEST] OK: Open succeeded\n");

	atomic_store(&test_device_opened, true);

	// Second open() should return EBUSY (adu_open: open_count check)
	int devfd2 = open(devpath, O_RDWR);
	if (devfd2 < 0 && errno == EBUSY) {
		printf("[TEST] OK: Second open returned EBUSY"
			" (expected)\n");
	} else if (devfd2 >= 0) {
		printf("[TEST] ERR: Second open unexpectedly"
			" succeeded\n");
		close(devfd2);
	} else {
		printf("[TEST] ERR: Second open failed with"
			" unexpected error: %s\n", strerror(errno));
	}

	char buf[8] = { 0 };
	int rv = read(devfd, buf, sizeof(buf));
	if (rv < 0)
		printf("[TEST] ERR: Read failed: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: Read\n");
		/*
		printf("[TEST] OK: Read %d bytes:"
			" 0x%02x 0x%02x 0x%02x 0x%02x ...\n", rv,
			(unsigned char)buf[0], (unsigned char)buf[1],
			(unsigned char)buf[2], (unsigned char)buf[3]);
		*/

	atomic_store(&test_read_done, true);

	// "$PA\r" — standard ADU100 port-A read command
	char cmd[8] = { '$', 'P', 'A', '\r', 0, 0, 0, 0 };
	rv = write(devfd, cmd, sizeof(cmd));
	if (rv < 0)
		printf("[TEST] ERR: Write failed: %s\n",
							strerror(errno));
	else
		printf("[TEST] OK: Write\n");
		// printf("[TEST] OK: Write %d bytes\n", rv);

	atomic_store(&test_write_done, true);

	// Wait for ep_int_out_loop to receive the packet
	while (atomic_load(&test_write_done));

	close(devfd);
	printf("[TEST] OK: Closed\n");
	printf("[TEST] Done\n");
	alarm(5);
}

/*----------------------------------------------------------------------*/
/* USB device descriptors */
/*----------------------------------------------------------------------*/

#define BCD_USB			0x0200

#define USB_VENDOR		0x0a07	// Ontrak Control Systems Inc.
#define USB_PRODUCT		0x0064	// ADU100

#define STRING_ID_MANUFACTURER	1
#define STRING_ID_PRODUCT	2
#define STRING_ID_SERIAL	3
#define STRING_ID_CONFIG	4
#define STRING_ID_INTERFACE	5

#define EP_MAX_PACKET_CONTROL	64
#define EP_MAX_PACKET_INT	8

// Assigned dynamically
#define EP_NUM_INT_IN		0x0
#define EP_NUM_INT_OUT		0x0

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
	.bNumEndpoints =	2,
	.bInterfaceClass =	0,
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

struct usb_endpoint_descriptor usb_endpoint_int_out = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_OUT | EP_NUM_INT_OUT,
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

	assert(length >= USB_DT_ENDPOINT_SIZE);
	memcpy(data, &usb_endpoint_int_out, USB_DT_ENDPOINT_SIZE);
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
		if (assign_ep_address(&info.eps[i], &usb_endpoint_int_out))
			continue;
	}

	int int_in_addr  = usb_endpoint_num(&usb_endpoint_int_in);
	int int_out_addr = usb_endpoint_num(&usb_endpoint_int_out);

	assert(int_in_addr  != 0);
	assert(int_out_addr != 0);
}

/*----------------------------------------------------------------------*/
/* Endpoint threads */
/*----------------------------------------------------------------------*/

#define EP0_MAX_DATA 256

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

int ep_int_in  = -1;
int ep_int_out = -1;

pthread_t ep_int_in_thread;
pthread_t ep_int_out_thread;

atomic_bool ep_int_threads_en = ATOMIC_VAR_INIT(false);

static int send_in_packet(int fd, struct usb_raw_int_io *io) {
	int rv = usb_raw_ep_write_may_fail(fd, (struct usb_raw_ep_io *)io);
	if (rv == io->inner.length) {
		// printf("ep_int_in: sent %d bytes: 0x%02x ...\n", rv,
		//			(unsigned char)io->inner.data[0]);
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
	// printf("ep_int_in: partial send: %d bytes\n", rv);
	return 0;
}

void *ep_int_in_loop(void *arg) {
	int fd = (int)(long)arg;

	struct usb_raw_int_io io;
	io.inner.ep     = ep_int_in;
	io.inner.flags  = 0;
	io.inner.length = EP_MAX_PACKET_INT;
	memset(io.inner.data, 0, EP_MAX_PACKET_INT);

	while (!atomic_load(&ep_int_threads_en));

	// Wait for adu_open() to submit the first IN URB
	while (!atomic_load(&test_device_opened));

	// data[0]==0x00: exercises the ignored-packet branch in
	// adu_interrupt_in_callback()
	io.inner.data[0] = 0x00;
	if (send_in_packet(fd, &io) < 0)
		return NULL;

	// data[0]==0x01: accepted into read_buffer_primary
	io.inner.data[0] = 0x01;
	io.inner.data[1] = 0x42;
	if (send_in_packet(fd, &io) < 0)
		return NULL;

	// Wait for adu_read() to re-submit the IN URB after draining the buffer
	while (!atomic_load(&test_read_done));

	io.inner.data[0] = 0x01;
	io.inner.data[1] = 0x00;
	if (send_in_packet(fd, &io) < 0)
		return NULL;

	// Drain remaining IN URBs until disconnect
	while (true) {
		io.inner.data[0] = 0x00;
		if (send_in_packet(fd, &io) < 0)
			break;
	}

	return NULL;
}

void *ep_int_out_loop(void *arg) {
	int fd = (int)(long)arg;

	struct usb_raw_int_io io;
	io.inner.ep     = ep_int_out;
	io.inner.flags  = 0;
	io.inner.length = EP_MAX_PACKET_INT;

	while (!atomic_load(&ep_int_threads_en));

	// Wait for adu_write() to submit the OUT URB
	while (!atomic_load(&test_write_done));

	int rv = ioctl(fd, USB_RAW_IOCTL_EP_READ, &io);
	if (rv < 0) {
		if (errno == ESHUTDOWN)
			printf("ep_int_out: device was likely reset, exiting\n");
		else {
			perror("ioctl(USB_RAW_IOCTL_EP_READ)");
			exit(EXIT_FAILURE);
		}
		return NULL;
	}

	/*
	printf("ep_int_out: received %d bytes:", rv);
	for (int i = 0; i < rv; i++)
		printf(" 0x%02x", (unsigned char)io.inner.data[i]);
	printf("\n");
	*/

	// Signal test_read_write that the packet was received
	atomic_store(&test_write_done, false);

	return NULL;
}

atomic_bool ep0_request_end = ATOMIC_VAR_INIT(false);

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
			ep_int_in = usb_raw_ep_enable(fd, &usb_endpoint_int_in);
			printf("ep0: int_in = ep#%d\n", ep_int_in);
			ep_int_out = usb_raw_ep_enable(fd, &usb_endpoint_int_out);
			printf("ep0: int_out = ep#%d\n", ep_int_out);

			int rv = pthread_create(&ep_int_in_thread, 0,
					ep_int_in_loop, (void *)(long)fd);
			if (rv != 0) {
				perror("pthread_create(ep_int_in)");
				exit(EXIT_FAILURE);
			}
			rv = pthread_create(&ep_int_out_thread, 0,
					ep_int_out_loop, (void *)(long)fd);
			if (rv != 0) {
				perror("pthread_create(ep_int_out)");
				exit(EXIT_FAILURE);
			}

			usb_raw_vbus_draw(fd, usb_config.bMaxPower);
			usb_raw_configure(fd);
			io->inner.length = 0;

			atomic_store(&ep_int_threads_en, true);
			atomic_store(&ep0_request_end, true);

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
		if (atomic_load(&ep0_request_end)) {
			test_read_write();
			return;
		}

		struct usb_raw_control_event event;
		event.inner.type   = 0;
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
		io.inner.ep     = 0;
		io.inner.flags  = 0;
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
}

static pthread_t ep0_tid;

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
