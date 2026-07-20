#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <re.h>
#include <rem.h>

#define SRATE       8000
#define PTIME       20                        /* ms per RTP packet */
#define FRAME_SZ    (SRATE * PTIME / 1000)    /* samples per frame: 160 */
#define FRAME_BYTES (FRAME_SZ * 2)            /* s16le bytes per frame: 320 */
#define TX_BUF_SZ   (FRAME_BYTES * 10)        /* ~200 ms of stdin audio */
#define RX_MAX_SAMPC 4096                     /* max samples decoded/packet */

#define DEFAULT_LOCAL_IP "10.42.0.1"
#define DEFAULT_RTP_PORT 4000
#define DEFAULT_PEER_URI "sip:11@10.42.0.102:5062;transport=udp"

static struct sip *sip;
static struct sipsess_sock *sock;
static struct sipsess *sess;
static struct sdp_session *sdp_sess;
static struct udp_sock *rtp_socket;
static struct udp_sock *rtcp_socket;
static struct sa peer_rtp;
static struct re_fhs *stdin_watcher;
static struct tmr tx_tmr;
static uint64_t tx_next;
static FILE *wav_fp;
static FILE *wav_out;
static long wav_out_samples;

static char local_ip[64]  = DEFAULT_LOCAL_IP;
static char peer_uri[256] = DEFAULT_PEER_URI;
static int  rtp_port = DEFAULT_RTP_PORT;

static uint8_t  pt = 8;
static uint16_t rtp_seq;
static uint32_t rtp_ts;
static uint32_t rtp_ssrc;
static bool     call_active;
static bool     tx_marker = true;

static uint8_t  tx_buf[TX_BUF_SZ];
static size_t   tx_len;
static unsigned stdin_drops;
static bool     stdin_eof;

static int16_t  rx_samples[RX_MAX_SAMPC];
static uint16_t rx_last_seq;
static bool     rx_seq_valid;
static unsigned rx_pt_drops;

static void stdin_handler(int flags, void *arg);

static void send_rtp(const uint8_t *payload, size_t n) {
    uint8_t hdr[12];
    hdr[0] = 0x80;
    hdr[1] = (uint8_t)((pt & 0x7F) | (tx_marker ? 0x80 : 0));
    tx_marker = false;
    hdr[2] = (uint8_t)(rtp_seq >> 8);
    hdr[3] = (uint8_t)(rtp_seq & 0xFF); ++rtp_seq;
    hdr[4] = (uint8_t)(rtp_ts >> 24);
    hdr[5] = (uint8_t)(rtp_ts >> 16);
    hdr[6] = (uint8_t)(rtp_ts >> 8);
    hdr[7] = (uint8_t)(rtp_ts & 0xFF);
    /* PCMA @ 8 kHz: 1 payload byte == 1 sample, so n is the TS increment */
    rtp_ts += (uint32_t)n;
    hdr[8]  = (uint8_t)(rtp_ssrc >> 24);
    hdr[9]  = (uint8_t)(rtp_ssrc >> 16);
    hdr[10] = (uint8_t)(rtp_ssrc >> 8);
    hdr[11] = (uint8_t)(rtp_ssrc & 0xFF);

    struct mbuf *mb = mbuf_alloc(12 + n);
    if (!mb)
        return;
    mbuf_write_mem(mb, hdr, 12);
    mbuf_write_mem(mb, payload, n);
    mb->pos = 0;
    if (sa_isset(&peer_rtp, SA_ADDR)) {
        int err = udp_send(rtp_socket, &peer_rtp, mb);
        if (err)
            re_fprintf(stderr, "RTP send failed: %m\n", err);
    }
    mem_deref(mb);
}

static void encode_and_send(const int16_t *samples) {
    uint8_t pcma[FRAME_SZ];
    for (size_t i = 0; i < FRAME_SZ; i++)
        pcma[i] = g711_pcm2alaw(samples[i]);
    send_rtp(pcma, sizeof(pcma));
}

