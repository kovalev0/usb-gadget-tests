// SPDX-License-Identifier: Apache-2.0
//
// Emulates an FTDI USB Serial device (VID: 0x0403, PID: 0x6014),
// simulating device enumeration.
// It uses a vendor-specific protocol (USB 2.0) over a HS connection.
// Two bulk endpoints (IN and OUT) are configured.
// Handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION) and FTDI-specific vendor requests
// (e.g., GET_LATENCY_TIMER, READ_EEPROM).
//
// Supports GPIO via gpiolib (enabled by default, disable with
// --no-gpiolib).
//
// Vasiliy Kovalev <kovalev@altlinux.org>

#include "../usb_gadget_tests.h"

/*----------------------------------------------------------------------*/

#define FTDI_SIO_REQUEST_TYPE		0x40
#define FTDI_SIO_SET_LATENCY_TIMER	9
#define FTDI_SIO_GET_LATENCY_TIMER	0x0a
#define FTDI_SIO_READ_EEPROM		0x90

// The ftdi_gpio_init() function requests reading GPIO settings
// from eeprom if GPIO is supported.
// Use the "--no-gpiolib" option if gpiolib is not supported.
// Introduce default enabled (CONFIG_GPIOLIB=y) flag.
bool gpiolib_set = true;

/*----------------------------------------------------------------------*/

void log_control_request(struct usb_ctrlrequest *ctrl) {
	printf("  bRequestType: 0x%x (%s), bRequest: 0x%x, wValue: 0x%x,"
		" wIndex: 0x%x, wLength: %d\n", ctrl->bRequestType,
		(ctrl->bRequestType & USB_DIR_IN) ? "IN" : "OUT",
		ctrl->bRequest, ctrl->wValue, ctrl->wIndex, ctrl->wLength);

	switch (ctrl->bRequestType & USB_TYPE_MASK) {
	case USB_TYPE_STANDARD:
		printf("  type = USB_TYPE_STANDARD\n");
		break;
	case FTDI_SIO_REQUEST_TYPE:
		printf("  type = FTDI_SIO_REQUEST_TYPE\n");
		break;
	default:
		printf("  type = unknown = %d\n", (int)ctrl->bRequestType);
		break;
	}

	switch (ctrl->bRequestType & USB_TYPE_MASK) {
	case USB_TYPE_STANDARD:
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
	case FTDI_SIO_REQUEST_TYPE:
		switch (ctrl->bRequest) {
		case FTDI_SIO_GET_LATENCY_TIMER:
			printf("  req = FTDI_SIO_GET_LATENCY_TIMER\n");
			break;
		case FTDI_SIO_SET_LATENCY_TIMER:
			printf("  req = FTDI_SIO_SET_LATENCY_TIMER\n");
			break;
		case FTDI_SIO_READ_EEPROM:
			printf("  req = FTDI_SIO_READ_EEPROM\n");
			break;
		default:
			printf("  req = unknown = 0x%x\n", ctrl->bRequest);
			break;
		}
		break;
	default:
		printf("  req = unknown = 0x%x\n", ctrl->bRequest);
		break;
	}
}

/*----------------------------------------------------------------------*/

#define BCD_USB		0x0200
#define BCD_DEVICE	0x0600

#define USB_VENDOR	0x0403	// Future Technology Devices International, Ltd
#define USB_PRODUCT	0x6014	// FT232H Single HS USB-UART/FIFO IC

#define STRING_ID_MANUFACTURER	0
#define STRING_ID_PRODUCT	1
#define STRING_ID_SERIAL	2
#define STRING_ID_CONFIG	3
#define STRING_ID_INTERFACE	4

#define EP_MAX_PACKET_CONTROL	64
#define EP_MAX_PACKET_BULK	512

