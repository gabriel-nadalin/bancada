/*
 * slmodem_bridge - bridge a Smart Link softmodem to 8 kHz s16 PCM stdin/stdout.
 *
 * The modem line side is exchanged as raw 8 kHz mono 16-bit LE PCM over
 * stdin/stdout (compatible with rtp_bridge).  AT commands and user data
 * go through a PTY.
 *
 * Compile as 32-bit because slmodem's dsplibs.o is x86 32-bit.
 */

#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>

#include <samplerate.h>

#include <modem.h>
#include <modem_debug.h>

#define NET_RATE     8000
#define MODEM_RATE   9600
#define PTIME_MS     10
#define NET_SAMPC    (NET_RATE   * PTIME_MS / 1000)   /* 80 */
#define MODEM_SAMPC  (MODEM_RATE * PTIME_MS / 1000)   /* 96 */

/* slmodem init/exit externals */
extern unsigned int modem_debug_level;
extern int  dp_dummy_init(void);
extern void dp_dummy_exit(void);
extern int  dp_sinus_init(void);
extern void dp_sinus_exit(void);
extern int  prop_dp_init(void);
extern void prop_dp_exit(void);

static struct modem *g_modem;
static int g_pty;
static int g_running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

/* ------------------------------------------------------------------ driver */

static int bridge_start(struct modem *m)
{
	(void)m;
	return 0;
}

static int bridge_stop(struct modem *m)
{
	(void)m;
	return 0;
}

static int bridge_ioctl(struct modem *m, unsigned int cmd, unsigned long arg)
{
	(void)m;
	(void)arg;
	switch (cmd) {
	case MDMCTL_CAPABILITIES: return -1;
	case MDMCTL_HOOKSTATE:    return 0;
	case MDMCTL_SPEED:        return 0;
	case MDMCTL_GETFMTS:
	case MDMCTL_SETFMT:       return 0;
	case MDMCTL_SETFRAGMENT:  return 0;
	case MDMCTL_SPEAKERVOL:   return 0;
	case MDMCTL_CODECTYPE:    return CODEC_UNKNOWN;
	case MDMCTL_IODELAY:      return 0;
	default:                  return -2;
	}
}

static struct modem_driver bridge_driver = {
	.name  = "slmodem_bridge driver",
	.start = bridge_start,
	.stop  = bridge_stop,
	.ioctl = bridge_ioctl,
};

/* ------------------------------------------------------------------ resample */

/*
 * 8 kHz <-> 9.6 kHz streaming resampler via libsamplerate.
 *
 * The modem DSP (slmodem) runs at its native 9.6 kHz (MODEM_RATE); the
 * network side is standard 8 kHz telephony.  We bridge the two with a
 * proper windowed-sinc resampler (SRC_SINC_BEST_QUALITY) instead of the
 * previous linear interpolation: the linear kernel has a weak (~20 dB)
 * stopband and rolls the passband, both of which eat equalizer/DQPSK
 * margin on V.22bis/V.32.
 *
 * libsamplerate is streaming: internal filter latency means the first
 * frame may short-produce.  We therefore accumulate per-direction output
 * and emit a complete DSP frame (MODEM_SAMPC) / network frame (NET_SAMPC)
 * only when enough samples are buffered, preserving the frame cadence.
 */

#define RESAMPLE_MAX  (MODEM_SAMPC > NET_SAMPC ? MODEM_SAMPC : NET_SAMPC)

static SRC_STATE *src_to_modem;   /* 8k  -> 9.6k (network -> modem DSP)  */
static SRC_STATE *src_to_net;     /* 9.6k -> 8k  (modem DSP -> network)  */

static int resampler_init(void)
{
	int err = 0;

	src_to_modem = src_new(SRC_SINC_BEST_QUALITY, 1, &err);
	if (!src_to_modem) {
		fprintf(stderr, "resample: src_new(net->modem): %s\n",
			src_strerror(err));
		return -1;
	}
	src_to_net = src_new(SRC_SINC_BEST_QUALITY, 1, &err);
	if (!src_to_net) {
		fprintf(stderr, "resample: src_new(modem->net): %s\n",
			src_strerror(err));
		src_delete(src_to_modem);
		src_to_modem = NULL;
		return -1;
	}
	return 0;
}



