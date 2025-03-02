#include "usb_gadget_tests.h"

/*----------------------------------------------------------------------*/

int usb_raw_open() {
	int fd = open("/dev/raw-gadget", O_RDWR);
	if (fd < 0) {
		perror("open()");
		exit(EXIT_FAILURE);
	}
	return fd;
}

void usb_raw_init(int fd, enum usb_device_speed speed,
			const char *driver, const char *device) {
	struct usb_raw_init arg;
	strcpy((char *)&arg.driver_name[0], driver);
	strcpy((char *)&arg.device_name[0], device);
	arg.speed = speed;
	int rv = ioctl(fd, USB_RAW_IOCTL_INIT, &arg);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_INIT)");
		exit(EXIT_FAILURE);
	}
}

void usb_raw_run(int fd) {
	int rv = ioctl(fd, USB_RAW_IOCTL_RUN, 0);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_RUN)");
		exit(EXIT_FAILURE);
	}
}

void usb_raw_event_fetch(int fd, struct usb_raw_event *event) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, event);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EVENT_FETCH)");
		exit(EXIT_FAILURE);
	}
}

int usb_raw_ep0_read(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_READ, io);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP0_READ)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep0_write(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_WRITE, io);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP0_WRITE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep_enable(int fd, struct usb_endpoint_descriptor *desc) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_ENABLE, desc);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP_ENABLE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep_disable(int fd, int ep) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_DISABLE, ep);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP_DISABLE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep_read(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_READ, io);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP_READ)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep_write(int fd, struct usb_raw_ep_io *io) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_WRITE, io);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP_WRITE)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

int usb_raw_ep_write_may_fail(int fd, struct usb_raw_ep_io *io) {
	return ioctl(fd, USB_RAW_IOCTL_EP_WRITE, io);
}

void usb_raw_configure(int fd) {
	int rv = ioctl(fd, USB_RAW_IOCTL_CONFIGURE, 0);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_CONFIGURED)");
		exit(EXIT_FAILURE);
	}
}

void usb_raw_vbus_draw(int fd, uint32_t power) {
	int rv = ioctl(fd, USB_RAW_IOCTL_VBUS_DRAW, power);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_VBUS_DRAW)");
		exit(EXIT_FAILURE);
	}
}

int usb_raw_eps_info(int fd, struct usb_raw_eps_info *info) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EPS_INFO, info);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EPS_INFO)");
		exit(EXIT_FAILURE);
	}
	return rv;
}

void usb_raw_ep0_stall(int fd) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP0_STALL, 0);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP0_STALL)");
		exit(EXIT_FAILURE);
	}
}

void usb_raw_ep_set_halt(int fd, int ep) {
	int rv = ioctl(fd, USB_RAW_IOCTL_EP_SET_HALT, ep);
	if (rv < 0) {
		perror("ioctl(USB_RAW_IOCTL_EP_SET_HALT)");
		exit(EXIT_FAILURE);
	}
}

/*----------------------------------------------------------------------*/

void log_event(struct usb_raw_event *event) {
	switch (event->type) {
	case USB_RAW_EVENT_CONNECT:
		printf("event: connect, length: %u\n", event->length);
		break;
	case USB_RAW_EVENT_CONTROL:
		printf("event: control, length: %u\n", event->length);
		log_control_request((struct usb_ctrlrequest *)&event->data[0]);
		break;
	case USB_RAW_EVENT_SUSPEND:
		printf("event: suspend\n");
		break;
	case USB_RAW_EVENT_RESUME:
		printf("event: resume\n");
		break;
	case USB_RAW_EVENT_RESET:
		printf("event: reset\n");
		break;
	case USB_RAW_EVENT_DISCONNECT:
		printf("event: disconnect\n");
		break;
	default:
		printf("event: %d (unknown), length: %u\n", event->type, event->length);
	}
}

/*----------------------------------------------------------------------*/