// Assigned dynamically.
#define EP_NUM_BULK_OUT	0x0
#define EP_NUM_BULK_IN	0x0

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
	.iConfiguration = 	STRING_ID_CONFIG,
	.bmAttributes =		USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
	.bMaxPower =		0x32,
};

struct usb_interface_descriptor usb_interface = {
	.bLength =		USB_DT_INTERFACE_SIZE,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bInterfaceNumber =	0,
	.bAlternateSetting =	0,
	.bNumEndpoints =	2,
	.bInterfaceClass =	USB_CLASS_VENDOR_SPEC,
	.bInterfaceSubClass =	USB_CLASS_VENDOR_SPEC,
	.bInterfaceProtocol =	USB_CLASS_VENDOR_SPEC,
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

int build_config(char *data, int length, bool other_speed) {
	struct usb_config_descriptor *config =
		(struct usb_config_descriptor *)data;
	int total_length = 0;

	assert(length >= sizeof(usb_config));
	memcpy(data, &usb_config, sizeof(usb_config));
	data += sizeof(usb_config);
	length -= sizeof(usb_config);
	total_length += sizeof(usb_config);

	assert(length >= sizeof(usb_interface));
	memcpy(data, &usb_interface, sizeof(usb_interface));
	data += sizeof(usb_interface);
	length -= sizeof(usb_interface);
	total_length += sizeof(usb_interface);

	assert(length >= USB_DT_ENDPOINT_SIZE);
	memcpy(data, &usb_endpoint_bulk_out, USB_DT_ENDPOINT_SIZE);
	data += USB_DT_ENDPOINT_SIZE;
	length -= USB_DT_ENDPOINT_SIZE;
	total_length += USB_DT_ENDPOINT_SIZE;

	assert(length >= USB_DT_ENDPOINT_SIZE);
	memcpy(data, &usb_endpoint_bulk_in, USB_DT_ENDPOINT_SIZE);
	data += USB_DT_ENDPOINT_SIZE;
	length -= USB_DT_ENDPOINT_SIZE;
	total_length += USB_DT_ENDPOINT_SIZE;

	config->wTotalLength = __cpu_to_le16(total_length);
	printf("config->wTotalLength: %d\n", total_length);

	return total_length;
}

/*----------------------------------------------------------------------*/

bool assign_ep_address(struct usb_raw_ep_info *info,
				struct usb_endpoint_descriptor *ep) {
	if (usb_endpoint_num(ep) != 0)
		return false;  // Already assigned.
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
	}

	int bulk_out_addr = usb_endpoint_num(&usb_endpoint_bulk_out);
	assert(bulk_out_addr != 0);

	int bulk_in_addr = usb_endpoint_num(&usb_endpoint_bulk_in);
	assert(bulk_in_addr != 0);
}

/*----------------------------------------------------------------------*/

// Data structures for control and endpoint operations
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

int ep_bulk_out = -1;
int ep_bulk_in  = -1;

pthread_t ep_bulk_out_thread;
pthread_t ep_bulk_in_thread;

atomic_bool ep_bulk_out_en = ATOMIC_VAR_INIT(false);
atomic_bool ep_bulk_in_en = ATOMIC_VAR_INIT(false);

void *ep_bulk_out_loop(void *arg) {
	int fd = (int)(long)arg;
	struct usb_raw_bulk_io io;

	while (!atomic_load(&ep_bulk_out_en));
	while (true) {
		assert(ep_bulk_out != -1);
		io.inner.ep = ep_bulk_out;
		io.inner.flags = 0;
		io.inner.length = sizeof(io.data);

		int rv = usb_raw_ep_read(fd, (struct usb_raw_ep_io *)&io);
		(void) rv; // printf("bulk_out: read %d bytes\n", rv);
	}

	return NULL;
}

