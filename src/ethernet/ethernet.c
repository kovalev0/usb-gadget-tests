// SPDX-License-Identifier: Apache-2.0
//
// Emulates a USB-Ethernet device (VID: 0x0bda, PID: 0x8150),
// simulating device enumeration.
// It uses a vendor-specific protocol (USB 2.1) over a high-speed
// connection. Three endpoints (bulk IN, bulk OUT, interrupt IN)
// are configured.
// Handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION) and vendor-specific requests.
// Note: CDC transmissions are not implemented due to gadget's
// single-configuration limit (real device uses 2 configurations,
// second includes CDC).
//
// Vasiliy Kovalev <kovalev@altlinux.org>

#include "../usb_gadget_tests.h"

/*----------------------------------------------------------------------*/

#define	RTL8150_REQ_GET_REGS	0x05
#define	RTL8150_REQ_SET_REGS	0x05
#define	RTL8150_REQ_SGET_REGS	0x05
#define	IDR			0x0120
#define	CR			0x012e

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
	case USB_TYPE_VENDOR:
		printf("  type = USB_TYPE_VENDOR\n");
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
			case USB_DT_BOS:
				printf("  desc = USB_DT_BOS\n");
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
		switch (ctrl->bRequest) {
		case RTL8150_REQ_SGET_REGS:
			if (ctrl->wValue == IDR)
				printf("  req = RTL8150_REQ_GET_REGS\n");
			else if (ctrl->wValue == CR)
				printf("  req = RTL8150_REQ_SET_REGS\n");
			else
				printf("  RTL8150_REQ_SGET_REGS: wValue = unknown = 0x%x\n",
					ctrl->bRequest);
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

#define BCD_USB		0x0210

#define USB_VENDOR	0x0bda	// Realtek
#define USB_PRODUCT	0x8150	// RTL8150


#define STRING_ID_MANUFACTURER	0
#define STRING_ID_PRODUCT	1
#define STRING_ID_SERIAL	2
#define STRING_ID_CONFIG	3
#define STRING_ID_INTERFACE	4

#define EP_MAX_PACKET_CONTROL	64
#define EP_MAX_PACKET_BULK	512
#define EP_MAX_PACKET_INT	2

// Hardcode
#define EP_NUM_BULK_IN	0x1
#define EP_NUM_BULK_OUT	0x2
#define EP_NUM_INT_IN	0x3

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
	.bcdDevice =		0,
	.iManufacturer =	STRING_ID_MANUFACTURER,
	.iProduct =		STRING_ID_PRODUCT,
	.iSerialNumber =	STRING_ID_SERIAL,
//	.bNumConfigurations =	2,	// Not support gadget
	.bNumConfigurations =	1,
};

// USB Configuration Descriptor for RTL8150 (without cdc)
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

// USB Interface Descriptor for Vendor-Specific Class
struct usb_interface_descriptor usb_interface = {
	.bLength =		USB_DT_INTERFACE_SIZE,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bInterfaceNumber =	0,
	.bAlternateSetting =	0,
	.bNumEndpoints =	3,
	.bInterfaceClass =	USB_CLASS_VENDOR_SPEC,
	.bInterfaceSubClass =	USB_SUBCLASS_VENDOR_SPEC,
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

struct usb_endpoint_descriptor usb_endpoint_int_in = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN | EP_NUM_INT_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	EP_MAX_PACKET_INT,
	.bInterval =		10,
};

struct usb_bos_descriptor usb_bos = {
	.bLength =		USB_DT_BOS_SIZE,
	.bDescriptorType =	USB_DT_BOS,
	.wTotalLength =		USB_DT_BOS_SIZE,
	.bNumDeviceCaps =	0,
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

	assert(length >= USB_DT_ENDPOINT_SIZE);
	memcpy(data, &usb_endpoint_int_in, USB_DT_ENDPOINT_SIZE);
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
// debug
/*
	for (int i = 0; i < num; i++) {
		printf("ep #%d:\n", i);
		printf("  name: %s\n", &info.eps[i].name[0]);
		printf("  addr: %u\n", info.eps[i].addr);
		printf("  type: %s %s %s\n",
			info.eps[i].caps.type_iso ? "iso" : "___",
			info.eps[i].caps.type_bulk ? "blk" : "___",
			info.eps[i].caps.type_int ? "int" : "___");
		printf("  dir : %s %s\n",
			info.eps[i].caps.dir_in ? "in " : "___",
			info.eps[i].caps.dir_out ? "out" : "___");
		printf("  maxpacket_limit: %u\n",
			info.eps[i].limits.maxpacket_limit);
		printf("  max_streams: %u\n", info.eps[i].limits.max_streams);
	}
*/

	for (int i = 0; i < num; i++) {
		if (assign_ep_address(&info.eps[i], &usb_endpoint_bulk_out))
			continue;
		if (assign_ep_address(&info.eps[i], &usb_endpoint_bulk_in))
			continue;
		if (assign_ep_address(&info.eps[i], &usb_endpoint_int_in))
			continue;
	}

	int bulk_out_addr = usb_endpoint_num(&usb_endpoint_bulk_out);
	assert(bulk_out_addr != 0);
//	printf("bulk_out: addr = %u\n", bulk_out_addr);