/*
 * 20 ms pacing timer: drains exactly one frame per tick, taken from the
 * WAV file or from the stdin accumulation buffer.  Rescheduled against an
 * absolute time base so processing time does not accumulate drift; if we
 * fall more than one frame behind, the schedule is resynced instead of
 * bursting catch-up packets.
 */
static void tx_tick(void *arg) {
    (void)arg;
    uint64_t now = tmr_jiffies();
    if (now - PTIME > tx_next)
        tx_next = now + PTIME;
    else
        tx_next += PTIME;
    tmr_start(&tx_tmr, tx_next > now ? tx_next - now : 1, tx_tick, NULL);

    if (!wav_fp && stdin_eof && tx_len < FRAME_BYTES) {
        /* stdin closed and remaining buffered audio drained: hang up */
        re_cancel();
        return;
    }

    int16_t samples[FRAME_SZ];
    if (wav_fp) {
        size_t n = fread(samples, 2, FRAME_SZ, wav_fp);
        if (n == 0) {
            re_fprintf(stderr, "WAV: EOF, hanging up\n");
            re_cancel();
            return;
        }
        for (size_t i = n; i < FRAME_SZ; i++)
            samples[i] = 0;
        encode_and_send(samples);
    }
    else if (tx_len >= FRAME_BYTES) {
        memcpy(samples, tx_buf, FRAME_BYTES);
        tx_len -= FRAME_BYTES;
        if (tx_len)
            memmove(tx_buf, tx_buf + FRAME_BYTES, tx_len);
        encode_and_send(samples);
        /* buffer was full and stdin unwatched: resume reading */
        if (!stdin_watcher && !stdin_eof &&
            fd_listen(&stdin_watcher, 0, FD_READ, stdin_handler, NULL)) {
            re_fprintf(stderr, "Failed to re-watch stdin\n");
            re_cancel();
        }
    }
}

/*
 * stdin only refills the accumulation buffer; the pacing timer consumes
 * it.  When the buffer is full, stdin is unwatched (back-pressure) so a
 * regular file or fast pipe cannot spin the event loop.
 */
static void stdin_handler(int flags, void *arg) {
    (void)flags;
    (void)arg;
    size_t space = sizeof(tx_buf) - tx_len;
    if (!space) {
        uint8_t scratch[512];
        (void)read(0, scratch, sizeof(scratch));
        stdin_watcher = fd_close(stdin_watcher);
        if (++stdin_drops % 50 == 1)
            re_fprintf(stderr, "stdin: TX buffer full, dropping audio"
                               " (%u times)\n", stdin_drops);
        return;
    }
    ssize_t n = read(0, tx_buf + tx_len, space);
    if (n <= 0) {
        re_fprintf(stderr, "stdin: %s, hanging up\n", n == 0 ? "EOF" : "error");
        stdin_watcher = fd_close(stdin_watcher);
        stdin_eof = true;
        if (tx_len == 0)
            re_cancel();
        return;
    }
    tx_len += (size_t)n;
    if (tx_len == sizeof(tx_buf))
        stdin_watcher = fd_close(stdin_watcher);
}

static int write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

