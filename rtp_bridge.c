#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <re.h>
#include <rem.h>

#define SRATE      8000
#define PTIME      20
#define FRAME_SZ   (SRATE * PTIME / 1000)
#define FRAME_BYTES (FRAME_SZ * 2)
#define BUF_SZ     (FRAME_BYTES * 2)
#define LOCAL_IP   "10.42.0.1"
#define LOCAL_SIP  "sip:12@10.42.0.1"
#define PEER_URI   "sip:11@10.42.0.102:5062;transport=udp"

static struct sip *sip;
static struct sipsess_sock *sock;
static struct sipsess *sess;
static struct udp_sock *rtp_socket;
static struct sa peer_rtp;
static struct re_fhs *stdin_watcher;
static struct tmr wav_tmr;
static FILE *wav_fp;
static FILE *wav_out;
static int    wav_out_samples;

static uint8_t  pt = 8;
static uint16_t rtp_seq;
static uint32_t rtp_ts;
static uint32_t rtp_ssrc;
static bool     call_active;

static uint8_t  tx_buf[BUF_SZ];
static size_t   tx_len;

static void send_rtp(const uint8_t *payload, size_t n) {
    uint8_t hdr[12];
    hdr[0] = 0x80;
    hdr[1] = pt & 0x7F;
    hdr[2] = (rtp_seq >> 8) & 0xFF;
    hdr[3] = rtp_seq & 0xFF; ++rtp_seq;
    hdr[4] = (rtp_ts >> 24) & 0xFF;
    hdr[5] = (rtp_ts >> 16) & 0xFF;
    hdr[6] = (rtp_ts >> 8) & 0xFF;
    hdr[7] = rtp_ts & 0xFF; rtp_ts += (uint32_t)n;
    hdr[8]  = (rtp_ssrc >> 24) & 0xFF;
    hdr[9]  = (rtp_ssrc >> 16) & 0xFF;
    hdr[10] = (rtp_ssrc >> 8) & 0xFF;
    hdr[11] = rtp_ssrc & 0xFF;
    struct mbuf *mb = mbuf_alloc(12 + n);
    mbuf_write_mem(mb, hdr, 12);
    mbuf_write_mem(mb, payload, n);
    mb->pos = 0;
    if (sa_isset(&peer_rtp, SA_ADDR))
        udp_send(rtp_socket, &peer_rtp, mb);
    mem_deref(mb);
}

static void encode_and_send(void) {
    uint8_t pcma[FRAME_SZ];
    int16_t *samples = (int16_t *)tx_buf;
    for (size_t i = 0; i < FRAME_SZ; i++)
        pcma[i] = g711_pcm2alaw(samples[i]);
    send_rtp(pcma, FRAME_SZ);
}

static void stdin_handler(int flags, void *arg) {
    (void)flags;
    (void)arg;
    ssize_t n = read(0, tx_buf + tx_len, sizeof(tx_buf) - tx_len);
    if (n <= 0) {
        re_fprintf(stderr, "stdin: %s, hanging up\n", n == 0 ? "EOF" : "error");
        fd_close(stdin_watcher);
        stdin_watcher = NULL;
        re_cancel();
        return;
    }
    tx_len += (size_t)n;
    while (tx_len >= FRAME_BYTES) {
        encode_and_send();
        tx_len -= FRAME_BYTES;
        if (tx_len)
            memmove(tx_buf, tx_buf + FRAME_BYTES, tx_len);
    }
}

static void wav_timer_handler(void *arg) {
    (void)arg;
    int16_t samples[FRAME_SZ];
    size_t n = fread(samples, 2, FRAME_SZ, wav_fp);
    if (n == 0) {
        re_fprintf(stderr, "WAV: EOF, hanging up\n");
        re_cancel();
        return;
    }
    uint8_t pcma[FRAME_SZ];
    for (size_t i = 0; i < n; i++)
        pcma[i] = g711_pcm2alaw(samples[i]);
    for (size_t i = n; i < FRAME_SZ; i++)
        pcma[i] = g711_pcm2alaw(0);
    send_rtp(pcma, FRAME_SZ);
    tmr_start(&wav_tmr, PTIME, wav_timer_handler, NULL);
}

static void wav_out_close(void) {
    if (!wav_out) return;
    uint32_t data_sz = (uint32_t)wav_out_samples * 2;
    uint32_t riff_sz = 36 + data_sz;
    fseek(wav_out, 4, SEEK_SET);
    fwrite(&riff_sz, 4, 1, wav_out);
    fseek(wav_out, 40, SEEK_SET);
    fwrite(&data_sz, 4, 1, wav_out);
    fclose(wav_out);
    wav_out = NULL;
}

static void rtp_handler(const struct sa *src, struct mbuf *mb, void *arg) {
    (void)src; (void)arg;
    if (mbuf_get_left(mb) < 12) return;
    mbuf_advance(mb, 12);
    size_t n = mbuf_get_left(mb);
    const uint8_t *p = mbuf_buf(mb);
    int16_t samples[256];
    for (size_t i = 0; i < n && i < 256; i++) {
        samples[i] = g711_alaw2pcm(p[i]);
        write(1, &samples[i], 2);
    }
    if (wav_out) {
        fwrite(samples, 2, n, wav_out);
        wav_out_samples += (int)n;
    }
}