	int bulk_in_addr = usb_endpoint_num(&usb_endpoint_bulk_in);
	assert(bulk_in_addr != 0);
//	printf("bulk_in: addr = %u\n", bulk_in_addr);

	int ep_int_in_addr = usb_endpoint_num(&usb_endpoint_int_in);
	assert(ep_int_in_addr != 0);
//	printf("ep_int_in: addr = %u\n", ep_int_in_addr);
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

struct usb_raw_int_io {
	struct usb_raw_ep_io		inner;
	char				data[EP_MAX_PACKET_INT];
};

int ep_bulk_out	= -1;
int ep_bulk_in	= -1;

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

int ep_int_in = -1;
pthread_t ep_int_in_thread;

atomic_bool ep_int_in_en = ATOMIC_VAR_INIT(false);

int ep_int_in_send_packet(int fd, struct usb_raw_int_io* io) {
	int rv;

	rv = usb_raw_ep_write_may_fail(fd, (struct usb_raw_ep_io *)io);
	if(rv == io->inner.length) {
		// printf("ep_int_in: send: %d\n", rv);
		return 0;
	}
	if (rv < 0 && errno == ESHUTDOWN) {
		printf("ep_int_in: device was likely reset, exiting\n");
	} else if (rv < 0) {
		perror("usb_raw_ep_write_may_fail()");
		exit(EXIT_FAILURE);
	}

	return rv;
}

void *ep_int_in_loop(void *arg) {
	int fd = (int)(long)arg;

	struct usb_raw_int_io io;
	io.inner.ep = ep_int_in;
	io.inner.flags = 0;
	io.inner.length = EP_MAX_PACKET_INT;

	while (!atomic_load(&ep_int_in_en));
	while(true){
		sleep(1);
		memcpy(&io.inner.data[0], "\x22\x10", 2);
		ep_int_in_send_packet(fd,&io);
	}
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
					io->data[2] = 'e';
					io->data[3] = 0x00;
				}
				io->inner.length = 4;
				return true;
			case USB_DT_BOS:
				memcpy(&io->data[0], &usb_bos, sizeof(usb_bos));
				io->inner.length = event->ctrl.wLength;
				return true;
			default:
				printf("fail: no response\n");
				exit(EXIT_FAILURE);
			}
			break;
		case USB_REQ_GET_CONFIGURATION:
			io->inner.length =
				build_config(&io->data[0],
					sizeof(io->data), false);
			return true;
		case USB_REQ_SET_CONFIGURATION:
			if (ep_bulk_out == -1) {
				ep_bulk_out = usb_raw_ep_enable(fd,
							&usb_endpoint_bulk_out);
				// printf("bulk_out: ep = #%d\n", ep_bulk_out);
			}
			if (ep_bulk_in == -1) {
				ep_bulk_in = usb_raw_ep_enable(fd,
							&usb_endpoint_bulk_in);
				// printf("bulk_in: ep = #%d\n", ep_bulk_in);
			}
			if (ep_int_in == -1) {
				ep_int_in = usb_raw_ep_enable(fd,
							&usb_endpoint_int_in);
				// printf("int_in: ep = #%d\n", ep_int_in);
			}
			if (!ep_bulk_out_thread)
				pthread_create(&ep_bulk_out_thread, 0,
					       ep_bulk_out_loop, (void *)(long)fd);
			if (!ep_bulk_in_thread)
				pthread_create(&ep_bulk_in_thread, 0,
					       ep_bulk_in_loop, (void *)(long)fd);
			if (!ep_int_in_thread)
				pthread_create(&ep_int_in_thread, 0,
					       ep_int_in_loop, (void *)(long)fd);
			usb_raw_vbus_draw(fd, usb_config.bMaxPower);
			usb_raw_configure(fd);
			io->inner.length = 0;
			return true;
		default:
			printf("fail: no response\n");
			exit(EXIT_FAILURE);
		}
		break;
	case USB_TYPE_VENDOR:
		switch (event->ctrl.bRequest) {
		case RTL8150_REQ_SGET_REGS:
			if (event->ctrl.wValue == IDR) {
				io->data[0] = 0x05;
				io->inner.length = event->ctrl.wLength;
				// Last request
				atomic_store(&ep0_request_end, true);
				return true;
			}
			else if (event->ctrl.wValue == CR) {
				io->data[0] = 0x20;
				io->inner.length = 1;
				return true;
			}
			printf("fail: unknown vendor wValue=0x%04x, wIndex=0x%04x\n",
					event->ctrl.wValue, event->ctrl.wIndex);
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
			// Debug
			// atomic_store(&ep_bulk_out_en, true);
			// atomic_store(&ep_bulk_in_en, true);
			// atomic_store(&ep_int_in_en, true);

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
	if (argc >= 2)
		device = argv[1];
	if (argc >= 3)
		driver = argv[2];

	int fd = usb_raw_open();
	usb_raw_init(fd, USB_SPEED_HIGH, driver, device);
	usb_raw_run(fd);

	ep0_loop(fd);

	close(fd);

	return 0;
}