static void rtp_handler(const struct sa *src, struct mbuf *mb, void *arg) {
    (void)src;
    (void)arg;
    if (!call_active)
        return;

    size_t len = mbuf_get_left(mb);
    if (len < 12)
        return;
    const uint8_t *p = mbuf_buf(mb);

    if ((p[0] >> 6) != 2)                         /* RTP version 2 */
        return;
    const unsigned cc      = p[0] & 0x0F;
    const bool     pad     = (p[0] >> 5) & 1;
    const bool     ext     = (p[0] >> 4) & 1;
    const uint8_t  pkt_pt  = p[1] & 0x7F;
    const uint16_t seq     = (uint16_t)((p[2] << 8) | p[3]);

    size_t hdrlen = 12 + 4 * cc;
    if (len < hdrlen)
        return;
    if (ext) {
        if (len < hdrlen + 4)
            return;
        hdrlen += 4 + 4 * (size_t)((p[hdrlen + 2] << 8) | p[hdrlen + 3]);
        if (len < hdrlen)
            return;
    }
    size_t paylen = len - hdrlen;
    if (pad) {
        uint8_t padlen = p[len - 1];
        if (padlen > paylen)
            return;
        paylen -= padlen;
    }

    if (pkt_pt != pt) {
        if (++rx_pt_drops == 1)
            re_fprintf(stderr, "RTP: dropping packets with pt=%u"
                               " (expecting %u)\n", pkt_pt, pt);
        return;
    }

    if (rx_seq_valid) {
        int16_t diff = (int16_t)(seq - rx_last_seq);
        if (diff <= 0)
            return;                               /* duplicate/reordered */
        if (diff > 1)
            re_fprintf(stderr, "RTP: gap of %d packet(s)\n", diff - 1);
    }
    rx_last_seq = seq;
    rx_seq_valid = true;

    if (paylen > RX_MAX_SAMPC) {
        re_fprintf(stderr, "RTP: oversized payload (%zu bytes),"
                           " truncating\n", paylen);
        paylen = RX_MAX_SAMPC;
    }
    for (size_t i = 0; i < paylen; i++)
        rx_samples[i] = g711_alaw2pcm(p[hdrlen + i]);

    if (paylen && write_all(1, rx_samples, paylen * 2)) {
        re_fprintf(stderr, "stdout: write failed (%m), hanging up\n", errno);
        re_cancel();
        return;
    }
    if (wav_out && paylen) {
        if (fwrite(rx_samples, 2, paylen, wav_out) != paylen)
            re_fprintf(stderr, "wav: write failed\n");
        else
            wav_out_samples += (long)paylen;
    }
}

