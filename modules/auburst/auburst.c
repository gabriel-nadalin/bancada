#include <re_atomic.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#define AMPLITUDE 0.5f
#define PI 3.14159265358979323846

/* --- private state --- */
struct ausrc_st {
    uint32_t ptime;
    size_t sampc;
    RE_ATOMIC bool run;
    thrd_t thread;
    ausrc_read_h *rh;
    ausrc_error_h *errh;
    void *arg;
    struct ausrc_prm prm;
    int freq;
    unsigned burst_ms;
    unsigned period_ms;
    uint64_t frame;
    FILE *wav;
};

/* --- destructor --- */
static void wav_close(struct ausrc_st *st) {
    if (!st->wav) return;
    uint32_t data_sz = (uint32_t)ftell(st->wav) - 44;
    fseek(st->wav, 4, SEEK_SET);
    uint32_t riff_sz = 36 + data_sz;
    fwrite(&riff_sz, 4, 1, st->wav);
    fseek(st->wav, 40, SEEK_SET);
    fwrite(&data_sz, 4, 1, st->wav);
    fclose(st->wav);
    st->wav = NULL;
}

static void wav_init(struct ausrc_st *st, uint32_t srate, uint8_t ch) {
    st->wav = fopen("/tmp/auburst_tx.wav", "wb");
    if (!st->wav) return;
    uint16_t fmt = 1, nch = ch, bps = 16;
    uint32_t sr = srate, br = sr * nch * bps / 8;
    uint16_t blk = nch * bps / 8;
    fwrite("RIFF", 4, 1, st->wav);
    uint32_t tmp = 0;
    fwrite(&tmp, 4, 1, st->wav);
    fwrite("WAVE", 4, 1, st->wav);
    fwrite("fmt ", 4, 1, st->wav);
    tmp = 16; fwrite(&tmp, 4, 1, st->wav);
    fwrite(&fmt, 2, 1, st->wav);
    fwrite(&nch, 2, 1, st->wav);
    fwrite(&sr, 4, 1, st->wav);
    fwrite(&br, 4, 1, st->wav);
    fwrite(&blk, 2, 1, st->wav);
    fwrite(&bps, 2, 1, st->wav);
    fwrite("data", 4, 1, st->wav);
    fwrite(&tmp, 4, 1, st->wav);
}

static void destructor(void *arg) {
    struct ausrc_st *st = arg;
    if (re_atomic_rlx(&st->run)) {
        re_atomic_rlx_set(&st->run, false);
        thrd_join(st->thread, NULL);
    }
    wav_close(st);
}

/* --- thread: generates bursts of sine + silence --- */
static int play_thread(void *arg) {
    struct ausrc_st *st = arg;
    int16_t *sampv = mem_alloc(st->sampc * sizeof(int16_t), NULL);
    if (!sampv) return ENOMEM;
    uint64_t ts = tmr_jiffies();
    while (re_atomic_rlx(&st->run)) {
        struct auframe af;
        auframe_init(&af, AUFMT_S16LE, sampv, st->sampc,
                     st->prm.srate, st->prm.ch);
        sys_msleep(4);
        uint64_t now = tmr_jiffies();
        if (ts > now)
            continue;
        uint64_t period = (uint64_t)st->prm.srate * st->period_ms / 1000;
        uint64_t burst  = (uint64_t)st->prm.srate * st->burst_ms / 1000;
        for (size_t i = 0; i < st->sampc; i++) {
            uint64_t t = st->frame + i / st->prm.ch;
            int val = 0;
            if (t % period == 0)
                bevent_app_emit(BEVENT_MODULE, NULL, "auburst,tx,%llu", tmr_jiffies());
            if (t % period < burst)
                val = (int16_t)(sin(2.0 * PI * st->freq * t / st->prm.srate) * 32767 * AMPLITUDE);
            sampv[i] = val;
        }
        st->frame += st->sampc / st->prm.ch;
        if (st->wav)
            fwrite(sampv, sizeof(int16_t), st->sampc, st->wav);
        st->rh(&af, st->arg);
        ts += st->ptime;
    }
    mem_deref(sampv);
    return 0;
}

/* --- alloc: called when baresip selects this source --- */
static int alloc_handler(struct ausrc_st **stp, const struct ausrc *as,
                         struct ausrc_prm *prm, const char *dev,
                         ausrc_read_h *rh, ausrc_error_h *errh, void *arg) {
    if (!stp || !as || !prm || !rh || !dev) return EINVAL;
    struct ausrc_st *st = mem_zalloc(sizeof(*st), destructor);
    if (!st) return ENOMEM;
    st->rh = rh; st->errh = errh; st->arg = arg;
    st->prm = *prm;
    sscanf(dev, "%d,%u,%u", &st->freq, &st->burst_ms, &st->period_ms);
    if (!st->freq) st->freq = 1000;
    if (!st->burst_ms) st->burst_ms = 50;
    if (!st->period_ms) st->period_ms = st->burst_ms * 2;
    st->ptime = prm->ptime ? prm->ptime : 10;
    st->sampc = prm->srate * prm->ch * st->ptime / 1000;
    wav_init(st, prm->srate, prm->ch);
    re_atomic_rlx_set(&st->run, true);
    int err = thread_create_name(&st->thread, "auburst", play_thread, st);
    if (err) { re_atomic_rlx_set(&st->run, false); mem_deref(st); return err; }
    *stp = st;
    info("auburst: freq=%d burst=%ums period=%ums\n",
        st->freq, st->burst_ms, st->period_ms);
    info("auburst: srate=%u ch=%u ptime=%u sampc=%zu\n",
        prm->srate, prm->ch, st->ptime, st->sampc);
    return 0;
}

/* --- module glue --- */
static struct ausrc *ausrc;
static int module_init(void) {
    return ausrc_register(&ausrc, baresip_ausrcl(), "auburst", alloc_handler);
}

static int module_close(void) {
    ausrc = mem_deref(ausrc);
    return 0;
}

EXPORT_SYM const struct mod_export DECL_EXPORTS(auburst) = {
    "auburst", "ausrc", module_init, module_close
};