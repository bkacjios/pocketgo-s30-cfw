/*
 * s30-uvc: the screen as a USB webcam. Feeds the UVC gadget function that
 * /usr/libexec/s30/usb-gadget adds to the USB gadget (Settings > USB > Video):
 * answers the host's probe/commit requests and, while the host streams,
 * sends the screen as shown (s30-capture.h, every plane: game, OSD, menu)
 * as 480x320 YUYV (BT.601, limited range).
 *
 * The function keeps the whole gadget off the bus until this daemon
 * subscribes to its events, so it must run whenever the function is bound.
 *
 * Bandwidth: one 1024-byte isochronous packet per microframe (the musb
 * controller has no high-bandwidth mode), about 8 MB/s, so a 300 KB frame
 * takes ~38 ms: the host gets up to ~26 frames per second whatever interval
 * it asks for. Frames are captured only when a buffer comes back, so a
 * frame is never older than the transfer before it.
 *
 * No tearing: a capture waits for the app's next page flip (the frame ring
 * libs30lease.so marks, s30-frames.h) and then the vblank after it, and
 * copies the planes then: the frame just flipped to stays on screen for a
 * frame of the app's, and the copy takes a few ms. A screen that doesn't
 * flip (nothing moving) is captured after FLIP_WAIT_MS.
 *
 * Usage: s30-uvc   (as root, for the display card's buffer handles)
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/usb/ch9.h>
#include <linux/usb/g_uvc.h>
#include <linux/usb/video.h>
#include <linux/videodev2.h>
#include "s30-capture.h"
#include "s30-frames.h"

#define WIDTH PANEL_H // landscape
#define HEIGHT PANEL_W
#define FRAME_SIZE (WIDTH * HEIGHT * 2)
#define MAX_PAYLOAD 1024 // the function's streaming_maxpacket
#define NBUFS 2
#define FLIP_WAIT_MS 40

/* The frame intervals the gadget's descriptor lists (100 ns units, 30 and 15
   fps); the first is the default */
static const uint32_t kIntervals[] = { 333333, 666666 };

static int sVideo = -1;
static S30Capture sCap;
static struct uvc_streaming_control sProbe, sCommit;
static int sControl;   // probe or commit: the selector of the SET_CUR whose data comes next
static int sStreaming; // buffers queued and the stream on
static void* sBuf[NBUFS];
static size_t sBufLen[NBUFS];
static uint64_t sNext; // when the next frame is due (ms)

static uint64_t now_ms(void) {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* The UVC gadget's video node (driver g_uvc) */
static int open_video(void) {
	int i;

	for (i = 0; i < 16; i++) {
		struct v4l2_capability cap;
		char path[32];
		int fd;

		snprintf(path, sizeof(path), "/dev/video%d", i);
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}
		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && strcmp((const char*)cap.driver, "g_uvc") == 0) {
			return fd;
		}
		close(fd);
	}
	return -1;
}

/* A probe/commit block for the one format and frame, at the interval nearest
   to `interval` */
static void fill_control(struct uvc_streaming_control* c, uint32_t interval) {
	uint32_t best = kIntervals[0];
	size_t i;

	for (i = 1; i < sizeof(kIntervals) / sizeof(kIntervals[0]); i++) {
		if ((interval > kIntervals[i] ? interval - kIntervals[i] : kIntervals[i] - interval) <
		    (interval > best ? interval - best : best - interval)) {
			best = kIntervals[i];
		}
	}
	memset(c, 0, sizeof(*c));
	c->bmHint = 1; // dwFrameInterval fixed
	c->bFormatIndex = 1;
	c->bFrameIndex = 1;
	c->dwFrameInterval = best;
	c->dwMaxVideoFrameSize = FRAME_SIZE;
	c->dwMaxPayloadTransferSize = MAX_PAYLOAD;
	c->bmFramingInfo = 3;
	c->bPreferedVersion = 1;
	c->bMaxVersion = 1;
}

