#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <re.h>
#include <baresip.h>


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
		info("dial_and_play: Call established!\n");
		break;

	case BEVENT_CALL_CLOSED:
		info("dial_and_play: Call closed (%s)\n",
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
	const char *src_dev  = "duvet.wav";
	const char *play_mod = "aufile";
	const char *play_dev = "/dev/null";

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
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	/* Initialize libre event loop and networking stack */
	err = libre_init();
	if (err) {
		warning("dial_and_play: libre_init failed (%m)\n", err);
		return err;
	}

	/* Load default baresip configuration */
	err = conf_configure();
	if (err) {
		warning("dial_and_play: conf_configure failed (%m)\n", err);
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
		warning("dial_and_play: baresip_init failed (%m)\n", err);
		goto out;
	}

	/* Initialise User Agents */
	err = ua_init("teste", true, false, false);
	if (err)
		goto out;

	/* Register for call events */
	err = bevent_register(event_handler, NULL);
	if (err) {
		warning("dial_and_play: bevent_register failed (%m)\n", err);
		goto out;
	}

	/* Load required modules */
	err = module_load(".", "aufile");
	if (err) {
		warning("dial_and_play: failed to load aufile (%m)\n", err);
		goto out;
	}

	err = module_load(".", "g711");
	if (err) {
		warning("dial_and_play: failed to load g711 (%m)\n", err);
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
		warning("dial_and_play: ua_alloc failed (%m)\n", err);
		goto out;
	}

	info("dial_and_play: Dialing %s ...\n", peer);

	err = ua_connect(
		ua,
		&call,
		NULL,
		peer,
		VIDMODE_OFF
	);

	if (err) {
		warning("dial_and_play: ua_connect failed (%m)\n", err);
		goto out;
	}

	/* Enter main event loop */
	err = re_main(signal_handler);

	if (err) {
		warning("dial_and_play: re_main exited (%m)\n", err);
	}

out:
	/* Cleanup */
	baresip_close();
	libre_close();
	ua_close();

	return err;
}