// SPDX-License-Identifier: Apache-2.0
//
// Emulates a USB HID mouse (VID: 0x093a, PID: 0x2510), simulating
// device enumeration and right-click, cursor movement, and scrolling.
// It uses the USB HID protocol (USB 1.1) over a high-speed connection.
// A single interrupt IN endpoint sends HID reports with button states,
// coordinates, and wheel data.
// It handles standard USB control requests (e.g., GET_DESCRIPTOR,
// SET_CONFIGURATION) and HID-specific requests (e.g., SET_IDLE),
// sending events after last hid request.
//
// Vasiliy Kovalev <kovalev@altlinux.org>

#include "../usb_gadget_tests.h"

/*----------------------------------------------------------------------*/

#include <linux/hid.h>

struct hid_class_descriptor {
	__u8  bDescriptorType;
	__le16 wDescriptorLength;
} __attribute__ ((packed));

struct hid_descriptor {
	__u8  bLength;
	__u8  bDescriptorType;
	__le16 bcdHID;
	__u8  bCountryCode;
	__u8  bNumDescriptors;

	struct hid_class_descriptor desc[1];
} __attribute__ ((packed));

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
	case USB_TYPE_CLASS:
		printf("  type = USB_TYPE_CLASS\n");
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
			case HID_DT_REPORT:
				printf("  descriptor = HID_DT_REPORT\n");
				return;
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
		switch (ctrl->bRequest) {
		case HID_REQ_SET_IDLE:
			printf("  req = HID_REQ_SET_IDLE\n");
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

#define BCD_USB		0x0110

#define USB_VENDOR	0x093a
#define USB_PRODUCT	0x2510

#define STRING_ID_MANUFACTURER	0
#define STRING_ID_PRODUCT	1
#define STRING_ID_SERIAL	2
#define STRING_ID_CONFIG	3
#define STRING_ID_INTERFACE	4

#define EP_MAX_PACKET_CONTROL	8
#define EP_MAX_PACKET_INT	4

// Assigned dynamically.
#define EP_NUM_INT_IN	0x0

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
	.bcdDevice =		0x100,
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
	.bNumEndpoints =	1,
	.bInterfaceClass =	USB_CLASS_HID,
	.bInterfaceSubClass =	1,
	.bInterfaceProtocol =	2,	// Mouse
	.iInterface =		STRING_ID_INTERFACE,
};

struct usb_endpoint_descriptor usb_endpoint = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN | EP_NUM_INT_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	EP_MAX_PACKET_INT,
	.bInterval =		10,
};

char usb_hid_report[] = {
	0x05, 0x01,                    // Usage Page (Generic Desktop)		0
	0x09, 0x02,                    // Usage (Mouse)				2
	0xa1, 0x01,                    // Collection (Application)		4
	0x09, 0x01,                    //  Usage (Pointer)			6
	0xa1, 0x00,                    //    Collection (Physical)		8
	0x05, 0x09,                    //      Usage Page (Buttons)		10
	0x19, 0x01,                    //      Usage Minimum (1)		12
	0x29, 0x03,                    //      Usage Maximum (3)		14
	0x15, 0x00,                    //      Logical Minimum (0)		16
	0x25, 0x01,                    //      Logical Maximum (1)		18
	0x75, 0x01,                    //      Report Size (1)			20
	0x95, 0x03,                    //      Report Count (3)			22
	0x81, 0x02,                    //      Input (Data,Var,Abs)		24
	0x75, 0x05,                    //      Report Size (5)			26
	0x95, 0x01,                    //      Report Count (1)			28
	0x81, 0x01,                    //      Input (Cnst,Arr,Abs)		30
	0x05, 0x01,                    //      Usage Page (Generic Desktop)	32
	0x09, 0x30,                    //      Usage (X)			34
	0x09, 0x31,                    //      Usage (Y)			36
	0x09, 0x38,                    //      Usage (Wheel)			38
	0x15, 0x81,                    //      Logical Minimum (-127)		40
	0x25, 0x7f,                    //      Logical Maximum (127)		42
	0x75, 0x08,                    //      Report Size (8)			44
	0x95, 0x03,                    //      Report Count (3)			46
	0x81, 0x06,                    //      Input (Data,Var,Rel)		48
	0xc0,                          //    End Collection			50
	0xc0,                          // End Collection			51
};

struct hid_descriptor usb_hid = {
	.bLength =		9,
	.bDescriptorType =	HID_DT_HID,
	.bcdHID =		__constant_cpu_to_le16(0x0111), // bcdHID = 1.11 -> 0x0111
	.bCountryCode =		0,
	.bNumDescriptors =	1,
	.desc =			{
		{
			.bDescriptorType =	HID_DT_REPORT,
			.wDescriptorLength =	sizeof(usb_hid_report),
		}
	},
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

	assert(length >= sizeof(usb_hid));
	memcpy(data, &usb_hid, sizeof(usb_hid));
	data += sizeof(usb_hid);
	length -= sizeof(usb_hid);
	total_length += sizeof(usb_hid);

	assert(length >= USB_DT_ENDPOINT_SIZE);
	memcpy(data, &usb_endpoint, USB_DT_ENDPOINT_SIZE);
	data += USB_DT_ENDPOINT_SIZE;
	length -= USB_DT_ENDPOINT_SIZE;
	total_length += USB_DT_ENDPOINT_SIZE;