/*
 * Feed one frame of int16 samples through a resampler state.  Produced
 * output is appended to the caller's accumulator; when a complete
 * `want`-sample block is available it is written (int16) into `block` and
 * the remainder stays buffered.  Returns 1 if a block was emitted.
 *
 * When `end_of_input` is set the state is flushed (tail drained) — this is
 * only used at shutdown.
 */
static int resample_emit(SRC_STATE *st, double ratio,
                         const int16_t *in, int in_cnt,
                         float *acc, int *acc_n, int want,
                         int16_t *block, int end_of_input)
{
	float fin[RESAMPLE_MAX];
	float fout[RESAMPLE_MAX * 3];
	SRC_DATA d;
	int have = *acc_n;
	int i;

	for (i = 0; i < in_cnt; i++)
		fin[i] = in[i] / 32768.0f;

	d.data_in	= fin;
	d.data_out	= fout;
	d.input_frames	= in_cnt;
	d.output_frames	= (long)(sizeof(fout) / sizeof(fout[0]));
	d.src_ratio	= ratio;
	d.end_of_input	= end_of_input;

	src_process(st, &d);

	{
		int out_cap = (int)(sizeof(fout) / sizeof(fout[0]));
		for (i = 0; i < d.output_frames_gen && have < out_cap; i++)
			acc[have++] = fout[i];
	}

	if (have >= want) {
		for (i = 0; i < want; i++)
			block[i] = (int16_t)(acc[i] * 32767.0f + 0.5f);
		memmove(acc, acc + want, (size_t)(have - want) * sizeof(float));
		*acc_n = have - want;
		return 1;
	}
	*acc_n = have;
	return 0;
}

/* ------------------------------------------------------------------ pty */

static int pty_open(char *name, size_t name_len)
{
	struct termios tios;
	int pty;
	char *pty_name;

	pty = posix_openpt(O_RDWR | O_NOCTTY);
	if (pty < 0 || grantpt(pty) < 0 || unlockpt(pty) < 0) {
		fprintf(stderr, "pty_open: %s\n", strerror(errno));
		return -1;
	}

	if (tcgetattr(pty, &tios) < 0) {
		fprintf(stderr, "tcgetattr: %s\n", strerror(errno));
		return -1;
	}
	cfmakeraw(&tios);
	cfsetispeed(&tios, B115200);
	cfsetospeed(&tios, B115200);
	if (tcsetattr(pty, TCSANOW, &tios) < 0) {
		fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
		return -1;
	}

	fcntl(pty, F_SETFL, O_NONBLOCK);

	pty_name = ptsname(pty);
	if (!pty_name) {
		fprintf(stderr, "ptsname: %s\n", strerror(errno));
		return -1;
	}
	strncpy(name, pty_name, name_len - 1);
	name[name_len - 1] = '\0';

	return pty;
}

/* ------------------------------------------------------------------ main */

static void usage(const char *name)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  -m, --mode MODE    Modem mode: orig (default) or ans\n"
		"  -d, --dial CMD     AT dial command (default: ATX3D\\r)\n"
		"  -r, --record FILE  Record RX audio to WAV file\n"
		"  -M MOD             SREG_DP modulation value (default: 122)\n"
		"  -e, --early-dial   In orig mode, dial at startup (before the\n"
		"                     audio path is established) so the modem is\n"
		"                     already listening when the call cuts through\n"
		"  -h, --help         Show this help\n"
		"\n"
		"Audio I/O:\n"
		"  stdin  : 8 kHz mono s16le PCM from network (rtp_bridge)\n"
		"  stdout : 8 kHz mono s16le PCM to network (rtp_bridge)\n"
		"\n"
		"Data/Control:\n"
		"  A PTY is created and printed to stderr.\n"
		"  Write AT commands and data to the PTY.\n"
		"  Received data is printed to the PTY.\n",
		name);
}

