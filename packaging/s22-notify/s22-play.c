// SPDX-License-Identifier: GPL-2.0-only
/*
 * s22-play: play an Ogg Vorbis sound (a notification, a ringtone, a preview
 * in Settings), or a 16-bit PCM WAV (speech: bm -o - | s22-play -, D065).
 *
 * With the sound server up (s22-audio: PipeWire in the GUI's runtime
 * directory, D040) it plays into the loudspeaker node, "s22.speaker",
 * through PipeWire's ALSA plugin, mixed with whatever else plays but not
 * under the media volume; else straight to the card. Its own volume is
 * LEVEL out of 15 (-v), by default the ring and notification volume
 * (ring_volume in the settings); 0 plays nothing. -l loops until killed
 * (a ringtone); SIGTERM stops it at once. A WAV is FILE.wav, or "-" for
 * standard input (to its end; -l loops a file only).
 *
 *   s22-play [-l] [-v LEVEL] [-d DEVICE] FILE.ogg|FILE.wav|-
 */
#include <alsa/asoundlib.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <vorbis/vorbisfile.h>

#define GUI_RUN		"/run/s22-gui"
#define SETTINGS	"/var/lib/s22/settings"
#define SPEAKER		"s22.speaker"
#define LEVELS		15
#define RING_DEF	12

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int usage(void)
{
	fprintf(stderr, "usage: s22-play [-l] [-v LEVEL] [-d DEVICE] FILE.ogg|FILE.wav|-\n");
	return 2;
}

static int ring_volume(void)
{
	char line[128];
	int v = RING_DEF;
	FILE *f = fopen(SETTINGS, "re");

	while (f && fgets(line, sizeof(line), f))
		if (strncmp(line, "ring_volume=", 12) == 0)
			v = atoi(line + 12);
	if (f)
		fclose(f);
	return v < 0 ? 0 : v > LEVELS ? LEVELS : v;
}

/* A 16-bit PCM WAV: its channels and rate, and f at its samples; false if
 * it is not one */
static bool wav_open(FILE *f, int *channels, long *rate, long *data_at)
{
	unsigned char h[12], ck[8], fmt[16];

	if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
		return false;
	bool have_fmt = false;
	long at = 12;
	while (fread(ck, 1, 8, f) == 8) {
		unsigned long len = ck[4] | ck[5] << 8 | ck[6] << 16 | (unsigned long)ck[7] << 24;
		at += 8;
		if (!memcmp(ck, "fmt ", 4) && len >= 16) {
			if (fread(fmt, 1, 16, f) != 16)
				return false;
			if ((fmt[0] | fmt[1] << 8) != 1 || (fmt[14] | fmt[15] << 8) != 16)
				return false;	/* PCM, 16 bits */
			*channels = fmt[2] | fmt[3] << 8;
			*rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (long)fmt[7] << 24;
			have_fmt = true;
			len -= 16;
			at += 16;
		} else if (!memcmp(ck, "data", 4)) {
			*data_at = at;
			return have_fmt && *channels >= 1 && *channels <= 2 && *rate > 0;
		}
		/* skip the rest of the chunk, by reading (standard input) */
		for (len += len & 1; len > 0; len--, at++)
			if (getc(f) == EOF)
				return false;
	}
	return false;
}

int main(int argc, char **argv)
{
	const char *device = NULL;
	bool loop = false;
	int opt, level = -1;

	while ((opt = getopt(argc, argv, "lv:d:")) != -1) {
		switch (opt) {
		case 'l': loop = true; break;
		case 'v': level = atoi(optarg); break;
		case 'd': device = optarg; break;
		default: return usage();
		}
	}
	if (optind != argc - 1)
		return usage();
	if (level < 0)
		level = ring_volume();
	if (level == 0)
		return 0;
	/* Amplitude: LEVEL/15, as wpctl's volumes go */
	int gain = level >= LEVELS ? 65536 : level * 65536 / LEVELS;

	if (!device) {
		if (access(GUI_RUN "/pipewire-0", F_OK) == 0) {
			setenv("PIPEWIRE_RUNTIME_DIR", GUI_RUN, 0);
			setenv("PIPEWIRE_NODE", SPEAKER, 0);
			setenv("PIPEWIRE_PROPS", "{ media.role = Notification "
			       "application.name = s22-play }", 0);
			device = "pipewire";
		} else {
			device = "plughw:0,0";
		}
	}

	const char *name = argv[optind];
	size_t nlen = strlen(name);
	bool wav = !strcmp(name, "-") || (nlen > 4 && !strcasecmp(name + nlen - 4, ".wav"));
	FILE *f = !strcmp(name, "-") ? stdin : fopen(name, "rb");
	OggVorbis_File vf;
	int channels;
	long rate, data_at = 0;
	if (wav) {
		if (!f || !wav_open(f, &channels, &rate, &data_at)) {
			fprintf(stderr, "s22-play: %s: not a 16-bit PCM WAV\n", name);
			return 1;
		}
		if (f == stdin)
			loop = false;
	} else {
		if (!f || ov_open(f, &vf, NULL, 0) < 0) {
			fprintf(stderr, "s22-play: %s: not an Ogg Vorbis file\n", name);
			return 1;
		}
		vorbis_info *vi = ov_info(&vf, -1);
		channels = vi->channels;
		rate = vi->rate;
	}

	snd_pcm_t *pcm;
	int err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
	if (err >= 0)
		err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
					 SND_PCM_ACCESS_RW_INTERLEAVED, channels,
					 rate, 1, 100000);
	if (err < 0) {
		fprintf(stderr, "s22-play: %s: %s\n", device, snd_strerror(err));
		if (!wav)
			ov_clear(&vf);
		return 1;
	}

	struct sigaction sa = { .sa_handler = on_signal };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	short buf[4096];
	int section;
	while (!stop) {
		long n;
		if (wav) {
			n = (long)fread(buf, 2 * channels, sizeof(buf) / (2 * channels), f) * 2 * channels;
			if (n == 0) {
				if (!loop || fseek(f, data_at, SEEK_SET) != 0)
					break;
				continue;
			}
		} else {
			n = ov_read(&vf, (char *)buf, sizeof(buf), 0, 2, 1, &section);
			if (n == 0) {
				if (!loop || ov_raw_seek(&vf, 0) != 0)
					break;
				continue;
			}
		}
		if (n < 0)
			continue;		/* a hole in the stream */
		if (gain != 65536)
			for (long i = 0; i < n / 2; i++)
				buf[i] = (short)((buf[i] * gain) >> 16);
		snd_pcm_sframes_t frames = n / (2 * channels);
		short *p = buf;
		while (frames > 0 && !stop) {
			snd_pcm_sframes_t w = snd_pcm_writei(pcm, p, frames);
			if (w < 0) {
				if (snd_pcm_recover(pcm, w, 1) < 0)
					stop = 1;
				continue;
			}
			frames -= w;
			p += w * channels;
		}
	}
	if (stop)
		snd_pcm_drop(pcm);
	else
		snd_pcm_drain(pcm);
	snd_pcm_close(pcm);
	if (!wav)
		ov_clear(&vf);
	return 0;
}
