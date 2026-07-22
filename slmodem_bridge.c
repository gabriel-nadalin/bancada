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
static SRC_STATE *g_src_up;     /* net (8k) -> modem (9.6k) */
static SRC_STATE *g_src_down;   /* modem (9.6k) -> net (8k) */

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

static int resample_up(const int16_t *in, int16_t *out, int in_count, int out_count)
{
	float fin[NET_SAMPC], fout[MODEM_SAMPC];
	SRC_DATA data;
	int i;

	for (i = 0; i < in_count; i++)
		fin[i] = (float)in[i];

	data.data_in       = fin;
	data.data_out      = fout;
	data.input_frames  = in_count;
	data.output_frames = out_count;
	data.end_of_input  = 0;
	data.src_ratio     = (double)MODEM_RATE / (double)NET_RATE;

	if (src_process(g_src_up, &data) != 0)
		return -1;

	for (i = 0; i < data.output_frames_gen; i++)
		out[i] = (int16_t)fout[i];

	return (int)data.output_frames_gen;
}

static int resample_down(const int16_t *in, int16_t *out, int in_count, int out_count)
{
	float fin[MODEM_SAMPC], fout[NET_SAMPC];
	SRC_DATA data;
	int i;

	for (i = 0; i < in_count; i++)
		fin[i] = (float)in[i];

	data.data_in       = fin;
	data.data_out      = fout;
	data.input_frames  = in_count;
	data.output_frames = out_count;
	data.end_of_input  = 0;
	data.src_ratio     = (double)NET_RATE / (double)MODEM_RATE;

	if (src_process(g_src_down, &data) != 0)
		return -1;

	for (i = 0; i < data.output_frames_gen; i++)
		out[i] = (int16_t)fout[i];

	return (int)data.output_frames_gen;
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
		"  -d, --dial CMD     AT dial command (default: ATD\\r)\n"
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
	const char *mode = "orig";
	const char *dial_cmd = "ATD\r";
	int opt;
	int err;
	char pty_name[64];
	int frame = 0;

	while ((opt = getopt(argc, argv, "m:d:h")) != -1) {
		switch (opt) {
		case 'm':
			mode = optarg;
			break;
		case 'd':
			dial_cmd = optarg;
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

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	modem_debug_level = 1;
	modem_debug_init("bridge");

	dp_dummy_init();
	dp_sinus_init();
	prop_dp_init();
	modem_timer_init();

	g_src_up = src_new(SRC_SINC_FASTEST, 1, &err);
	g_src_down = src_new(SRC_SINC_FASTEST, 1, &err);
	if (!g_src_up || !g_src_down) {
		fprintf(stderr, "src_new failed\n");
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

	/* Set stdin/stdout to binary and non-blocking where helpful. */
	fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
	fcntl(g_pty, F_SETFL, O_NONBLOCK);

	int16_t net_rx[NET_SAMPC];
	int16_t net_tx[NET_SAMPC];
	int16_t modem_rx[MODEM_SAMPC];
	int16_t modem_tx[MODEM_SAMPC];

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
			int modem_in, net_out;

			/* resample 8k -> 9.6k */
			modem_in = resample_up(net_rx, modem_rx, NET_SAMPC, MODEM_SAMPC);
			if (modem_in < 0) {
				fprintf(stderr, "resample_up failed\n");
				break;
			}

			/* run modem DSP */
			modem_process(g_modem, modem_rx, modem_tx, modem_in);

			/* resample 9.6k -> 8k */
			net_out = resample_down(modem_tx, net_tx, modem_in, NET_SAMPC);
			if (net_out < 0) {
				fprintf(stderr, "resample_down failed\n");
				break;
			}

			/* write network PCM to stdout */
			size_t out_bytes = net_out * sizeof(int16_t);
			size_t written = 0;
			while (written < out_bytes) {
				n = write(STDOUT_FILENO, (char *)net_tx + written,
				          out_bytes - written);
				if (n < 0) {
					if (errno == EAGAIN)
						continue;
					fprintf(stderr, "stdout write error: %s\n",
					        strerror(errno));
					g_running = 0;
					break;
				}
				written += (size_t)n;
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

		/* --- auto-dial on first frame in originate mode --- */
		if (frame == 0 && strcmp(mode, "orig") == 0) {
			modem_write(g_modem, dial_cmd, (int)strlen(dial_cmd));
		}
		frame++;
	}

	fprintf(stderr, "shutting down...\n");

	modem_delete(g_modem);
	src_delete(g_src_up);
	src_delete(g_src_down);
	dp_dummy_exit();
	dp_sinus_exit();
	prop_dp_exit();

	return 0;
}
