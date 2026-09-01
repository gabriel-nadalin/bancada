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

#ifdef USE_LIBSAMPLERATE
#include <samplerate.h>
#endif

#include <modem.h>
#include <modem_debug.h>

extern int16_t SnrToRetrainTable[6];

#define NET_RATE         8000
#define MODEM_RATE_MAX   9600
#define PTIME_MS         10
#define NET_SAMPC        (NET_RATE * PTIME_MS / 1000)       /* 80 */
#define MODEM_SAMPC_MAX  (MODEM_RATE_MAX * PTIME_MS / 1000) /* 96 */
#define DEFAULT_IO_DELAY 240
#define DEFAULT_V32BIS_RETRAIN_SNR 13

/* slmodem init/exit externals */
extern unsigned int modem_debug_level;
extern int  dp_dummy_init(void);
extern void dp_dummy_exit(void);
extern int  dp_sinus_init(void);
extern void dp_sinus_exit(void);
extern int  prop_dp_init(void);
extern void prop_dp_exit(void);

#ifndef USE_LIBSAMPLERATE
/* Fixed-rate converters exported by the vendor DSP object. */
struct RcFixedHandle;
extern struct RcFixedHandle *RcFixed_Create(int ratio_code);
extern void RcFixed_Delete(struct RcFixedHandle *handle);
extern void RcFixed_Resample(struct RcFixedHandle *handle,
	int16_t *in_samples, unsigned int in_count,
	uint16_t *out_samples, unsigned int *out_count);

#define RC_8K_TO_9_6K 2
#define RC_9_6K_TO_8K 3
#endif

static struct modem *g_modem;
static int g_pty;
static int g_running = 1;
static int g_io_delay = DEFAULT_IO_DELAY;
static int g_modem_rate = MODEM_RATE_MAX;
static int g_max_rate;
static int g_v32bis_retrain_snr = DEFAULT_V32BIS_RETRAIN_SNR;

static int modem_sampc(void)
{
	return g_modem_rate * PTIME_MS / 1000;
}

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
	/* IODELAY is the local capture/playback skew in native-rate samples,
	 * not the network round-trip time.  For VPCM, 240 at 9.6 kHz becomes
	 * the DSP's nominal 244 after its internal +4; the queue-free 8 kHz
	 * bridge reports zero to V.32/V.32bis. */
	case MDMCTL_IODELAY:      return g_io_delay;
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

#ifdef USE_LIBSAMPLERATE

/* Optional comparison path using libsamplerate's streaming sinc filter. */
#define RESAMPLE_MAX  MODEM_SAMPC_MAX

static SRC_STATE *src_to_modem;
static SRC_STATE *src_to_net;
static float to_modem_acc[3 * RESAMPLE_MAX];
static float to_net_acc[3 * RESAMPLE_MAX];
static int to_modem_acc_n;
static int to_net_acc_n;

static int resample_emit(SRC_STATE *st, double ratio,
			 const int16_t *in, int in_cnt,
			 float *acc, int *acc_n, int want,
			 int16_t *block)
{
	float fin[RESAMPLE_MAX];
	float fout[RESAMPLE_MAX * 3];
	SRC_DATA d;
	int have = *acc_n;
	int i;

	for (i = 0; i < in_cnt; i++)
		fin[i] = in[i] / 32768.0f;

	d.data_in = fin;
	d.data_out = fout;
	d.input_frames = in_cnt;
	d.output_frames = (long)(sizeof(fout) / sizeof(fout[0]));
	d.src_ratio = ratio;
	d.end_of_input = 0;

	if (src_process(st, &d) != 0)
		return -1;

	for (i = 0; i < d.output_frames_gen &&
	     have < (int)(sizeof(fout) / sizeof(fout[0])); i++)
		acc[have++] = fout[i];

	if (have < want) {
		*acc_n = have;
		return 0;
	}
	for (i = 0; i < want; i++)
		block[i] = (int16_t)(acc[i] * 32767.0f + 0.5f);
	memmove(acc, acc + want, (size_t)(have - want) * sizeof(float));
	*acc_n = have - want;
	return want;
}