void *ep_bulk_in_loop(void *arg) {
	int fd = (int)(long)arg;
	struct usb_raw_bulk_io io;

	while (!atomic_load(&ep_bulk_in_en));
	while (true) {
		assert(ep_bulk_in != -1);
		io.inner.ep = ep_bulk_in;
		io.inner.flags = 0;
		io.inner.length = sizeof(io.data);

		for (int i = 0; i < sizeof(io.data); i++)
			io.data[i] = (i % EP_MAX_PACKET_BULK) % 63;

		int rv = usb_raw_ep_write(fd, (struct usb_raw_ep_io *)&io);
		(void) rv; // printf("bulk_in: wrote %d bytes\n", rv);

		sleep(1);
	}

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
						sizeof(io->data), false);
				return true;
			case USB_DT_STRING:
				io->data[0] = 4;
				io->data[1] = USB_DT_STRING;
				if ((event->ctrl.wValue & 0xff) == 0) {
					io->data[2] = 0x09;
					io->data[3] = 0x04;
				} else {
					io->data[2] = 's';
					io->data[3] = 0x00;
				}
				io->inner.length = 4;
				return true;
			default:
				printf("fail: no response\n");
				exit(EXIT_FAILURE);
			}
			break;
		case USB_REQ_SET_CONFIGURATION:
			if (ep_bulk_out == -1) {
				ep_bulk_out = usb_raw_ep_enable(fd,
							&usb_endpoint_bulk_out);
			}
			if (ep_bulk_in == -1) {
				ep_bulk_in = usb_raw_ep_enable(fd,
							&usb_endpoint_bulk_in);
			}
			if (!ep_bulk_out_thread)
				pthread_create(&ep_bulk_out_thread, 0,
					       ep_bulk_out_loop, (void *)(long)fd);
			if (!ep_bulk_in_thread)
				pthread_create(&ep_bulk_in_thread, 0,
					       ep_bulk_in_loop, (void *)(long)fd);
			usb_raw_vbus_draw(fd, usb_config.bMaxPower);
			usb_raw_configure(fd);
			io->inner.length = 0;
			return true;
		default:
			printf("fail: no response\n");
			exit(EXIT_FAILURE);
		}
		break;
	case FTDI_SIO_REQUEST_TYPE:
		switch (event->ctrl.bRequest) {
		case FTDI_SIO_GET_LATENCY_TIMER:
			io->data[0] = 0x0a;
			io->inner.length = 1;
			return true;
		case FTDI_SIO_SET_LATENCY_TIMER:
			io->inner.length = 0;
			if (!gpiolib_set) {
				// Last request (# CONFIG_GPIOLIB is not set)
				atomic_store(&ep0_request_end, true);
			}
			return true;
		case FTDI_SIO_READ_EEPROM:
			io->data[0] = 0x23;
			io->data[1] = 0x10;
			io->inner.length = 2;
			if (gpiolib_set) {
				// Last request ( CONFIG_GPIOLIB=y )
				atomic_store(&ep0_request_end, true);
			}
			return true;
		default:
			printf("fail: no response\n");
			exit(EXIT_FAILURE);
		}
		break;

	default:
		printf("fail: no response\n");
		exit(EXIT_FAILURE);
	}
}

void ep0_loop(int fd) {
	while (true) {
		if (atomic_load(&ep0_request_end)) {
			// Prep for later
			// atomic_store(&ep_bulk_out_en, true);
			// atomic_store(&ep_bulk_in_en, true);

			sleep(1);
			// Exit
			return;
		}
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
}

int main(int argc, char **argv) {
	const char *device = "dummy_udc.0";
	const char *driver = "dummy_udc";
	if (argc >= 2) {
		if (!strcmp(argv[1], "--no-gpiolib")) {
			// # CONFIG_GPIOLIB is not set
			printf("--no-gpiolib\n");
			gpiolib_set = false;
		}
	}

	int fd = usb_raw_open();
	usb_raw_init(fd, USB_SPEED_HIGH, driver, device);
	usb_raw_run(fd);

	ep0_loop(fd);

	close(fd);

	return 0;
}
