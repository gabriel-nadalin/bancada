#include <stdint.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <re.h>
#include <baresip.h>
static void hangup_handler(void *arg);
static struct tmr hangup_tmr;
static unsigned playback_dur = 15;


/** Read WAV duration in seconds from the data chunk. */
static double wav_duration_sec(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0.0;

	char hdr[12];
	if (fread(hdr, 1, 12, f) != 12
	    || memcmp(hdr, "RIFF", 4) != 0
	    || memcmp(hdr + 8, "WAVE", 4) != 0) {
		fclose(f);
		return 0.0;
	}

	int sr = 0;
	uint32_t data_sz = 0;
	char chunk_id[4];

	while (fread(chunk_id, 1, 4, f) == 4) {
		uint32_t sz;
		if (fread(&sz, 4, 1, f) != 1) break;

		if (memcmp(chunk_id, "fmt ", 4) == 0) {
			uint16_t fmt;  (void)fmt;
			uint16_t ch;
			fread(&fmt, 2, 1, f);
			fread(&ch,  2, 1, f);
			fread(&sr,  4, 1, f);
			fseek(f, sz - 8, SEEK_CUR);
		}
		else if (memcmp(chunk_id, "data", 4) == 0) {
			data_sz = sz;
			break;
		}
		else {
			fseek(f, sz, SEEK_CUR);
		}
	}

	fclose(f);
	if (sr <= 0 || data_sz < 4) return 0.0;
	return (double)data_sz / 2.0 / (double)sr;
}

static void hangup_handler(void *arg)
{
	(void)arg;
	info("baresip_play: playback duration reached\n");
	re_cancel();
}

static void signal_handler(int sig)
{
	(void)sig;
	re_cancel();
}


static void event_handler(enum bevent_ev ev,
			  struct bevent *event,
			  void *arg)
{
	(void)arg;

	switch (ev) {

	case BEVENT_CALL_ESTABLISHED:
		info("baresip_play: Call established!\n");
		tmr_start(&hangup_tmr,
			  (uint64_t)(playback_dur + 3) * 1000,
			  hangup_handler, NULL);
		break;

	case BEVENT_CALL_CLOSED:
		info("baresip_play: Call closed (%s)\n",
		     bevent_get_text(event));
		re_cancel();
		break;

	default:
		break;
	}
}


static void usage(const char *name)
{
	re_fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"General options:\n"
		"  -p, --peer URI       Peer SIP URI to dial\n"
		"                       (default: sip:13@10.42.0.102:5062;transport=udp)\n"
		"Audio options:\n"
		"  -m, --src-mod NAME    Audio source module  (default: aufile)\n"
		"  -d, --src-dev DEVICE  Audio source device  (default: duvet.wav)\n"
		"  -M, --play-mod NAME   Audio playback module\n"
		"  -D, --play-dev DEVICE Audio playback device\n"
		"  -h, --help            Show this help\n",
		name);
}


int main(int argc, char *argv[])
{
	struct config *cfg;
	struct ua *ua = NULL;
	struct call *call = NULL;
	int err;
	const char *peer    = "sip:13@10.42.0.102:5062;transport=udp";
	const char *src_mod  = "aufile";
	const char *play_mod = "aufile";
	const char *play_dev = "/dev/null";
	const char *src_dev  = "duvet.wav";
	static const struct option longopts[] = {
		{"peer",     required_argument, NULL, 'p'},
		{"src-mod",  required_argument, NULL, 'm'},
		{"src-dev",  required_argument, NULL, 'd'},
		{"play-mod", required_argument, NULL, 'M'},
		{"play-dev", required_argument, NULL, 'D'},
		{"help",     no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0}
	};
	for (;;) {
		int c = getopt_long(argc, argv, "p:m:d:M:D:h", longopts, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'p':
			peer = optarg;
			break;
		case 'm':
			src_mod = optarg;
			break;
		case 'd':
			src_dev = optarg;
			break;
		case 'M':
			play_mod = optarg;
			break;
		case 'D':
			play_dev = optarg;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	/* Auto-detect playback duration from the source WAV. */
	{
		double dur = wav_duration_sec(src_dev);
		if (dur > 0.0)
			playback_dur = (unsigned)(dur + 0.5);
		info("baresip_play: source duration = %u s\n", playback_dur);
	}

	/* Initialize libre event loop and networking stack */
	err = libre_init();
	if (err) {
		warning("baresip_play: libre_init failed (%m)\n", err);
		return err;
	}

	/* Load default baresip configuration */
	err = conf_configure();
	if (err) {
		warning("baresip_play: conf_configure failed (%m)\n", err);
		goto out;
	}

	cfg = conf_config();

	/* Disable jitter buffer for minimum latency */
	cfg->avt.audio.jbtype = JBUF_OFF;
	cfg->net.af = AF_INET;
	str_ncpy(cfg->sip.local, "10.42.0.1:5060", sizeof(cfg->sip.local));
	cfg->audio.buffer.min = 10;
	cfg->audio.buffer.max = 10;
	cfg->audio.adaptive = false;
	cfg->audio.silence = -50.0;

	if (src_mod)
		str_ncpy(cfg->audio.src_mod, src_mod,
			 sizeof(cfg->audio.src_mod));

	if (src_dev)
		str_ncpy(cfg->audio.src_dev, src_dev,
			 sizeof(cfg->audio.src_dev));

	if (play_mod)
		str_ncpy(cfg->audio.play_mod, play_mod,
			 sizeof(cfg->audio.play_mod));

	if (play_dev)
		str_ncpy(cfg->audio.play_dev, play_dev,
			 sizeof(cfg->audio.play_dev));

	/* Initialize baresip core */
	err = baresip_init(cfg);
	if (err) {
		warning("baresip_play: baresip_init failed (%m)\n", err);
		goto out;
	}

	/* Initialise User Agents */
	err = ua_init("teste", true, false, false);
	if (err)
		goto out;

	/* Register for call events */
	err = bevent_register(event_handler, NULL);
	if (err) {
		warning("baresip_play: bevent_register failed (%m)\n", err);
		goto out;
	}

	/* Load required modules */
	err = module_load(".", "aufile");
	if (err) {
		warning("baresip_play: failed to load aufile (%m)\n", err);
		goto out;
	}

	err = module_load(".", "g711");
	if (err) {
		warning("baresip_play: failed to load g711 (%m)\n", err);
		goto out;
	}

	/*
	 * Create a registrar-less SIP account.
	 *
	 * regint=0 disables registration completely.
	 * We only need a local identity for making the call.
	 */
	err = ua_alloc(&ua,
		       "<sip:12@10.42.0.1>;regint=0;ptime=10");
	if (err) {
		warning("baresip_play: ua_alloc failed (%m)\n", err);
		goto out;
	}

	info("baresip_play: Dialing %s ...\n", peer);

	err = ua_connect(
		ua,
		&call,
		NULL,
		peer,
		VIDMODE_OFF
	);

	if (err) {
		warning("baresip_play: ua_connect failed (%m)\n", err);
		goto out;
	}

	/* Enter main event loop */
	err = re_main(signal_handler);

	if (err) {
		warning("baresip_play: re_main exited (%m)\n", err);
	}

out:
	/* Cleanup */
	baresip_close();
	libre_close();
	ua_close();

	return err;
}