static int resampler_init(void)
{
	int err = 0;

	if (g_modem_rate == NET_RATE) {
		fprintf(stderr, "Resampler: bypassed (native 8 kHz data pump)\n");
		return 0;
	}

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
	fprintf(stderr, "Resampler: libsamplerate\n");
	return 0;
}

static int resample_to_modem(int16_t *input, int16_t *output)
{
	if (g_modem_rate == NET_RATE) {
		memcpy(output, input, sizeof(input[0]) * NET_SAMPC);
		return NET_SAMPC;
	}
	return resample_emit(src_to_modem,
		(double)g_modem_rate / (double)NET_RATE,
		input, NET_SAMPC, to_modem_acc, &to_modem_acc_n,
		modem_sampc(), output);
}

static int resample_to_net(int16_t *input, int input_count, int16_t *output)
{
	if (input_count <= 0)
		return 0;
	if (g_modem_rate == NET_RATE) {
		memcpy(output, input, sizeof(input[0]) * (size_t)input_count);
		return input_count;
	}
	return resample_emit(src_to_net,
		(double)NET_RATE / (double)g_modem_rate,
		input, input_count, to_net_acc, &to_net_acc_n,
		input_count * NET_RATE / g_modem_rate, output);
}

static void resampler_destroy(void)
{
	if (src_to_net)
		src_delete(src_to_net);
	if (src_to_modem)
		src_delete(src_to_modem);
	src_to_net = NULL;
	src_to_modem = NULL;
}

#else

/* Default path: fixed-point converters paired with the proprietary DSP. */
static struct RcFixedHandle *src_to_modem;
static struct RcFixedHandle *src_to_net;

static int resampler_init(void)
{
	if (g_modem_rate == NET_RATE) {
		fprintf(stderr, "Resampler: bypassed (native 8 kHz data pump)\n");
		return 0;
	}
	src_to_modem = RcFixed_Create(RC_8K_TO_9_6K);
	if (!src_to_modem) {
		fprintf(stderr, "resample: RcFixed_Create(net->modem) failed\n");
		return -1;
	}
	src_to_net = RcFixed_Create(RC_9_6K_TO_8K);
	if (!src_to_net) {
		fprintf(stderr, "resample: RcFixed_Create(modem->net) failed\n");
		RcFixed_Delete(src_to_modem);
		src_to_modem = NULL;
		return -1;
	}
	fprintf(stderr, "Resampler: dsplibs RcFixed\n");
	return 0;
}

static int resample_fixed(struct RcFixedHandle *state,
			  int16_t *input, unsigned int input_count,
			  int16_t *output, unsigned int output_capacity)
{
	unsigned int produced = output_capacity;

	RcFixed_Resample(state, input, input_count,
		(uint16_t *)output, &produced);
	return (int)produced;
}

static int resample_to_modem(int16_t *input, int16_t *output)
{
	if (g_modem_rate == NET_RATE) {
		memcpy(output, input, sizeof(input[0]) * NET_SAMPC);
		return NET_SAMPC;
	}
	return resample_fixed(src_to_modem, input, NET_SAMPC,
		output, (unsigned int)modem_sampc());
}

static int resample_to_net(int16_t *input, int input_count, int16_t *output)
{
	if (input_count <= 0)
		return 0;
	if (g_modem_rate == NET_RATE) {
		memcpy(output, input, sizeof(input[0]) * (size_t)input_count);
		return input_count;
	}
	return resample_fixed(src_to_net, input, (unsigned int)input_count,
		output, NET_SAMPC);
}

static void resampler_destroy(void)
{
	if (src_to_net)
		RcFixed_Delete(src_to_net);
	if (src_to_modem)
		RcFixed_Delete(src_to_modem);
	src_to_net = NULL;
	src_to_modem = NULL;
}

#endif

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

