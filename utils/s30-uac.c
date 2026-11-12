/*
 * s30-uac: what the speaker plays, to the host through the UAC2 gadget
 * function (Settings > USB > Audio; /usr/libexec/s30/usb-gadget starts it).
 *
 * The codec copies AIF1's playback data back into AIF1's capture ("AIF1
 * Slot 0 Digital ADC Capture Switch": the AIF1 DA0 stereo mux, so on the
 * speaker it is the mono mix the speaker gets). AIF1 has one clock, so the
 * capture must run at the playback's rate, sample size and channels
 * (symmetric in the codec driver), and while it runs, no app can open
 * playback at another rate. So the capture is only open while an app plays
 * (/proc/asound/card0/pcm0p shows its parameters) and the host records (the
 * gadget's "Playback Rate" control is non-zero), and is closed as soon as
 * either stops.
 *
 * The capture, at the app's rate, is resampled (linear) to the host's. The
 * codec and the USB bus have their own clocks, so the ratio follows the
 * gadget's buffer: a little faster when it runs low, slower when it fills.
 *
 * Usage: s30-uac
 */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CODEC_PCM "hw:0,0"
#define CHANNELS 2
#define TARGET_MS 40 // the gadget buffer's fill the ratio steers to
#define MAX_ADJUST 0.005

static volatile sig_atomic_t sQuit;

static void on_signal(int sig) {
	(void)sig;
	sQuit = 1;
}

/* The UAC2 gadget's sound card, -1 if none */
static int gadget_card(void) {
	int card = -1;

	while (snd_card_next(&card) == 0 && card >= 0) {
		char* name = NULL;

		if (snd_card_get_name(card, &name) == 0 && strstr(name, "UAC2") != NULL) {
			free(name);
			return card;
		}
		free(name);
	}
	return -1;
}

/* An integer control of a card, by interface and name; -1 if missing */
static long ctl_get(int card, snd_ctl_elem_iface_t iface, const char* name) {
	snd_ctl_t* ctl;
	snd_ctl_elem_value_t* v;
	char hw[16];
	long r = -1;

	snprintf(hw, sizeof(hw), "hw:%d", card);
	if (snd_ctl_open(&ctl, hw, 0) < 0) {
		return -1;
	}
	snd_ctl_elem_value_alloca(&v);
	snd_ctl_elem_value_set_interface(v, iface);
	snd_ctl_elem_value_set_name(v, name);
	if (snd_ctl_elem_read(ctl, v) == 0) {
		r = snd_ctl_elem_value_get_integer(v, 0);
	}
	snd_ctl_close(ctl);
	return r;
}