/* drain RTCP so the peer's reports don't trigger ICMP port unreachable */
static void rtcp_handler(const struct sa *src, struct mbuf *mb, void *arg) {
    (void)src;
    (void)mb;
    (void)arg;
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* open a WAV file and position the stream at the audio data; the file must
 * be canonical PCM mono SRATE Hz 16-bit */
static int wav_open(const char *path) {
    wav_fp = fopen(path, "rb");
    if (!wav_fp) {
        re_fprintf(stderr, "Cannot open %s\n", path);
        return -1;
    }
    uint8_t hdr[12];
    if (fread(hdr, 1, sizeof(hdr), wav_fp) != sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        re_fprintf(stderr, "%s: not a RIFF/WAVE file\n", path);
        goto err;
    }
    bool fmt_ok = false;
    for (;;) {
        uint8_t chdr[8];
        if (fread(chdr, 1, sizeof(chdr), wav_fp) != sizeof(chdr)) {
            re_fprintf(stderr, "%s: no data chunk found\n", path);
            goto err;
        }
        uint32_t csz = le32(chdr + 4);
        if (!memcmp(chdr, "fmt ", 4)) {
            uint8_t fmt[16];
            if (csz < sizeof(fmt) ||
                fread(fmt, 1, sizeof(fmt), wav_fp) != sizeof(fmt)) {
                re_fprintf(stderr, "%s: bad fmt chunk\n", path);
                goto err;
            }
            if (le16(fmt) != 1 || le16(fmt + 2) != 1 ||
                le32(fmt + 4) != SRATE || le16(fmt + 14) != 16) {
                re_fprintf(stderr,
                           "%s: need PCM mono %u Hz 16-bit"
                           " (got fmt=%u ch=%u rate=%u bits=%u)\n",
                           path, SRATE, le16(fmt), le16(fmt + 2),
                           le32(fmt + 4), le16(fmt + 14));
                goto err;
            }
            fmt_ok = true;
            csz -= sizeof(fmt);
        }
        else if (!memcmp(chdr, "data", 4)) {
            if (!fmt_ok) {
                re_fprintf(stderr, "%s: missing fmt chunk\n", path);
                goto err;
            }
            return 0;                         /* positioned at audio data */
        }
        /* skip remainder of chunk (chunks are 2-byte aligned) */
        if (fseek(wav_fp, (long)(csz + (csz & 1)), SEEK_CUR)) {
            re_fprintf(stderr, "%s: truncated file\n", path);
            goto err;
        }
    }
err:
    fclose(wav_fp);
    wav_fp = NULL;
    return -1;
}

static int wav_out_open(const char *path) {
    wav_out = fopen(path, "wb");
    if (!wav_out) {
        re_fprintf(stderr, "Cannot create %s\n", path);
        return -1;
    }
    wav_out_samples = 0;
    uint16_t fmt = 1, nch = 1, bps = 16, blk = nch * bps / 8;
    uint32_t sr = SRATE, br = sr * blk;
    uint32_t tmp = 0;
    fwrite("RIFF", 4, 1, wav_out);
    fwrite(&tmp, 4, 1, wav_out);
    fwrite("WAVE", 4, 1, wav_out);
    fwrite("fmt ", 4, 1, wav_out);
    tmp = 16; fwrite(&tmp, 4, 1, wav_out);
    fwrite(&fmt, 2, 1, wav_out);
    fwrite(&nch, 2, 1, wav_out);
    fwrite(&sr, 4, 1, wav_out);
    fwrite(&br, 4, 1, wav_out);
    fwrite(&blk, 2, 1, wav_out);
    fwrite(&bps, 2, 1, wav_out);
    fwrite("data", 4, 1, wav_out);
    fwrite(&tmp, 4, 1, wav_out);
    return 0;
}

static void wav_out_close(void) {
    if (!wav_out)
        return;
    uint32_t data_sz = (uint32_t)wav_out_samples * 2;
    uint32_t riff_sz = 36 + data_sz;
    fseek(wav_out, 4, SEEK_SET);
    fwrite(&riff_sz, 4, 1, wav_out);
    fseek(wav_out, 40, SEEK_SET);
    fwrite(&data_sz, 4, 1, wav_out);
    fclose(wav_out);
    wav_out = NULL;
}

static void start_media(void) {
    call_active = true;
    tx_marker = true;
    tx_next = tmr_jiffies() + PTIME;
    tmr_start(&tx_tmr, PTIME, tx_tick, NULL);

    if (wav_fp) {
        re_fprintf(stderr, "Streaming WAV file\n");
    }
    else {
        if (fd_listen(&stdin_watcher, 0, FD_READ, stdin_handler, NULL)) {
            re_fprintf(stderr, "Failed to watch stdin\n");
            re_cancel();
            return;
        }
        re_fprintf(stderr, "Bridging stdin/stdout\n");
    }
    re_fprintf(stderr, "Call established\n");
}

/* build the persistent local SDP session (our offer) once at startup;
 * the answer is later decoded into the same session so libre can match
 * the remote media against it */
static int sdp_setup(void) {
    struct sa laddr;
    sa_set_str(&laddr, local_ip, 0);
    struct sdp_media *m = NULL;
    int err = sdp_session_alloc(&sdp_sess, &laddr);
    if (err)
        return err;
    err = sdp_media_add(&m, sdp_sess, sdp_media_audio,
                        (uint16_t)rtp_port, sdp_proto_rtpavp);
    if (err)
        return err;
    sdp_media_set_ldir(m, SDP_SENDRECV);
    err = sdp_format_add(NULL, m, false, "8", "PCMA", SRATE, 1,
                         NULL, NULL, NULL, false, NULL);
    if (err)
        return err;
    sdp_media_set_lattr(m, false, "ptime", "%u", PTIME);
    return 0;
}

static int desc_handler(struct mbuf **descp, const struct sa *src,
                        const struct sa *dst, void *arg) {
    (void)src;
    (void)dst;
    (void)arg;
    return sdp_encode(descp, sdp_sess, true);
}

static int answer_handler(const struct sip_msg *msg, void *arg) {
    (void)msg;
    (void)arg;
    return 0;
}

static void estab_handler(const struct sip_msg *msg, void *arg) {
    (void)arg;
    const char *fail = NULL;

    /* decode the answer into our persistent session (matches the offer) */
    if (sdp_decode(sdp_sess, msg->mb, false)) {
        fail = "no/invalid SDP in answer";
        goto out;
    }
    struct le *le = list_head(sdp_session_medial(sdp_sess, false));
    struct sdp_media *rm = le ? le->data : NULL;
    if (!rm || !sdp_media_rport(rm)) {
        fail = "no usable audio media in answer";
        goto out;
    }
    if (!sa_isset(sdp_media_raddr(rm), SA_ADDR)) {
        fail = "no remote RTP address in answer";
        goto out;
    }
    const struct sdp_format *rfmt = sdp_media_rformat(rm, "PCMA");
    if (!rfmt) {
        fail = "peer did not accept PCMA";
        goto out;
    }
    sa_cpy(&peer_rtp, sdp_media_raddr(rm));
    sa_set_port(&peer_rtp, sdp_media_rport(rm));
    pt = rfmt->pt;
    re_fprintf(stderr, "Peer RTP: %J pt=%u\n", &peer_rtp, pt);

out:
    if (fail) {
        re_fprintf(stderr, "Call setup failed: %s\n", fail);
        re_cancel();
        return;
    }
    start_media();
}

static void close_handler(int err, const struct sip_msg *msg, void *arg) {
    (void)msg;
    (void)arg;
    if (err)
        re_fprintf(stderr, "Call closed: %m\n", err);
    else
        re_fprintf(stderr, "Call ended\n");
    re_cancel();
}

static void progr_handler(const struct sip_msg *msg, void *arg) {
    (void)arg;
    re_fprintf(stderr, "SIP: %u %r\n", msg->scode, &msg->reason);
}

static void signal_handler(int sig) {
    (void)sig;
    re_cancel();
}

static void usage(const char *name) {
    re_fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "SIP options:\n"
        "  -p, --peer URI        Peer SIP URI to dial\n"
        "                        (default: %s)\n"
        "  -l, --local-ip IP     Local IP address (default: %s)\n"
        "  -P, --rtp-port PORT   Local RTP port (default: %u)\n"
        "Audio options:\n"
        "  -i, --input FILE      Play WAV file instead of stdin\n"
        "                        (PCM mono %u Hz 16-bit)\n"
        "  -o, --output FILE     Record received audio to WAV file\n"
        "  -h, --help            Show this help\n"
        "\n"
        "Without -i, raw s16le/%u Hz/mono audio is read from stdin;\n"
        "received audio is always written to stdout in the same format.\n",
        name, DEFAULT_PEER_URI, DEFAULT_LOCAL_IP, DEFAULT_RTP_PORT,
        SRATE, SRATE);
}