/* ---------------------------------------------------------------------------
 * Frames
 * ------------------------------------------------------------------------- */

/* Until the app's next page flip, FLIP_WAIT_MS at most. The ring is opened
   each time: s30-display makes a new one when it restarts. */
static void wait_flip(void) {
	int fd = open(S30_FRAMES_PATH, O_RDONLY | O_CLOEXEC);
	const S30Frames* ring = NULL;
	uint64_t end = now_ms() + FLIP_WAIT_MS;

	if (fd >= 0) {
		void* m = mmap(NULL, sizeof(S30Frames), PROT_READ, MAP_SHARED, fd, 0);

		ring = m == MAP_FAILED ? NULL : m;
		close(fd);
	}
	if (ring == NULL) {
		return;
	}
	{
		uint32_t start = ring->count;
		struct timespec ms1 = { 0, 1000000 };

		while (ring->count == start && now_ms() < end) {
			nanosleep(&ms1, NULL);
		}
	}
	munmap((void*)ring, sizeof(S30Frames));
}

/* The screen, rotated to landscape, as YUYV into `out` */
static void capture_yuyv(uint8_t* out) {
	static uint32_t screen[PANEL_W * PANEL_H];
	int x, y;

	wait_flip();
	s30_capture(&sCap, 0, 1, screen);
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < WIDTH; x += 2) {
			uint32_t c0 = screen[(PANEL_H - 1 - x) * PANEL_W + y];
			uint32_t c1 = screen[(PANEL_H - 2 - x) * PANEL_W + y];
			int r0 = (int)(c0 >> 16) & 255, g0 = (int)(c0 >> 8) & 255, b0 = (int)c0 & 255;
			int r1 = (int)(c1 >> 16) & 255, g1 = (int)(c1 >> 8) & 255, b1 = (int)c1 & 255;
			int r = (r0 + r1) >> 1, g = (g0 + g1) >> 1, b = (b0 + b1) >> 1;

			out[0] = (uint8_t)(((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16);
			out[1] = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
			out[2] = (uint8_t)(((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16);
			out[3] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
			out += 4;
		}
	}
}

static int queue_buffer(int i) {
	struct v4l2_buffer b = { 0 };

	capture_yuyv(sBuf[i]);
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = (uint32_t)i;
	b.bytesused = FRAME_SIZE;
	if (ioctl(sVideo, VIDIOC_QBUF, &b) != 0) {
		perror("s30-uvc: QBUF");
		return -1;
	}
	return 0;
}

static void stream_off(void) {
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT, i;
	struct v4l2_requestbuffers rb = { 0 };

	if (!sStreaming) {
		return;
	}
	sStreaming = 0;
	ioctl(sVideo, VIDIOC_STREAMOFF, &type);
	for (i = 0; i < NBUFS; i++) {
		if (sBuf[i] != NULL) {
			munmap(sBuf[i], sBufLen[i]);
			sBuf[i] = NULL;
		}
	}
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	rb.memory = V4L2_MEMORY_MMAP;
	ioctl(sVideo, VIDIOC_REQBUFS, &rb);
	fprintf(stderr, "s30-uvc: stream off\n");
}

/* The host selected the streaming alternate setting: the format, the
   committed interval, buffers filled with the screen, then the stream on */
static void stream_on(void) {
	struct v4l2_format fmt = { 0 };
	struct v4l2_streamparm parm = { 0 };
	struct v4l2_requestbuffers rb = { 0 };
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT, i;

	stream_off();
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = WIDTH;
	fmt.fmt.pix.height = HEIGHT;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = WIDTH * 2;
	fmt.fmt.pix.sizeimage = FRAME_SIZE;
	if (ioctl(sVideo, VIDIOC_S_FMT, &fmt) != 0) {
		perror("s30-uvc: S_FMT");
		return;
	}
	parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	parm.parm.output.timeperframe.numerator = sCommit.dwFrameInterval ? sCommit.dwFrameInterval : kIntervals[0];
	parm.parm.output.timeperframe.denominator = 10000000;
	ioctl(sVideo, VIDIOC_S_PARM, &parm);
	rb.count = NBUFS;
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	rb.memory = V4L2_MEMORY_MMAP;
	// The gadget's REQBUFS returns the buffer count, not 0
	if (ioctl(sVideo, VIDIOC_REQBUFS, &rb) < 0 || rb.count < NBUFS) {
		perror("s30-uvc: REQBUFS");
		return;
	}
	sStreaming = 1; // stream_off() cleans up from here
	for (i = 0; i < NBUFS; i++) {
		struct v4l2_buffer b = { 0 };

		b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = (uint32_t)i;
		if (ioctl(sVideo, VIDIOC_QUERYBUF, &b) != 0 ||
		    (sBuf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, sVideo, b.m.offset)) ==
		        MAP_FAILED) {
			perror("s30-uvc: buffer");
			sBuf[i] = NULL;
			stream_off();
			return;
		}
		sBufLen[i] = b.length;
		if (queue_buffer(i) != 0) {
			stream_off();
			return;
		}
	}
	if (ioctl(sVideo, VIDIOC_STREAMON, &type) != 0) {
		perror("s30-uvc: STREAMON");
		stream_off();
		return;
	}
	sNext = now_ms();
	fprintf(stderr, "s30-uvc: stream on, %u fps\n", 10000000u / (uint32_t)parm.parm.output.timeperframe.numerator);
}

/* A buffer the host has received: the next frame into it, at the committed
   interval at most */
static void refill(void) {
	struct v4l2_buffer b = { 0 };
	uint32_t interval = sCommit.dwFrameInterval ? sCommit.dwFrameInterval : kIntervals[0];
	uint64_t now = now_ms();

	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	b.memory = V4L2_MEMORY_MMAP;
	if (ioctl(sVideo, VIDIOC_DQBUF, &b) != 0) {
		if (errno != EAGAIN) {
			perror("s30-uvc: DQBUF");
		}
		return;
	}
	sNext += interval / 10000;
	if (sNext < now) {
		sNext = now; // behind (the bus is slower than the interval): no catching up
	}
	queue_buffer((int)b.index);
}

/* ---------------------------------------------------------------------------
 * Control requests
 * ------------------------------------------------------------------------- */

/* A class request. The function rewrites wIndex: 1 for the streaming
   interface; for the control interface the entity ID in the high byte. Only
   probe and commit are answered; everything else stalls (no controls). */
static void setup(const struct usb_ctrlrequest* req, struct uvc_request_data* resp) {
	int cs = req->wValue >> 8;
	struct uvc_streaming_control* c = (struct uvc_streaming_control*)resp->data;

	resp->length = -EL2HLT; // stall
	if (req->wIndex != UVC_STRING_STREAMING_IDX || (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL)) {
		return;
	}
	switch (req->bRequest) {
		case UVC_SET_CUR:
			sControl = cs;
			resp->length = req->wLength; // the data arrives as a DATA event
			break;
		case UVC_GET_CUR:
			*c = cs == UVC_VS_PROBE_CONTROL ? sProbe : sCommit;
			resp->length = req->wLength;
			break;
		case UVC_GET_MIN:
		case UVC_GET_DEF:
			fill_control(c, kIntervals[0]);
			resp->length = req->wLength;
			break;
		case UVC_GET_MAX:
			fill_control(c, kIntervals[sizeof(kIntervals) / sizeof(kIntervals[0]) - 1]);
			resp->length = req->wLength;
			break;
		case UVC_GET_RES:
			memset(c, 0, sizeof(*c));
			resp->length = req->wLength;
			break;
		case UVC_GET_LEN:
			resp->data[0] = 0;
			resp->data[1] = sizeof(struct uvc_streaming_control);
			resp->length = 2;
			break;
		case UVC_GET_INFO:
			resp->data[0] = UVC_CONTROL_CAP_GET | UVC_CONTROL_CAP_SET;
			resp->length = 1;
			break;
	}
	if (resp->length > (int)sizeof(struct uvc_streaming_control)) {
		resp->length = sizeof(struct uvc_streaming_control);
	}
}

/* SET_CUR's data: probe takes the host's interval (the rest is fixed),
   commit makes it the stream's */
static void data(const struct uvc_request_data* d) {
	struct uvc_streaming_control host = { 0 };

	memcpy(&host, d->data, d->length < (int)sizeof(host) ? (size_t)(d->length > 0 ? d->length : 0) : sizeof(host));
	fill_control(sControl == UVC_VS_COMMIT_CONTROL ? &sCommit : &sProbe,
	             host.dwFrameInterval ? host.dwFrameInterval : kIntervals[0]);
}

static void event(void) {
	struct v4l2_event ev;
	struct uvc_event* uev = (struct uvc_event*)&ev.u.data;
	struct uvc_request_data resp;

	if (ioctl(sVideo, VIDIOC_DQEVENT, &ev) != 0) {
		return;
	}
	switch (ev.type) {
		case UVC_EVENT_SETUP:
			memset(&resp, 0, sizeof(resp));
			setup(&uev->req, &resp);
			if (ioctl(sVideo, UVCIOC_SEND_RESPONSE, &resp) != 0) {
				perror("s30-uvc: SEND_RESPONSE");
			}
			break;
		case UVC_EVENT_DATA:
			data(&uev->data);
			break;
		case UVC_EVENT_STREAMON:
			stream_on();
			break;
		case UVC_EVENT_STREAMOFF:
		case UVC_EVENT_DISCONNECT:
			stream_off();
			break;
	}
}

static volatile sig_atomic_t sQuit;

static void on_signal(int sig) {
	(void)sig;
	sQuit = 1;
}

int main(void) {
	// SETUP last: subscribing to it connects the gadget
	static const uint32_t kEvents[] = { UVC_EVENT_DATA, UVC_EVENT_STREAMON, UVC_EVENT_STREAMOFF, UVC_EVENT_DISCONNECT,
		                                UVC_EVENT_SETUP };
	struct sigaction sa = { 0 };
	size_t i;

	sVideo = open_video();
	if (sVideo < 0) {
		fprintf(stderr, "s30-uvc: no UVC gadget video device\n");
		return 1;
	}
	if (s30_capture_open(&sCap) != 0) {
		return 1;
	}
	fill_control(&sProbe, kIntervals[0]);
	sCommit = sProbe;
	for (i = 0; i < sizeof(kEvents) / sizeof(kEvents[0]); i++) {
		struct v4l2_event_subscription sub = { 0 };

		sub.type = kEvents[i];
		if (ioctl(sVideo, VIDIOC_SUBSCRIBE_EVENT, &sub) != 0) {
			perror("s30-uvc: SUBSCRIBE_EVENT");
			return 1;
		}
	}
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	while (!sQuit) {
		uint64_t now = now_ms();
		int due = sStreaming && now >= sNext;
		struct pollfd p = { sVideo, (short)(POLLPRI | (due ? POLLOUT : 0)), 0 };
		int timeout = sStreaming && !due ? (int)(sNext - now) : -1;

		if (poll(&p, 1, timeout) < 0) {
			if (errno != EINTR) {
				perror("s30-uvc: poll");
				break;
			}
			continue;
		}
		if (p.revents & POLLPRI) {
			event();
		}
		if ((p.revents & POLLOUT) && sStreaming) {
			refill();
		}
	}
	stream_off();
	close(sVideo); // unsubscribes: the gadget leaves the bus
	return 0;
}
