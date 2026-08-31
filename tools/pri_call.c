#include <dahdi/user.h>
#include <errno.h>
#include <fcntl.h>
#include <libpri.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <rem_g711.h>

struct options {
	const char *dchan;
	const char *called;
	const char *caller;
	int keepalive_only;
	int channel;
	int hold_seconds;
	int setup_delay;
	int debug;
	int transcap;
	int layer1;
	int number_plan;
	int switchtype;
	int nodetype;
	int audio_bridge;
};

struct state {
	struct pri *pri;
	q931_call *call;
	const struct options *opts;
	time_t connected_at;
	int dchan_up;
	time_t dchan_up_at;
	int setup_sent;
	int connected;
	int hangup_sent;
	int done;
	int selected_channel;
	int bchan_fd;
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void log_line(const char *level, const char *fmt, ...)
{
	char ts[32];
	time_t now = time(NULL);
	struct tm tm;
	va_list ap;

	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
	fprintf(stderr, "%s %-5s ", ts, level);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void pri_message(struct pri *pri, char *msg)
{
	(void)pri;
	fprintf(stdout, "%s", msg);
}

static void pri_error(struct pri *pri, char *msg)
{
	(void)pri;
	fprintf(stderr, "%s", msg);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [options] <called-number>\n"
		"\n"
		"Options:\n"
		"  -d <device>       D-channel device or number (default: 16)\n"
		"  -c <caller>       Calling number (default: 11)\n"
		"  -b <channel>      Preferred B-channel, 0 means any (default: 1)\n"
		"  -t <seconds>      Hold call after CONNECT (default: 5)\n"
		"  -p <seconds>      Wait after D-channel up before SETUP (default: 1)\n"
		"  -n <node>         PRI node type: cpe or network (default: cpe)\n"
		"  -w <switch>       Switch type: euroisdn or ni2 (default: euroisdn)\n"
		"  -k                Keep Q.921 up only; do not place a call\n"
		"  -a                Use 3.1 kHz audio/A-law bearer (default)\n"
		"  -s                Use speech/A-law bearer\n"
		"  -A                Audio-bridge mode: after CONNECT, open the\n"
		"                    connected B-channel and bridge 8k A-law (DAHDI)\n"
		"                    <-> 8k linear s16le on stdin/stdout; hold the\n"
		"                    call until SIGINT (ignores -t)\n"
		"  -D                Enable full libpri protocol debug\n"
		"  -h                Show this help\n"
		"\n"
		"Use '-' as <called-number> to send an empty Called Party Number IE.\n"
		"This process runs as PRI CPE/TE with EuroISDN E1.\n",
		argv0);
}

static int parse_node(const char *value)
{
	if (!strcmp(value, "cpe"))
		return PRI_CPE;
	if (!strcmp(value, "network"))
		return PRI_NETWORK;
	fprintf(stderr, "Invalid node type: %s\n", value);
	exit(2);
}

static int parse_switch(const char *value)
{
	if (!strcmp(value, "euroisdn"))
		return PRI_SWITCH_EUROISDN_E1;
	if (!strcmp(value, "ni2"))
		return PRI_SWITCH_NI2;
	fprintf(stderr, "Invalid switch type: %s\n", value);
	exit(2);
}

static int parse_int(const char *value, const char *name)
{
	char *end = NULL;
	long parsed;

	errno = 0;
	parsed = strtol(value, &end, 10);
	if (errno || !end || *end || parsed < 0 || parsed > 255) {
		fprintf(stderr, "Invalid %s: %s\n", name, value);
		exit(2);
	}
	return (int)parsed;
}

static void parse_options(int argc, char **argv, struct options *opts)
{
	int opt;

	opts->dchan = "16";
	opts->caller = "11";
	opts->keepalive_only = 0;
	opts->channel = 1;
	opts->hold_seconds = 5;
	opts->setup_delay = 1;
	opts->debug = 0;
	opts->transcap = PRI_TRANS_CAP_3_1K_AUDIO;
	opts->layer1 = PRI_LAYER_1_ALAW;
	opts->number_plan = PRI_NPI_E163_E164 | PRI_TON_UNKNOWN;
	opts->switchtype = PRI_SWITCH_EUROISDN_E1;
	opts->nodetype = PRI_CPE;
	opts->audio_bridge = 0;

	while ((opt = getopt(argc, argv, "d:c:b:t:p:n:w:kasDAh")) != -1) {
		switch (opt) {
		case 'd':
			opts->dchan = optarg;
			break;
		case 'c':
			opts->caller = optarg;
			break;
		case 'b':
			opts->channel = parse_int(optarg, "B-channel");
			break;
		case 't':
			opts->hold_seconds = parse_int(optarg, "hold time");
			break;
		case 'p':
			opts->setup_delay = parse_int(optarg, "setup delay");
			break;
		case 'n':
			opts->nodetype = parse_node(optarg);
			break;
		case 'w':
			opts->switchtype = parse_switch(optarg);
			break;
		case 'k':
			opts->keepalive_only = 1;
			break;
		case 'a':
			opts->transcap = PRI_TRANS_CAP_3_1K_AUDIO;
			opts->layer1 = PRI_LAYER_1_ALAW;
			break;
		case 's':
			opts->transcap = PRI_TRANS_CAP_SPEECH;
			opts->layer1 = PRI_LAYER_1_ALAW;
			break;
		case 'A':
			opts->audio_bridge = 1;
			break;
		case 'D':
			opts->debug = 1;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			exit(2);
		}
	}

	if ((!opts->keepalive_only && optind + 1 != argc) || (opts->keepalive_only && optind != argc)) {
		usage(argv[0]);
		exit(2);
	}
	opts->called = opts->keepalive_only ? NULL : argv[optind];
}

static int is_channel_number(const char *value)
{
	if (!value || !*value)
		return 0;
	while (*value) {
		if (*value < '0' || *value > '9')
			return 0;
		value++;
	}
	return 1;
}

static int open_dchan(const char *path)
{
	struct dahdi_params params;
	const char *open_path = path;
	int fd;
	int flags;
	int channel = 0;

	if (is_channel_number(path)) {
		channel = parse_int(path, "D-channel");
		open_path = "/dev/dahdi/channel";
	}

	fd = open(open_path, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror(open_path);
		exit(1);
	}
	if (channel && ioctl(fd, DAHDI_SPECIFY, &channel) < 0) {
		perror("DAHDI_SPECIFY");
		exit(1);
	}

	memset(&params, 0, sizeof(params));
	if (ioctl(fd, DAHDI_GET_PARAMS, &params) < 0) {
		perror("DAHDI_GET_PARAMS");
		exit(1);
	}
	if (params.sigtype != DAHDI_SIG_HDLCRAW && params.sigtype != DAHDI_SIG_HDLCFCS
		&& params.sigtype != DAHDI_SIG_HARDHDLC) {
		fprintf(stderr, "%s sigtype is %d, expected HDLC RAW/FCS/HARDHDLC\n", path, params.sigtype);
		exit(1);
	}

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("fcntl O_NONBLOCK");
		exit(1);
	}

