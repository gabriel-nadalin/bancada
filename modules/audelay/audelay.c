#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>

#define PI 3.14159265358979323846

struct audec_st {
    struct aufilt_dec_st af;
    uint32_t srate;
    uint8_t  ch;
    int      freq;
    unsigned burst_ms;
    unsigned period_ms;
    double   coeff;
    double   norm_thresh;
    uint64_t prev_detected;
};

static void destructor(void *arg) {
    struct audec_st *st = arg;
    list_unlink(&st->af.le);
}

static RE_ATOMIC uint64_t last_tx_time;

static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg) {
    (void)arg;

    if (ev != BEVENT_MODULE) return;
    unsigned long long ts;
    if (1 == sscanf(bevent_get_text(event), "auburst,tx,%llu", &ts))
        re_atomic_rlx_set(&last_tx_time, (uint64_t)ts);
}

static int decode_update(struct aufilt_dec_st **stp, void **ctx,
                         const struct aufilt *af, struct aufilt_prm *prm,
                         const struct audio *au) {
    (void)ctx;
    (void)au;

    if (!stp || !af || !prm) return EINVAL;
    if (*stp) return 0;

    struct audec_st *st = mem_zalloc(sizeof(*st), destructor);
    if (!st) return ENOMEM;

    st->af.af = af;
    st->srate = prm->srate;
    st->ch    = prm->ch;

    struct config *cfg = conf_config();
    sscanf(cfg->audio.src_dev, "%d,%u,%u",
           &st->freq, &st->burst_ms, &st->period_ms);
    if (!st->freq)        st->freq = 1000;
    if (!st->burst_ms)    st->burst_ms = 50;
    if (!st->period_ms) st->period_ms = st->burst_ms * 2;

    st->coeff = 2.0 * cos(2.0 * PI * st->freq / st->srate);
    st->norm_thresh = 0.25;

    *stp = (struct aufilt_dec_st *)st;
    return 0;
}

static int decode(struct aufilt_dec_st *st, struct auframe *af) {
    struct audec_st *au = (struct audec_st *)st;

    size_t nf = af->sampc / af->ch;
    const int16_t *sampv = af->sampv;

    double s1 = 0, s2 = 0;
    int64_t sum_sq = 0;

    for (size_t i = 0; i < nf; i++) {
        double x = sampv[i * au->ch];
        double s0 = x + au->coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
        sum_sq += (int64_t)(x * x);
    }

    double power = s2 * s2 + s1 * s1 - au->coeff * s1 * s2;
    if (power < 0)
        power = 0;

    double norm = 0.0;
    if (sum_sq > 0 && nf > 0) {
        double denom = (double)sum_sq * nf;
        if (denom > 0)
            norm = (2.0 * power) / denom;
    }

    if (norm > au->norm_thresh) {
        uint64_t tx = re_atomic_rlx(&last_tx_time);
        if (tx && tx != au->prev_detected) {
            au->prev_detected = tx;
            int64_t rtt = (int64_t)(tmr_jiffies() - tx);
            info("audelay: RTT=%lld ms goertzel=%.2f power=%.0f\n",
                 rtt, norm, power);
        }
    }

    return 0;
}

static struct aufilt audelay = {
    .name    = "audelay",
    .decupdh = decode_update,
    .dech    = decode,
};

static int module_init(void) {
    bevent_register(event_handler, NULL);
    aufilt_register(baresip_aufiltl(), &audelay);
    return 0;
}

static int module_close(void) {
    bevent_unregister(event_handler);
    aufilt_unregister(&audelay);
    return 0;
}

EXPORT_SYM const struct mod_export DECL_EXPORTS(audelay) = {
    "audelay", "filter", module_init, module_close
};