	config->wTotalLength = __cpu_to_le16(total_length);
	printf("config->wTotalLength: %d\n", total_length);

	if (other_speed)
		config->bDescriptorType = USB_DT_OTHER_SPEED_CONFIG;

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
		if (assign_ep_address(&info.eps[i], &usb_endpoint))
			continue;
	}

	int ep_int_in_addr = usb_endpoint_num(&usb_endpoint);
	assert(ep_int_in_addr != 0);
//	printf("ep_int_in: addr = %u\n", ep_int_in_addr);
}

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

int ep_int_in = -1;
pthread_t ep_int_in_thread;
bool ep_int_in_thread_spawned = false;

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

	// Right mouse button press packet
	char press_right_click[4]  = {0x02, 0x00, 0x00, 0x00};
	// Mouse button release packet (no buttons pressed)
	char release_click[4]      = {0x00, 0x00, 0x00, 0x00};
	// Cursor movement packets
	char coords[3][4] = {
		{0x00, 0xFF, 0x00, 0x00}, // Position 0
		{0x00, 0x00, 0xFF, 0x00}, // Position 1
		{0x00, 0xFF, 0xFF, 0x00}  // Position 2
	};
	int num_coords  = 3;

	// Scroll state packets
	char scroll[2][4] = {
		{0x00, 0x00, 0x00, 0x01}, // Scroll up
		{0x00, 0x00, 0x00, 0xFF}  // Scroll down
	};
	int num_scroll  = 2;

	int num_attempt = 3;

	while (true) {
		for (int attempt = 0; attempt < num_attempt; attempt++) {
			// 300 ms delay
			usleep(300000);

			// Send right-click press
			memcpy(&io.inner.data[0], press_right_click, 4);
			ep_int_in_send_packet(fd,&io);

			for (int i = 0; i < num_coords; i++) {
				// 100 ms delay
				usleep(100000);

				// Send cursor movement
				memcpy(io.inner.data, coords[i], 4);
				ep_int_in_send_packet(fd,&io);
			}
			for (int i = 0; i < num_scroll; i++) {
				// 100 ms delay
				usleep(100000);

				// Send scroll state
				memcpy(io.inner.data, scroll[i], 4);
				ep_int_in_send_packet(fd,&io);
			}
			// 100 ms delay
			usleep(100000);

			// Send button release
			memcpy(io.inner.data, release_click, 4);
			ep_int_in_send_packet(fd,&io);
		}
		// Change coordinates
		for (int j = 0; j < 3; j++) {
			coords[j][1] += 10 * (j+1);
			coords[j][2] += 20 * (j+1);
		}
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
					io->data[2] = 'M';
					io->data[3] = 0x00;
				}
				io->inner.length = 4;
				return true;
			case HID_DT_REPORT:
				memcpy(&io->data[0], &usb_hid_report[0],
							sizeof(usb_hid_report));
				io->inner.length = sizeof(usb_hid_report);
				// Last request
				if (event->ctrl.wValue == 0x2200)
					atomic_store(&ep0_request_end, true);
				return true;
			default:
				printf("fail: no response\n");
				exit(EXIT_FAILURE);
			}
			break;
		case USB_REQ_SET_CONFIGURATION:
			ep_int_in = usb_raw_ep_enable(fd, &usb_endpoint);
			printf("ep0: ep_int_in enabled: %d\n", ep_int_in);
			int rv = pthread_create(&ep_int_in_thread, 0,
					ep_int_in_loop, (void *)(long)fd);
			if (rv != 0) {
				perror("pthread_create(ep_int_in)");
				exit(EXIT_FAILURE);
			}
			ep_int_in_thread_spawned = true;
			printf("ep0: spawned ep_int_in thread\n");
			usb_raw_vbus_draw(fd, usb_config.bMaxPower);
			usb_raw_configure(fd);
			io->inner.length = 0;
			return true;
		default:
			printf("fail: no response\n");
			exit(EXIT_FAILURE);
		}
		break;
	case USB_TYPE_CLASS:
		switch (event->ctrl.bRequest) {
		case HID_REQ_SET_IDLE:
			io->inner.length = 0;
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
	while(true) {
		if (atomic_load(&ep0_request_end)) {
			// Enable Mouse events
			atomic_store(&ep_int_in_en, true);
			// Waiting for the completion sending
			sleep(2);
			break;
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

		if (event.inner.type == USB_RAW_EVENT_RESET) {
			if (ep_int_in_thread_spawned) {
				printf("ep0: stopping ep_int_in thread\n");
				// Even though normally, on a device reset,
				// the endpoint threads should exit due to
				// ESHUTDOWN, let's also attempt to cancel
				// them just in case.
				pthread_cancel(ep_int_in_thread);
				int rv = pthread_join(ep_int_in_thread, NULL);
				if (rv != 0) {
					perror("pthread_join(ep_int_in)");
					exit(EXIT_FAILURE);
				}
				usb_raw_ep_disable(fd, ep_int_in);
				ep_int_in_thread_spawned = false;
				printf("ep0: stopped ep_int_in thread\n");
			}
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