static FILE *open_wav(const char *path, const char *label)
{
	struct {
		char riff[4];
		uint32_t flen;
		char wave[4];
		char fmt[4];
		uint32_t chunk;
		uint16_t pcm;
		uint16_t ch;
		uint32_t srate;
		uint32_t bps;
		uint16_t align;
		uint16_t bpsamp;
		char dat[4];
		uint32_t dlen;
	} hdr = {
		{'R','I','F','F'}, 36, {'W','A','V','E'},
		{'f','m','t',' '}, 16, 1, 1, 8000, 16000, 2, 16,
		{'d','a','t','a'}, 0
	};
	FILE *fp = fopen(path, "wb");

	if (!fp) {
		fprintf(stderr, "Failed to open %s\n", path);
		return NULL;
	}
	fwrite(&hdr, sizeof(hdr), 1, fp);
	fprintf(stderr, "Recording %s audio to %s\n", label, path);
	return fp;
}

static void close_wav(FILE *fp, const char *label)
{
	long data_len = ftell(fp);

	if (data_len > 44) {
		uint32_t flen;
		uint32_t dlen;

		data_len -= 44;
		flen = 36 + (uint32_t)data_len;
		dlen = (uint32_t)data_len;
		fseek(fp, 4, SEEK_SET);
		fwrite(&flen, 4, 1, fp);
		fseek(fp, 40, SEEK_SET);
		fwrite(&dlen, 4, 1, fp);
	}
	fprintf(stderr, "%s recording saved (%ld bytes of audio)\n",
		label, data_len > 0 ? data_len : 0L);
	fclose(fp);
}

static int emit_net_audio(FILE *txrec_fp, const int16_t *samples, int count)
{
	size_t out_bytes = (size_t)count * sizeof(samples[0]);
	size_t written = 0;
	ssize_t n;

	if (txrec_fp)
		fwrite(samples, sizeof(samples[0]), (size_t)count, txrec_fp);

	while (written < out_bytes) {
		n = write(STDOUT_FILENO, (const char *)samples + written,
			out_bytes - written);
		if (n == 0) {
			fprintf(stderr, "stdout write returned zero\n");
			return -1;
		}
		if (n < 0) {
			if (errno == EAGAIN)
				continue;
			fprintf(stderr, "stdout write error: %s\n",
				strerror(errno));
			return -1;
		}
		written += (size_t)n;
	}
	return 0;
}

