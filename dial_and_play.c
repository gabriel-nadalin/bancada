#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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


int main(void)
{
	struct config *cfg;
	struct ua *ua = NULL;
	struct call *call = NULL;
	int err;

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

	/*
	 * Use the aufile module as audio source.
	 * Audio will be read from duvet.wav and transmitted
	 * into the SIP call.
	 */
	str_ncpy(cfg->audio.src_mod,
		 "aufile",
		 sizeof(cfg->audio.src_mod));

	str_ncpy(cfg->audio.src_dev,
		 "duvet.wav",
		 sizeof(cfg->audio.src_dev));

	/* Initialize baresip core */
	err = baresip_init(cfg);
	if (err) {
		warning("dial_and_play: baresip_init failed (%m)\n", err);
		goto out;
	}

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
		       "<sip:10@10.42.0.1>;regint=0");
	if (err) {
		warning("dial_and_play: ua_alloc failed (%m)\n", err);
		goto out;
	}

	info("dial_and_play: Dialing "
	     "sip:11@10.42.0.102:5062;transport=udp ...\n");

	err = ua_connect(
		ua,
		&call,
		NULL,
		"sip:11@10.42.0.102:5062;transport=udp",
		VIDMODE_OFF);

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

	return err;
}