	return fd;
}

/* Open a Clear B-channel for raw A-law sample I/O. */
static int open_bchan(int channel)
{
	struct dahdi_params params;
	int fd;
	int flags;

	if (channel <= 0)
		return -1;

	fd = open("/dev/dahdi/channel", O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("/dev/dahdi/channel");
		return -1;
	}
	if (ioctl(fd, DAHDI_SPECIFY, &channel) < 0) {
		perror("DAHDI_SPECIFY");
		close(fd);
		return -1;
	}

	memset(&params, 0, sizeof(params));
	if (ioctl(fd, DAHDI_GET_PARAMS, &params) < 0) {
		perror("DAHDI_GET_PARAMS");
		close(fd);
		return -1;
	}
	log_line("INFO", "B-channel %d open sigtype=%d (expect %d Clear)",
		channel, params.sigtype, DAHDI_SIG_CLEAR);

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("fcntl O_NONBLOCK");
		close(fd);
		return -1;
	}

	return fd;
}

static void place_call(struct state *st)
{
	struct pri_sr *sr;

	if (st->setup_sent)
		return;

	st->call = pri_new_call(st->pri);
	if (!st->call) {
		log_line("ERROR", "pri_new_call failed");
		st->done = 1;
		return;
	}

	sr = pri_sr_new();
	if (!sr) {
		log_line("ERROR", "pri_sr_new failed");
		st->done = 1;
		return;
	}

	pri_sr_set_channel(sr, st->opts->channel, st->opts->channel ? 1 : 0, 1);
	pri_sr_set_bearer(sr, st->opts->transcap, st->opts->layer1);
	pri_sr_set_called(sr, (char *)(strcmp(st->opts->called, "-") ? st->opts->called : ""),
		st->opts->number_plan, 1);
	pri_sr_set_caller(sr, (char *)st->opts->caller, NULL, st->opts->number_plan,
		PRES_ALLOWED_USER_NUMBER_NOT_SCREENED);

	log_line("INFO", "sending SETUP called=%s caller=%s channel=%s bearer=%s/A-law%s",
		st->opts->called,
		st->opts->caller,
		st->opts->channel ? "explicit" : "any",
		st->opts->transcap == PRI_TRANS_CAP_SPEECH ? "speech" : "3.1kHz audio",
		st->opts->audio_bridge ? " (audio bridge)" : "");

	if (pri_setup(st->pri, st->call, sr)) {
		log_line("ERROR", "pri_setup failed");
		st->done = 1;
	} else {
		st->setup_sent = 1;
	}

	pri_sr_free(sr);
}