static void usage(const char *name)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  -m, --mode MODE    Modem mode: orig (default) or ans\n"
		"  -d, --dial CMD     AT dial command (default: ATX3D\\r)\n"
		"  -r, --record FILE  Record RX audio to WAV file\n"
		"  -T, --tx-record FILE  Record TX audio to WAV file\n"
		"  -M MOD             SREG_DP modulation value (default: 122)\n"
		"  -v LEVEL           Debug verbosity (default: 1)\n"
		"  -D SAMPLES         Local audio I/O delay in native-rate samples\n"
		"                     (default: 240)\n"
		"  -S RATE            Native data-pump rate: 8000 or 9600 Hz\n"
		"                     (default: 9600)\n"
		"  -R BITRATE         Cap the data-pump rate (300 through 56000 bit/s)\n"
		"  -N DB              V.32bis local retrain SNR threshold (0 through 40)\n"
		"                     (default: 13)\n"
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
	int debug_level = 1;
	int early_dial = 0;
	const char *rec_path = NULL;
	FILE *rec_fp = NULL;
	const char *txrec_path = NULL;
	FILE *txrec_fp = NULL;

	while ((opt = getopt(argc, argv, "m:d:r:T:M:v:D:S:R:N:eh")) != -1) {
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
		case 'T':
			txrec_path = optarg;
			break;
		case 'M':
			modulation = atoi(optarg);
			break;
		case 'v':
			debug_level = atoi(optarg);
			if (debug_level < 0) {
				fprintf(stderr, "Invalid debug level '%s'.\n", optarg);
				return 1;
			}
			break;
		case 'D':
			g_io_delay = atoi(optarg);
			if (g_io_delay < 0) {
				fprintf(stderr, "Invalid I/O delay '%s'.\n", optarg);
				return 1;
			}
			break;
		case 'S':
			g_modem_rate = atoi(optarg);
			if (g_modem_rate != NET_RATE &&
			    g_modem_rate != MODEM_RATE_MAX) {
				fprintf(stderr, "Invalid modem rate '%s'.\n", optarg);
				return 1;
			}
			break;
		case 'R':
			g_max_rate = atoi(optarg);
			if (g_max_rate < MODEM_MIN_RATE ||
			    g_max_rate > MODEM_MAX_RATE) {
				fprintf(stderr, "Invalid maximum rate '%s'.\n", optarg);
				return 1;
			}
			break;
		case 'N':
			g_v32bis_retrain_snr = atoi(optarg);
			if (g_v32bis_retrain_snr < 0 ||
			    g_v32bis_retrain_snr > 40) {
				fprintf(stderr,
					"Invalid V.32bis retrain threshold '%s'.\n",
					optarg);
				return 1;
			}
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

	if (modulation == 132) {
		/* The stock 12/14.4 kbit/s thresholds (20/24 dB) interpret a
		 * repeatable, short estimator dip on G.711 bridge paths as line
		 * degradation.  End-to-end RAS traffic remains valid during the
		 * dip.  Retain local SNR protection at an established lower-rate
		 * threshold; far-end retrain requests use a separate control path. */
		SnrToRetrainTable[4] = g_v32bis_retrain_snr;
		SnrToRetrainTable[5] = g_v32bis_retrain_snr;
		fprintf(stderr, "V.32bis retrain SNR threshold: %d dB\n",
			g_v32bis_retrain_snr);
	}
	if (rec_path) {
		rec_fp = open_wav(rec_path, "RX");
		if (!rec_fp)
			return 1;
	}
	if (txrec_path) {
		txrec_fp = open_wav(txrec_path, "TX");
		if (!txrec_fp)
			return 1;
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	modem_debug_level = (unsigned int)debug_level;
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
	g_modem->srate = (unsigned int)g_modem_rate;
	g_modem->frag = (unsigned int)(g_modem_rate / 200);
	if (g_max_rate)
		g_modem->max_rate = (unsigned int)g_max_rate;

	g_pty = pty_open(pty_name, sizeof(pty_name));
	if (g_pty < 0) {
		fprintf(stderr, "pty_open failed\n");
		return 1;
	}
	g_modem->pty = g_pty;

	fprintf(stderr, "PTY: %s\n", pty_name);
	fprintf(stderr, "Mode: %s\n", mode);
	fprintf(stderr, "Native data-pump rate: %d Hz\n", g_modem_rate);
	fprintf(stderr, "I/O delay: %d samples at %d Hz\n",
		g_io_delay, g_modem_rate);
	if (g_max_rate)
		fprintf(stderr, "Maximum data rate: %d bit/s\n", g_max_rate);

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
	int16_t net_in[NET_SAMPC * 2];
	int net_in_n = 0;
	int16_t modem_rx[MODEM_SAMPC_MAX];
	int16_t modem_tx[MODEM_SAMPC_MAX];
	unsigned int reported_tx_rate = 0;
	unsigned int reported_rx_rate = 0;

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

		/* Accumulate pipe reads so a short read cannot discard PCM. */
		{
			int16_t input[NET_SAMPC];
			int samples;
			int i;

			n = read(STDIN_FILENO, input, sizeof(input));
			if (n < 0 && errno != EAGAIN) {
				fprintf(stderr, "stdin read error: %s\n",
					strerror(errno));
				break;
			}
			if (n == 0) {
				fprintf(stderr, "stdin EOF\n");
				break;
			}
			samples = n > 0 ? (int)n / 2 : 0;
			for (i = 0; i < samples &&
			     net_in_n < (int)(sizeof(net_in) / sizeof(net_in[0])); i++)
				net_in[net_in_n++] = input[i];
		}

		while (net_in_n >= NET_SAMPC) {
			int converted;
			int modem_count = modem_sampc();
			int modem_offset = 0;

			memcpy(net_rx, net_in, sizeof(net_rx));
			memmove(net_in, net_in + NET_SAMPC,
				(size_t)(net_in_n - NET_SAMPC) * sizeof(net_in[0]));
			net_in_n -= NET_SAMPC;

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

			converted = resample_to_modem(net_rx, modem_rx);
			if (converted == 0)
				continue;
			if (converted != modem_sampc()) {
				fprintf(stderr,
					"resample: expected %d modem samples, got %d\n",
					modem_sampc(), converted);
				g_running = 0;
				break;
			}

			/* The original hardware drivers honor UPDATE_DELAY by letting
			 * their playback queue drain while discarding the same number
			 * of captured samples. V.32bis uses this to reduce excessive
			 * local buffering before training. */
			if (g_modem->update_delay < 0) {
				int drop = -g_modem->update_delay;

				if (drop > modem_count)
					drop = modem_count;
				modem_offset += drop;
				modem_count -= drop;
				g_modem->update_delay += drop;
				g_io_delay -= drop;
				fprintf(stderr,
					"I/O delay adjusted by -%d samples to %d\n",
					drop, g_io_delay);
			}
			if (modem_count == 0)
				continue;

			/* The original device loop calls modem_process() once per
			 * five-millisecond modem fragment.  Keep that boundary here as
			 * well: modem_process() advances sample timers only after the
			 * complete call, even though its data-pump layer can internally
			 * split a larger buffer. */
			{
				int processed = 0;

				while (processed < modem_count) {
					int fragment = modem_count - processed;

					if (fragment > (int)g_modem->frag)
						fragment = (int)g_modem->frag;
					modem_process(g_modem,
						modem_rx + modem_offset + processed,
						modem_tx + processed, fragment);
					processed += fragment;

					if (g_modem->tx_rate && g_modem->rx_rate &&
					    (g_modem->tx_rate != reported_tx_rate ||
					     g_modem->rx_rate != reported_rx_rate)) {
						reported_tx_rate = g_modem->tx_rate;
						reported_rx_rate = g_modem->rx_rate;
						fprintf(stderr,
							"Current data rate: TX=%u RX=%u bit/s\n",
							reported_tx_rate,
							reported_rx_rate);
					}
				}
			}

			converted = resample_to_net(modem_tx, modem_count, net_tx);
			if (converted < 0) {
				g_running = 0;
				break;
			}
			if (converted == 0)
				continue;

			if (emit_net_audio(txrec_fp, net_tx, converted) < 0) {
				g_running = 0;
				break;
			}

			while (g_modem->update_delay > 0) {
				int add = g_modem->update_delay;

				if (add > modem_sampc())
					add = modem_sampc();
				memset(modem_tx, 0, (size_t)add * sizeof(modem_tx[0]));
				converted = resample_to_net(modem_tx, add, net_tx);
				if (converted <= 0 ||
				    emit_net_audio(txrec_fp, net_tx, converted) < 0) {
					g_running = 0;
					break;
				}
				g_modem->update_delay -= add;
				g_io_delay += add;
				fprintf(stderr,
					"I/O delay adjusted by +%d samples to %d\n",
					add, g_io_delay);
			}
		}

		/* --- read AT/data from PTY and feed to modem --- */
		if (FD_ISSET(g_pty, &rset)) {
			char buf[256];
			n = read(g_pty, buf, sizeof(buf));
			if (n > 0) {
				int accepted = modem_write(g_modem, buf, (int)n);

				if (modem_debug_level > 1)
					fprintf(stderr,
						"PTY input: read %zd byte(s), modem accepted %d\n",
						n, accepted);
			}
		}
	}

	fprintf(stderr, "shutting down...\n");

	resampler_destroy();

	modem_delete(g_modem);
	dp_dummy_exit();
	dp_sinus_exit();
	prop_dp_exit();

	if (rec_fp)
		close_wav(rec_fp, "RX");
	if (txrec_fp)
		close_wav(txrec_fp, "TX");

	return 0;
}