int main(int argc, char *argv[]) {
    const char *input_path = NULL;
    const char *output_path = NULL;
    int err = 0;

    static const struct option longopts[] = {
        {"peer",     required_argument, NULL, 'p'},
        {"local-ip", required_argument, NULL, 'l'},
        {"rtp-port", required_argument, NULL, 'P'},
        {"input",    required_argument, NULL, 'i'},
        {"output",   required_argument, NULL, 'o'},
        {"help",     no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    for (;;) {
        int c = getopt_long(argc, argv, "p:l:P:i:o:h", longopts, NULL);
        if (c == -1)
            break;
        switch (c) {
        case 'p':
            str_ncpy(peer_uri, optarg, sizeof(peer_uri));
            break;
        case 'l':
            str_ncpy(local_ip, optarg, sizeof(local_ip));
            break;
        case 'P':
            rtp_port = atoi(optarg);
            if (rtp_port < 1 || rtp_port > 65534) {
                re_fprintf(stderr, "Invalid RTP port %s\n", optarg);
                return 1;
            }
            break;
        case 'i':
            input_path = optarg;
            break;
        case 'o':
            output_path = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    err = libre_init();
    if (err)
        return 1;
    poll_method_set(METHOD_SELECT);
    signal(SIGPIPE, SIG_IGN);

    rtp_ssrc = rand_u32();
    rtp_seq  = rand_u16();
    rtp_ts   = rand_u32();

    if (input_path && wav_open(input_path)) {
        err = 1;
        goto out;
    }
    if (output_path && wav_out_open(output_path)) {
        err = 1;
        goto out;
    }

    struct sa laddr;
    err = sa_set_str(&laddr, local_ip, 5060);
    if (err) {
        re_fprintf(stderr, "Invalid local IP %s\n", local_ip);
        goto out;
    }
    if (sip_alloc(&sip, NULL, 32, 32, 32, "rtp_bridge", NULL, NULL) ||
        sip_transp_add(sip, SIP_TRANSP_UDP, &laddr, NULL)) {
        re_fprintf(stderr, "SIP init failed\n");
        err = 1;
        goto out;
    }

    /* bind RTP before the INVITE goes out: the port is already offered in
     * the SDP, so early media must not hit a closed port */
    struct sa rtp_local;
    sa_set_str(&rtp_local, local_ip, (uint16_t)rtp_port);
    if (udp_listen(&rtp_socket, &rtp_local, rtp_handler, NULL)) {
        re_fprintf(stderr, "Failed to bind RTP socket %J\n", &rtp_local);
        err = 1;
        goto out;
    }
    struct sa rtcp_local;
    sa_set_str(&rtcp_local, local_ip, (uint16_t)(rtp_port + 1));
    if (udp_listen(&rtcp_socket, &rtcp_local, rtcp_handler, NULL))
        re_fprintf(stderr, "Warning: cannot bind RTCP port %u\n",
                   rtp_port + 1);

    err = sdp_setup();
    if (err) {
        re_fprintf(stderr, "SDP setup failed: %m\n", err);
        goto out;
    }

    if (sipsess_listen(&sock, sip, 32, NULL, NULL)) {
        re_fprintf(stderr, "sipsess_listen failed\n");
        err = 1;
        goto out;
    }

    char from_uri[96];
    re_snprintf(from_uri, sizeof(from_uri), "sip:12@%s", local_ip);

    re_fprintf(stderr, "Dialing %s ...\n", peer_uri);

    err = sipsess_connect(&sess, sock,
                          peer_uri,
                          NULL,
                          from_uri,
                          "12", NULL, 0,
                          "application/sdp",
                          NULL, NULL, false,
                          NULL,
                          desc_handler,
                          NULL, answer_handler,
                          progr_handler,
                          estab_handler,
                          NULL, NULL,
                          close_handler, NULL,
                          "");
    if (err) {
        re_fprintf(stderr, "sipsess_connect failed: %m\n", err);
        goto out;
    }

    re_main(signal_handler);

out:
    call_active = false;
    tmr_cancel(&tx_tmr);
    wav_out_close();
    if (wav_fp)
        fclose(wav_fp);
    stdin_watcher = fd_close(stdin_watcher);
    rtcp_socket = mem_deref(rtcp_socket);
    rtp_socket = mem_deref(rtp_socket);
    sess = mem_deref(sess);
    sock = mem_deref(sock);
    sip = mem_deref(sip);
    sdp_sess = mem_deref(sdp_sess);
    libre_close();
    return err;
}