static void hangup_call(struct state *st, int cause)
{
	if (!st->call || st->hangup_sent)
		return;
	log_line("INFO", "sending hangup cause=%d (%s)", cause, pri_cause2str(cause));
	pri_hangup(st->pri, st->call, cause);
	st->hangup_sent = 1;
}

/* B-channel (A-law) -> stdout (8k linear s16le) */
static void pump_bchan_to_stdout(struct state *st)
{
	uint8_t alaw[1024];
	int16_t lin[1024];
	uint8_t out[2048];
	ssize_t n;
	ssize_t i;

	n = read(st->bchan_fd, alaw, sizeof(alaw));
	if (n <= 0)
		return;

	for (i = 0; i < n; i++)
		lin[i] = g711_alaw2pcm(alaw[i]);
	for (i = 0; i < n; i++) {
		out[i * 2]     = (uint8_t)(lin[i] & 0xff);
		out[i * 2 + 1] = (uint8_t)((lin[i] >> 8) & 0xff);
	}

	i = 0;
	while (i < n * 2) {
		ssize_t wr = write(STDOUT_FILENO, out + i, n * 2 - i);
		if (wr < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		i += wr;
	}
}

/* stdin (8k linear s16le) -> B-channel (A-law) */
static void pump_stdin_to_bchan(struct state *st)
{
	uint8_t in[2048];
	uint8_t alaw[1024];
	ssize_t n;
	ssize_t cnt;
	ssize_t i;

	n = read(STDIN_FILENO, in, sizeof(in));
	if (n <= 0)
		return;
	if (n % 2)
		n--;                        /* ignore trailing odd byte */

	cnt = n / 2;
	for (i = 0; i < cnt; i++) {
		int16_t l = (int16_t)((uint16_t)in[i * 2] | ((uint16_t)in[i * 2 + 1] << 8));
		alaw[i] = g711_pcm2alaw(l);
	}

	i = 0;
	while (i < cnt) {
		ssize_t wr = write(st->bchan_fd, alaw + i, cnt - i);
		if (wr < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		i += wr;
	}
}

/* Open the B-channel for audio as early as the channel is known (RINGING),
 * so the slmodem can hear the RAS's V8bis before CONNECT cuts through. */
static void open_bchan_if_needed(struct state *st, int channel)
{
	if (!st->opts->audio_bridge || st->bchan_fd > 0 || channel <= 0)
		return;
	st->bchan_fd = open_bchan(channel);
	if (st->bchan_fd <= 0) {
		log_line("ERROR", "failed to open B-channel %d", channel);
		hangup_call(st, PRI_CAUSE_NORMAL_CLEARING);
	} else {
		log_line("INFO", "audio bridge active on channel %d", channel);
	}
}

static void handle_event(struct state *st, pri_event *event)
{
	if (!event)
		return;

	log_line("PRI", "%s (%d)", pri_event2str(event->e), event->e);

	switch (event->e) {
	case PRI_EVENT_DCHAN_UP:
		st->dchan_up = 1;
		st->dchan_up_at = time(NULL);
		log_line("INFO", "D-channel established%s", st->opts->keepalive_only ? "; keeping Q.921 up" : "");
		break;
	case PRI_EVENT_DCHAN_DOWN:
		st->dchan_up = 0;
		break;
	case PRI_EVENT_PROCEEDING:
		st->selected_channel = event->proceeding.channel;
		log_line("INFO", "call proceeding channel=%d cause=%d", event->proceeding.channel,
			event->proceeding.cause);
		break;
	case PRI_EVENT_RINGING:
		st->selected_channel = event->ringing.channel;
		log_line("INFO", "remote alerting channel=%d", event->ringing.channel);
		open_bchan_if_needed(st, event->ringing.channel);
		break;
	case PRI_EVENT_ANSWER:
		st->connected = 1;
		st->connected_at = time(NULL);
		st->selected_channel = event->answer.channel;
		log_line("INFO", "connected channel=%d", event->answer.channel);
		open_bchan_if_needed(st, event->answer.channel);
		break;
	case PRI_EVENT_CONNECT_ACK:
		st->selected_channel = event->connect_ack.channel;
		log_line("INFO", "connect ack channel=%d", event->connect_ack.channel);
		break;
	case PRI_EVENT_HANGUP:
		log_line("INFO", "remote hangup channel=%d cause=%d (%s)", event->hangup.channel,
			event->hangup.cause, pri_cause2str(event->hangup.cause));
		if (event->hangup.call)
			pri_hangup(st->pri, event->hangup.call, event->hangup.cause);
		st->done = 1;
		break;
	case PRI_EVENT_HANGUP_REQ:
		log_line("INFO", "remote disconnect channel=%d cause=%d (%s)", event->hangup.channel,
			event->hangup.cause, pri_cause2str(event->hangup.cause));
		hangup_call(st, event->hangup.cause ? event->hangup.cause : PRI_CAUSE_NORMAL_CLEARING);
		break;
	case PRI_EVENT_HANGUP_ACK:
		log_line("INFO", "hangup acknowledged");
		st->done = 1;
		break;
	case PRI_EVENT_RESTART:
		log_line("INFO", "restart channel=%d", event->restart.channel);
		break;
	case PRI_EVENT_CONFIG_ERR:
		log_line("ERROR", "configuration error: %s", event->err.err);
		break;
	default:
		break;
	}
}

static void maybe_run_timers(struct state *st)
{
	if (stop_requested) {
		hangup_call(st, PRI_CAUSE_NORMAL_CLEARING);
		if (!st->call)
			st->done = 1;
	}

	if (!st->opts->audio_bridge && st->connected && !st->hangup_sent &&
	    st->opts->hold_seconds > 0) {
		if (time(NULL) - st->connected_at >= st->opts->hold_seconds)
			hangup_call(st, PRI_CAUSE_NORMAL_CLEARING);
	}

	if (st->dchan_up && !st->opts->keepalive_only && !st->setup_sent) {
		if (time(NULL) - st->dchan_up_at >= st->opts->setup_delay)
			place_call(st);
	}
}

int main(int argc, char **argv)
{
	struct options opts;
	struct state st;
	int fd;

	parse_options(argc, argv, &opts);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	pri_set_message(pri_message);
	pri_set_error(pri_error);

	fd = open_dchan(opts.dchan);
	memset(&st, 0, sizeof(st));
	st.opts = &opts;
	st.selected_channel = -1;
	st.bchan_fd = 0;
	st.pri = pri_new(fd, opts.nodetype, opts.switchtype);
	if (!st.pri) {
		log_line("ERROR", "pri_new failed");
		return 1;
	}
	pri_set_debug(st.pri, opts.debug ? PRI_DEBUG_ALL : 0);

	log_line("INFO", "opened %s as %s %s%s", opts.dchan,
		opts.nodetype == PRI_CPE ? "CPE" : "network",
		opts.switchtype == PRI_SWITCH_EUROISDN_E1 ? "EuroISDN E1" : "NI2",
		opts.audio_bridge ? " (audio bridge)" : "");

	while (!st.done) {
		struct timeval tv;
		struct timeval *next;
		fd_set rfds;
		int maxfd = fd;
		int res;

		maybe_run_timers(&st);

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		if (st.bchan_fd > 0) {
			FD_SET(st.bchan_fd, &rfds);
			if (st.bchan_fd > maxfd)
				maxfd = st.bchan_fd;
		}
		if (opts.audio_bridge) {
			FD_SET(STDIN_FILENO, &rfds);
			if (STDIN_FILENO > maxfd)
				maxfd = STDIN_FILENO;
		}
		next = pri_schedule_next(st.pri);
		if (next) {
			struct timeval now;
			gettimeofday(&now, NULL);
			tv.tv_sec = next->tv_sec - now.tv_sec;
			tv.tv_usec = next->tv_usec - now.tv_usec;
			if (tv.tv_usec < 0) {
				tv.tv_usec += 1000000;
				tv.tv_sec--;
			}
			if (tv.tv_sec < 0) {
				tv.tv_sec = 0;
				tv.tv_usec = 0;
			}
		} else {
			tv.tv_sec = 1;
			tv.tv_usec = 0;
		}

		res = select(maxfd + 1, &rfds, NULL, NULL, &tv);
		if (res < 0) {
			if (errno == EINTR)
				continue;
			perror("select");
			break;
		}

		if (res == 0) {
			handle_event(&st, pri_schedule_run(st.pri));
		} else {
			if (st.bchan_fd > 0 && FD_ISSET(st.bchan_fd, &rfds))
				pump_bchan_to_stdout(&st);
			if (opts.audio_bridge && FD_ISSET(STDIN_FILENO, &rfds))
				pump_stdin_to_bchan(&st);
			if (FD_ISSET(fd, &rfds))
				handle_event(&st, pri_check_event(st.pri));
		}
	}

	if (st.bchan_fd > 0)
		close(st.bchan_fd);
	close(fd);

	log_line("INFO", "finished connected=%s channel=%d", st.connected ? "yes" : "no",
		st.selected_channel);
	return st.connected ? 0 : 1;
}