/* Both channels of a codec mixer control */
static void ctl_set2(const char* name, long value) {
	snd_ctl_t* ctl;
	snd_ctl_elem_value_t* v;

	if (snd_ctl_open(&ctl, "hw:0", 0) < 0) {
		return;
	}
	snd_ctl_elem_value_alloca(&v);
	snd_ctl_elem_value_set_interface(v, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_value_set_name(v, name);
	snd_ctl_elem_value_set_integer(v, 0, value);
	snd_ctl_elem_value_set_integer(v, 1, value);
	if (snd_ctl_elem_write(ctl, v) < 0) {
		fprintf(stderr, "s30-uac: %s: cannot set\n", name);
	}
	snd_ctl_close(ctl);
}

static void loopback(int on) {
	ctl_set2("AIF1 AD0 Capture Volume", on ? 160 : 0); // 160: as the DA0 playback volume (/etc/s30/rc)
	ctl_set2("AIF1 Slot 0 Digital ADC Capture Switch", on);
}

/* The app's playback rate if it plays S16_LE stereo, 0 if nothing plays,
   -1 if it plays something else */
static int codec_rate(void) {
	FILE* f = fopen("/proc/asound/card0/pcm0p/sub0/hw_params", "r");
	char line[64];
	int rate = 0, s16 = 0, stereo = 0;

	if (f == NULL) {
		return 0;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		sscanf(line, "rate: %d", &rate);
		if (strcmp(line, "format: S16_LE\n") == 0) {
			s16 = 1;
		}
		if (strcmp(line, "channels: 2\n") == 0) {
			stereo = 1;
		}
	}
	fclose(f);
	return rate == 0 ? 0 : s16 && stereo ? rate : -1;
}

static snd_pcm_t* open_pcm(const char* name, snd_pcm_stream_t dir, unsigned rate, snd_pcm_uframes_t period,
                           snd_pcm_uframes_t buffer) {
	snd_pcm_t* pcm;
	snd_pcm_hw_params_t* hw;
	snd_pcm_sw_params_t* sw;
	int err;

	if ((err = snd_pcm_open(&pcm, name, dir, SND_PCM_NONBLOCK)) < 0) {
		fprintf(stderr, "s30-uac: %s: %s\n", name, snd_strerror(err));
		return NULL;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0 ||
	    (err = snd_pcm_hw_params(pcm, hw)) < 0) {
		fprintf(stderr, "s30-uac: %s at %u Hz: %s\n", name, rate, snd_strerror(err));
		snd_pcm_close(pcm);
		return NULL;
	}
	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_set_start_threshold(pcm, sw, dir == SND_PCM_STREAM_PLAYBACK ? period : 1);
	snd_pcm_sw_params(pcm, sw);
	return pcm;
}

static uint64_t now_ms(void) {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* One session: the app plays at inRate and the host records at outRate.
   Ends when either stops (or changes rate); -1 if it couldn't start. */
static int session(int gadget, int inRate, int outRate) {
	static int16_t in[4096 * CHANNELS + CHANNELS], out[8192 * CHANNELS];
	const snd_pcm_uframes_t target = (snd_pcm_uframes_t)outRate * TARGET_MS / 1000;
	snd_pcm_t *cap, *play;
	char name[16];
	double pos = 0.0; // the next output frame's position in `in` (frame 0: the previous chunk's last)
	uint64_t nextCheck = 0;
	struct pollfd pfd;

	snprintf(name, sizeof(name), "hw:%d,0", gadget);
	play = open_pcm(name, SND_PCM_STREAM_PLAYBACK, (unsigned)outRate, target / 4, target * 3);
	if (play == NULL) {
		return -1;
	}
	loopback(1);
	cap = open_pcm(CODEC_PCM, SND_PCM_STREAM_CAPTURE, (unsigned)inRate, 256, 2048);
	if (cap == NULL) {
		loopback(0);
		snd_pcm_close(play);
		return -1;
	}
	fprintf(stderr, "s30-uac: %d Hz to %d Hz\n", inRate, outRate);
	memset(in, 0, sizeof(in[0]) * CHANNELS);
	memset(out, 0, sizeof(out));
	snd_pcm_writei(play, out, target); // prefilled to the target
	snd_pcm_start(cap);
	snd_pcm_poll_descriptors(cap, &pfd, 1);
	while (!sQuit) {
		snd_pcm_sframes_t n, delay = 0;
		double step;
		int o = 0;

		if (now_ms() >= nextCheck) {
			// The app closing (or reopening at another rate) must find the
			// capture closed: check often
			if (codec_rate() != inRate || ctl_get(gadget, SND_CTL_ELEM_IFACE_PCM, "Playback Rate") != outRate) {
				break;
			}
			nextCheck = now_ms() + 20;
		}
		poll(&pfd, 1, 20);
		n = snd_pcm_readi(cap, in + CHANNELS, 4096);
		if (n == -EAGAIN) {
			continue;
		}
		if (n < 0) {
			snd_pcm_recover(cap, (int)n, 1);
			snd_pcm_start(cap);
			continue;
		}
		// Steer the ratio by the gadget buffer's fill
		if (snd_pcm_delay(play, &delay) < 0) {
			delay = (snd_pcm_sframes_t)target;
		}
		step = (double)inRate / outRate;
		{
			double adj = ((double)delay - (double)target) / (double)target * MAX_ADJUST;
			adj = adj > MAX_ADJUST ? MAX_ADJUST : adj < -MAX_ADJUST ? -MAX_ADJUST : adj;
			step *= 1.0 + adj;
		}
		while (pos < (double)n && o < 8192) { // frames 0..n of `in`
			int i = (int)pos, c;
			double f = pos - i;

			for (c = 0; c < CHANNELS; c++) {
				out[o * CHANNELS + c] =
				    (int16_t)(in[i * CHANNELS + c] * (1.0 - f) + in[(i + 1) * CHANNELS + c] * f);
			}
			o++;
			pos += step;
		}
		pos -= (double)n;
		memcpy(in, in + n * CHANNELS, sizeof(in[0]) * CHANNELS); // the last frame starts the next chunk
		n = snd_pcm_writei(play, out, (snd_pcm_uframes_t)o);
		if (n == -EPIPE) { // the host fell behind or paused: start over at the target
			snd_pcm_prepare(play);
			memset(out, 0, sizeof(out[0]) * target * CHANNELS);
			snd_pcm_writei(play, out, target);
		}
	}
	snd_pcm_close(cap); // first: frees the codec for the app
	loopback(0);
	snd_pcm_close(play);
	fprintf(stderr, "s30-uac: stopped\n");
	return 0;
}

int main(void) {
	struct sigaction sa = { 0 };

	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	loopback(0);
	while (!sQuit) {
		int gadget = gadget_card(), inRate, outRate;

		if (gadget < 0) {
			sleep(1);
			continue;
		}
		inRate = codec_rate();
		outRate = (int)ctl_get(gadget, SND_CTL_ELEM_IFACE_PCM, "Playback Rate");
		if (inRate > 0 && outRate > 0 && session(gadget, inRate, outRate) != 0) {
			sleep(1); // not again at once (the log would fill)
		}
		usleep(50000);
	}
	return 0;
}