static int wav_open(const char *path) {
    wav_fp = fopen(path, "rb");
    if (!wav_fp) {
        re_fprintf(stderr, "Cannot open %s\n", path);
        return -1;
    }
    fseek(wav_fp, 44, SEEK_SET);
    return 0;
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

static void bind_rtp_and_audio(void) {
    struct sa local_rtp;
    sa_set_str(&local_rtp, LOCAL_IP, 4000);
    if (udp_listen(&rtp_socket, &local_rtp, rtp_handler, NULL)) {
        re_fprintf(stderr, "Failed to bind RTP socket\n");
        re_cancel();
        return;
    }
    call_active = true;
    if (wav_fp) {
        re_fprintf(stderr, "Streaming WAV file\n");
        tmr_start(&wav_tmr, 1, wav_timer_handler, NULL);
    } else {
        if (fd_listen(&stdin_watcher, 0, FD_READ, stdin_handler, NULL)) {
            re_fprintf(stderr, "Failed to watch stdin\n");
            re_cancel();
            return;
        }
        re_fprintf(stderr, "Bridging stdin/stdout\n");
    }
    re_fprintf(stderr, "Call established\n");
}

static int desc_handler(struct mbuf **descp, const struct sa *src,
                        const struct sa *dst, void *arg) {
    (void)src; (void)dst; (void)arg;
    struct sa laddr;
    sa_set_str(&laddr, LOCAL_IP, 0);
    struct sdp_session *sess = NULL;
    sdp_session_alloc(&sess, &laddr);
    struct sdp_media *m = NULL;
    sdp_media_add(&m, sess, sdp_media_audio, 4000, sdp_proto_rtpavp);
    sdp_media_set_ldir(m, SDP_SENDRECV);
    sdp_format_add(NULL, m, false, "8", "PCMA", 8000, 1, NULL, NULL, NULL, false, NULL);
    sdp_media_set_lattr(m, false, "ptime", "%u", PTIME);
    sdp_encode(descp, sess, true);
    mem_deref(sess);
    return 0;
}

static int answer_handler(const struct sip_msg *msg, void *arg) {
    (void)msg; (void)arg;
    return 0;
}

static void estab_handler(const struct sip_msg *msg, void *arg) {
    (void)arg;
    struct sa dummy;
    sa_init(&dummy, AF_INET);
    struct sdp_session *sess = NULL;
    if (sdp_session_alloc(&sess, &dummy)) {
        bind_rtp_and_audio();
        return;
    }
    if (!sdp_decode(sess, msg->mb, false)) {
        struct le *le = list_head(sdp_session_medial(sess, false));
        if (le) {
            struct sdp_media *rm = le->data;
            sa_cpy(&peer_rtp, sdp_media_raddr(rm));
            sa_set_port(&peer_rtp, sdp_media_rport(rm));
            const struct sdp_format *rfmt = sdp_media_rformat(rm, "PCMA");
            if (rfmt) pt = rfmt->pt;
            re_fprintf(stderr, "Peer RTP: %J pt=%u\n", &peer_rtp, pt);
        }
    }
    mem_deref(sess);
    bind_rtp_and_audio();
}

static void close_handler(int err, const struct sip_msg *msg, void *arg) {
    (void)msg; (void)arg;
    if (err) re_fprintf(stderr, "Call closed: %m\n", err);
    else     re_fprintf(stderr, "Call ended\n");
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

int main(int argc, char *argv[]) {
    rtp_ssrc = rand_u16() | ((uint32_t)rand_u16() << 16);

    if (libre_init()) return 1;
    poll_method_set(METHOD_SELECT);

    if (argc >= 2 && wav_open(argv[1])) return 1;
    if (argc >= 3 && wav_out_open(argv[2])) return 1;

    struct sa laddr;
    sa_set_str(&laddr, LOCAL_IP, 5060);
    if (sip_alloc(&sip, NULL, 32, 32, 32, "rtp_bridge", NULL, NULL) ||
        sip_transp_add(sip, SIP_TRANSP_UDP, &laddr, NULL)) {
        re_fprintf(stderr, "SIP init failed\n");
        goto out;
    }

    if (sipsess_listen(&sock, sip, 32, NULL, NULL)) {
        re_fprintf(stderr, "sipsess_listen failed\n");
        goto out;
    }

    re_fprintf(stderr, "Dialing %s ...\n", PEER_URI);

    int err = sipsess_connect(&sess, sock,
                              PEER_URI,
                              NULL,
                              LOCAL_SIP,
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

    signal(SIGINT, signal_handler);
    re_main(signal_handler);

out:
    call_active = false;
    tmr_cancel(&wav_tmr);
    wav_out_close();
    if (wav_fp) fclose(wav_fp);
    stdin_watcher = fd_close(stdin_watcher);
    rtp_socket = mem_deref(rtp_socket);
    sess = mem_deref(sess);
    sock = mem_deref(sock);
    sip = mem_deref(sip);
    libre_close();
    return 0;
}
