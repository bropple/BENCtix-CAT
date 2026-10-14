// SPDX-License-Identifier: GPL-2.0-only
/*
 * s22-pcmhold: hold a hostless ALSA PCM (D028).
 *
 * The q6voice VoiceMMode1 PCM carries no audio through Linux: opening,
 * configuring and starting it starts the modem's voice session on the
 * ADSP, and reads or writes fail with EINVAL. This opens it for playback
 * and capture, starts both, and holds them until SIGTERM/SIGINT.
 *
 *   s22-pcmhold [DEVICE]          (default hw:0,3)
 */
#include <alsa/asoundlib.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t quit;
static void on_signal(int sig) { (void)sig; quit = 1; }

static snd_pcm_t *start(const char *dev, snd_pcm_stream_t dir)
{
	const char *name = dir == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture";
	snd_pcm_t *pcm;
	int err;

	err = snd_pcm_open(&pcm, dev, dir, 0);
	if (err < 0) {
		fprintf(stderr, "s22-pcmhold: open %s %s: %s\n", dev, name, snd_strerror(err));
		return NULL;
	}
	err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
				 1, 8000, 0, 100000);
	/*
	 * Nothing is ever written, so playback would stop on an underrun at
	 * once (EPIPE): never stop on an empty buffer.
	 */
	if (err >= 0) {
		snd_pcm_sw_params_t *sw;
		snd_pcm_uframes_t boundary;

		snd_pcm_sw_params_alloca(&sw);
		err = snd_pcm_sw_params_current(pcm, sw);
		if (err >= 0)
			err = snd_pcm_sw_params_get_boundary(sw, &boundary);
		if (err >= 0)
			err = snd_pcm_sw_params_set_stop_threshold(pcm, sw, boundary);
		if (err >= 0)
			err = snd_pcm_sw_params(pcm, sw);
	}
	if (err >= 0)
		err = snd_pcm_prepare(pcm);
	if (err >= 0)
		err = snd_pcm_start(pcm);
	if (err < 0) {
		fprintf(stderr, "s22-pcmhold: start %s %s: %s\n", dev, name, snd_strerror(err));
		snd_pcm_close(pcm);
		return NULL;
	}
	return pcm;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "hw:0,3";
	snd_pcm_t *play, *cap;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	play = start(dev, SND_PCM_STREAM_PLAYBACK);
	cap = start(dev, SND_PCM_STREAM_CAPTURE);
	if (!play && !cap)
		return 1;
	fprintf(stderr, "s22-pcmhold: %s held (playback %s, capture %s)\n",
		dev, play ? "on" : "FAILED", cap ? "on" : "FAILED");
	while (!quit)
		pause();
	if (cap)
		snd_pcm_close(cap);
	if (play)
		snd_pcm_close(play);
	return 0;
}