int main(int argc, char *argv[])
{
	const char *dial_cmd = "ATX3D\r";
	const char *mode = "orig";
	int opt;
	char pty_name[64];
	int dial_sent = 0;
	int modulation = 122;
	int early_dial = 0;
	const char *rec_path = NULL;
	FILE *rec_fp = NULL;

	while ((opt = getopt(argc, argv, "m:d:r:M:eh")) != -1) {
		switch (opt) {
		case 'm':
			mode = optarg;
			break;
		case 'd':
			dial_cmd = optarg;
			break;
		case 'r':
			rec_path = optarg;
			break;
		case 'M':
			modulation = atoi(optarg);
			break;
		case 'e':
			early_dial = 1;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	if (strcmp(mode, "orig") != 0 && strcmp(mode, "ans") != 0) {
		fprintf(stderr, "Invalid mode '%s'. Use 'orig' or 'ans'.\n", mode);
		return 1;
	}

	if (rec_path) {
		/* Write WAV header (placeholder), fix up at exit */
		struct {
			char  riff[4];
			uint32_t flen;
			char  wave[4];
			char  fmt[4];
			uint32_t chunk;
			uint16_t pcm;
			uint16_t ch;
			uint32_t srate;
			uint32_t bps;
			uint16_t align;
			uint16_t bpsamp;
			char  dat[4];
			uint32_t dlen;
		} hdr = {
			{'R','I','F','F'}, 36, {'W','A','V','E'},
			{'f','m','t',' '}, 16, 1, 1, 8000, 16000, 2, 16,
			{'d','a','t','a'}, 0
		};
		rec_fp = fopen(rec_path, "wb");
		if (!rec_fp) {
			fprintf(stderr, "Failed to open %s\n", rec_path);
			return 1;
		}
		fwrite(&hdr, sizeof(hdr), 1, rec_fp);
		fprintf(stderr, "Recording RX audio to %s\n", rec_path);
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	modem_debug_level = 3;
	modem_debug_init("bridge");

	dp_dummy_init();
	dp_sinus_init();
	prop_dp_init();
	modem_timer_init();

	if (resampler_init() != 0) {
		fprintf(stderr, "resampler init failed, aborting\n");
		return 1;
	}

	g_modem = modem_create(&bridge_driver, "slmodem_bridge");
	if (!g_modem) {
		fprintf(stderr, "modem_create failed\n");
		return 1;
	}
	g_modem->name = "slmodem_bridge";
	g_modem->dev_name = "slmodem_bridge";

	g_pty = pty_open(pty_name, sizeof(pty_name));
	if (g_pty < 0) {
		fprintf(stderr, "pty_open failed\n");
		return 1;
	}
	g_modem->pty = g_pty;

	fprintf(stderr, "PTY: %s\n", pty_name);
	fprintf(stderr, "Mode: %s\n", mode);

	/* In answer mode, just go off-hook so we can detect remote caller. */
	if (strcmp(mode, "ans") == 0) {
		const char *ata = "ATA\r";
		modem_write(g_modem, ata, strlen(ata));
	}
	/* Early dial: originate now so the modem is already listening when the
	 * audio path cuts through. The RAS's V8bis happens before the B-channel
	 * connects, so a post-connect originate is too late to participate. */
	if (early_dial && strcmp(mode, "orig") == 0) {
		dial_sent = 1;
		fprintf(stderr, "Early dial (before audio path)...\n");
		char buf[64];
		int len = snprintf(buf, sizeof(buf),
		                   "ATS32=%d\rAT%%C0\r", modulation);
		modem_write(g_modem, buf, len);
		modem_write(g_modem, dial_cmd, (int)strlen(dial_cmd));
	}

	/* Set stdin/stdout to binary and non-blocking where helpful. */
	fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
	fcntl(g_pty, F_SETFL, O_NONBLOCK);


	int16_t net_rx[NET_SAMPC];
	int16_t net_tx[NET_SAMPC];
	int16_t modem_rx[MODEM_SAMPC];
	int16_t modem_tx[MODEM_SAMPC];
	float tx_acc[3 * RESAMPLE_MAX];
	float rx_acc[3 * RESAMPLE_MAX];
	int tx_acc_n = 0;
	int rx_acc_n = 0;

	while (g_running) {
		struct timeval tv;
		fd_set rset;
		ssize_t n;

		FD_ZERO(&rset);
		FD_SET(STDIN_FILENO, &rset);
		FD_SET(g_pty, &rset);

		tv.tv_sec  = 0;
		tv.tv_usec = PTIME_MS * 1000;

		select(g_pty + 1, &rset, NULL, NULL, &tv);

		/* --- read network PCM from stdin --- */
		n = read(STDIN_FILENO, net_rx, sizeof(net_rx));
		if (n < 0 && errno != EAGAIN) {
			fprintf(stderr, "stdin read error: %s\n", strerror(errno));
			break;
		}
		if (n == 0) {
			fprintf(stderr, "stdin EOF\n");
			break;
		}

		if (n == sizeof(net_rx)) {
			/* --- auto-dial on first received audio frame --- */
			if (!dial_sent && strcmp(mode, "orig") == 0) {
				dial_sent = 1;
				fprintf(stderr,
				        "Audio path established, auto-dialing...\n");
				/* Set modulation + disable V.42bis compression.
				 * Compression error propagation: one bit error in
				 * compressed data can expand to arbitrary garbage
				 * containing false 0x7E HDLC flags. */
				char buf[64];
				int len = snprintf(buf, sizeof(buf),
				                   "ATS32=%d\rAT%%C0\r",
				                   modulation);
				modem_write(g_modem, buf, len);
				modem_write(g_modem, dial_cmd,
				            (int)strlen(dial_cmd));
			}

			/* --- record raw RX audio if requested --- */
			if (rec_fp)
				fwrite(net_rx, 2, NET_SAMPC, rec_fp);

			/* --- 8k -> 9.6k (SRC); feed the modem one DSP frame --- */
			if (resample_emit(src_to_modem,
			                  (double)MODEM_RATE / (double)NET_RATE,
			                  net_rx, NET_SAMPC,
			                  tx_acc, &tx_acc_n, MODEM_SAMPC,
			                  modem_rx, 0)) {

				/* --- run modem DSP --- */
				modem_process(g_modem, modem_rx, modem_tx,
				              MODEM_SAMPC);

				/* --- 9.6k -> 8k (SRC); emit one network frame if ready --- */
				if (resample_emit(src_to_net,
				                  (double)NET_RATE / (double)MODEM_RATE,
				                  modem_tx, MODEM_SAMPC,
				                  rx_acc, &rx_acc_n, NET_SAMPC,
				                  net_tx, 0)) {
					/* --- write network PCM to stdout --- */
					size_t out_bytes = NET_SAMPC * sizeof(int16_t);
					size_t written = 0;
					while (written < out_bytes) {
						n = write(STDOUT_FILENO,
						          (char *)net_tx + written,
						          out_bytes - written);
						if (n < 0) {
							if (errno == EAGAIN)
								continue;
							fprintf(stderr,
							        "stdout write error: %s\n",
							        strerror(errno));
							g_running = 0;
							break;
						}
						written += (size_t)n;
					}
				}
			}
		}

		/* --- read AT/data from PTY and feed to modem --- */
		if (FD_ISSET(g_pty, &rset)) {
			char buf[256];
			n = read(g_pty, buf, sizeof(buf));
			if (n > 0) {
				modem_write(g_modem, buf, (int)n);
			}
		}
	}

	fprintf(stderr, "shutting down...\n");

	/* Best-effort drain of the modem->net filter tail so the final
	 * partial frame isn't dropped; release both resampler states. */
	if (src_to_net) {
		int16_t tail[NET_SAMPC];
		(void)resample_emit(src_to_net,
		                    (double)NET_RATE / (double)MODEM_RATE,
		                    NULL, 0, rx_acc, &rx_acc_n, NET_SAMPC,
		                    tail, 1);
		if (rx_acc_n >= NET_SAMPC)
			write(STDOUT_FILENO, tail, (size_t)NET_SAMPC * 2);
		src_delete(src_to_net);
		src_to_net = NULL;
	}
	if (src_to_modem) {
		src_delete(src_to_modem);
		src_to_modem = NULL;
	}

	modem_delete(g_modem);
	dp_dummy_exit();
	dp_sinus_exit();
	prop_dp_exit();

	if (rec_fp) {
		long data_len = ftell(rec_fp);
		if (data_len > 44) {
			data_len -= 44;
			fseek(rec_fp, 4, SEEK_SET);
			uint32_t flen = 36 + (uint32_t)data_len;
			fwrite(&flen, 4, 1, rec_fp);
			fseek(rec_fp, 40, SEEK_SET);
			uint32_t dlen = (uint32_t)data_len;
			fwrite(&dlen, 4, 1, rec_fp);
		}
		fprintf(stderr, "Recording saved (%ld bytes of audio)\n",
			data_len > 0 ? data_len : 0L);
		fclose(rec_fp);
	}

	return 0;
}
