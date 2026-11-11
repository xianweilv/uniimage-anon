#include "hd_synth.h"
#include "hd_tokdec.h"
#include "hd_tokenc.h"
#include <zlib.h>
#include <openssl/sha.h>

void hd_synth_cfg_default(hd_synth_cfg *c) {
    c->beta_tokens = 16384;
    c->threads = 1;
}

int hd_synth_refsonly_flag = 0;

typedef struct par_ctl par_ctl;

typedef struct {
    hd_store *st;
    const hd_synth_cfg *cfg;
    hd_synth_result *res;
    int refsonly;

    hd_token *tok;
    size_t ntok, cap;

    hd_bw bw;
    uint32_t crc;
    uint64_t isize;

    int       loaded_idx;
    int       run_obj_idx;
    uint64_t  run_off0;
    hd_tokstream obj_ts;
    uint8_t  *obj_raw;
    size_t    obj_raw_len;
    int       obj_has_raw;
    size_t    walk_k;
    uint64_t  walk_pos;

    uint8_t **comp_cache;
    size_t   *comp_len;
    int       n_comp;

    uint8_t zeros[65536];

    par_ctl *par;
    size_t   chunk_toks;
} synth_t;

static void hd_free_cache(synth_t *s) {
    if (s->comp_cache) {
        for (int i = 0; i < s->n_comp; i++) free(s->comp_cache[i]);
        free(s->comp_cache);
        free(s->comp_len);
        s->comp_cache = NULL;
        s->comp_len = NULL;
    }
}

static void flush_blocks(synth_t *s, int final) {
    uint32_t beta = s->cfg->beta_tokens;
    size_t i = 0;
    while (i < s->ntok) {
        size_t n = s->ntok - i;
        if (!final && n < beta) break;
        if (n > beta) n = beta;
        int is_final = final && (i + n >= s->ntok);
        double t0 = hd_now_ms();
        hd_encode_block(&s->bw, s->tok + i, n, is_final);
        s->res->t_encode_ms += hd_now_ms() - t0;
        i += n;
    }
    memmove(s->tok, s->tok + i, (s->ntok - i) * sizeof(hd_token));
    s->ntok -= i;
}

#include <pthread.h>

#define PAR_SLOTS 8
typedef struct par_slot {
    hd_token *toks; size_t n;
    uint8_t *out; size_t out_len; uint8_t out_bits; int out_bitcnt;
    long seq; int state;   /* 0 free 1 queued 2 done 3 failed */
} par_slot;

typedef struct par_ctl {
    par_slot slots[PAR_SLOTS];
    int next_seq, done_seq, failed;
    pthread_mutex_t mu;
    pthread_cond_t cv_free, cv_done;
    int nthr, shutdown;
    pthread_t th[15];
    uint32_t beta;
    double enc_ms;
} par_ctl;

static void par_encode_chunk(par_ctl *p, par_slot *sl) {
    static int par_check = -1;
    if (par_check < 0) par_check = getenv("PAR_CHECK") ? 1 : 0;
    for (size_t i = 0; par_check && i < sl->n; i++) {
        uint32_t v = sl->toks[i].v, d = sl->toks[i].d;
        int bad = (d == 0 && v > 255) || (d != 0 && (v < 3 || v > 258)) || d > 32768;
        if (bad) {
            fprintf(stderr, "CHUNK seq=%ld badtok i=%zu v=%u d=%u n=%zu\n",
                    sl->seq, i, v, d, sl->n);
            sl->out = NULL;
            sl->state = 4;
            return;
        }
    }
    size_t cap = sl->n + sl->n / 8 + (1 << 20);
    uint8_t *buf = malloc(cap);
    hd_bw w;
    hd_bw_init(&w, buf, cap);
    double t0 = hd_now_ms();
    size_t i = 0;
    while (i < sl->n) {
        size_t n = sl->n - i;
        if (n > p->beta) n = p->beta;
        hd_encode_block(&w, sl->toks + i, n, 0);
        i += n;
    }
    p->enc_ms += hd_now_ms() - t0;
    sl->out = buf;
    sl->out_len = w.len;
    sl->out_bits = (uint8_t)(w.bitbuf & 0xFF);
    sl->out_bitcnt = w.bitcnt;
}

static void *par_worker(void *arg) {
    par_ctl *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (!p->shutdown) {
            int found = -1;
            for (int i = 0; i < PAR_SLOTS; i++)
                if (p->slots[i].state == 1) { found = i; break; }
            if (found >= 0) { p->slots[found].state = 3; pthread_mutex_unlock(&p->mu);
                par_encode_chunk(p, &p->slots[found]);
                pthread_mutex_lock(&p->mu);
                p->slots[found].state = 2;
                pthread_cond_broadcast(&p->cv_done);
                continue;
            }
            pthread_cond_wait(&p->cv_free, &p->mu);
        }
        pthread_mutex_unlock(&p->mu);
        return NULL;
    }
}

static void par_start(par_ctl *p, int threads, uint32_t beta) {
    hd_tokenc_init();
    memset(p, 0, sizeof *p);
    p->beta = beta;
    p->nthr = threads - 1;
    if (p->nthr > 15) p->nthr = 15;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv_free, NULL);
    pthread_cond_init(&p->cv_done, NULL);
    for (int i = 0; i < p->nthr; i++)
        pthread_create(&p->th[i], NULL, par_worker, p);
}

static void par_collect(par_ctl *p, synth_t *s);

static void par_dispatch(par_ctl *p, synth_t *s, const hd_token *toks, size_t n) {
    pthread_mutex_lock(&p->mu);
    for (;;) {
        if (p->failed) { pthread_mutex_unlock(&p->mu); hd_die("par encode failed"); }
        int free_i = -1;
        for (int i = 0; i < PAR_SLOTS; i++)
            if (p->slots[i].state == 0) { free_i = i; break; }
        if (free_i >= 0) {
            par_slot *sl = &p->slots[free_i];
            sl->toks = malloc(n * sizeof(hd_token));
            memcpy(sl->toks, toks, n * sizeof(hd_token));
            sl->n = n;
            sl->seq = p->next_seq++;
            sl->state = 1;
            pthread_cond_broadcast(&p->cv_free);
            pthread_mutex_unlock(&p->mu);
            return;
        }
        pthread_mutex_unlock(&p->mu);
        par_collect(p, s);
        pthread_mutex_lock(&p->mu);
    }
}

static void par_collect(par_ctl *p, synth_t *s) {
    for (;;) {
        pthread_mutex_lock(&p->mu);
        int hit = -1;
        if (p->done_seq < p->next_seq)
            for (int i = 0; i < PAR_SLOTS; i++)
                if (p->slots[i].state == 2 && p->slots[i].seq == p->done_seq)
                    { hit = i; break; }
        if (hit < 0) { pthread_mutex_unlock(&p->mu); return; }
        par_slot *sl = &p->slots[hit];
        uint8_t *buf = sl->out; size_t len = sl->out_len;
        uint8_t bits = sl->out_bits; int bitcnt = sl->out_bitcnt;
        int failed = p->failed;
        pthread_mutex_unlock(&p->mu);
        if (!failed) {
            if (s->bw.bitcnt == 0) {
                if (s->bw.len + len + 8 > s->bw.cap) failed = 1;
                else {
                    memcpy(s->bw.out + s->bw.len, buf, len);
                    s->bw.len += len;
                }
            } else {
                int sh = s->bw.bitcnt;
                uint64_t acc = s->bw.bitbuf & ((1u << sh) - 1);
                if (s->bw.len + len + 8 > s->bw.cap) failed = 1;
                else {
                    for (size_t i = 0; i < len; i++) {
                        acc |= (uint64_t)buf[i] << sh;
                        s->bw.out[s->bw.len++] = (uint8_t)(acc & 0xFF);
                        acc >>= 8;
                    }
                    s->bw.bitbuf = acc;
                }
            }
            if (!failed && bitcnt) hd_bw_put(&s->bw, bits, bitcnt);
        }
        if (getenv("PAR_DEBUG"))
            fprintf(stderr, "COLLECT seq=%ld len=%zu bitcnt=%d -> "
                    "dstlen=%zu dstbitcnt=%d\n",
                    sl->seq, len, bitcnt, s->bw.len, s->bw.bitcnt);
        free(buf); free(sl->toks);
        pthread_mutex_lock(&p->mu);
        sl->state = 0; sl->toks = NULL; sl->out = NULL;
        p->done_seq++;
        if (failed) p->failed = 1;
        pthread_cond_broadcast(&p->cv_free);
        pthread_mutex_unlock(&p->mu);
        if (failed) hd_die("par append overflow");
    }
}

static void par_drain(par_ctl *p, synth_t *s) {
    pthread_mutex_lock(&p->mu);
    while (p->done_seq < p->next_seq) {
        int hit = -1;
        for (int i = 0; i < PAR_SLOTS; i++)
            if (p->slots[i].state == 2 && p->slots[i].seq == p->done_seq)
                { hit = i; break; }
        if (hit >= 0) { pthread_mutex_unlock(&p->mu); par_collect(p, s);
            pthread_mutex_lock(&p->mu); continue; }
        pthread_cond_wait(&p->cv_done, &p->mu);
    }
    pthread_mutex_unlock(&p->mu);
}

static void par_stop(par_ctl *p, hd_synth_result *res) {
    pthread_mutex_lock(&p->mu);
    p->shutdown = 1;
    pthread_cond_broadcast(&p->cv_free);
    pthread_mutex_unlock(&p->mu);
    for (int i = 0; i < p->nthr; i++) pthread_join(p->th[i], NULL);
    res->t_encode_ms += p->enc_ms;
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv_free);
    pthread_cond_destroy(&p->cv_done);
}

static void buf_full(synth_t *s) {
    if (s->par) {
        uint32_t beta = s->cfg->beta_tokens;
        if (s->ntok >= beta) {
            size_t cut = (s->ntok / beta) * beta;
            par_dispatch(s->par, s, s->tok, cut);
            memmove(s->tok, s->tok + cut,
                    (s->ntok - cut) * sizeof(hd_token));
            s->ntok -= cut;
        }
        return;
    }
    flush_blocks(s, 0);
}

static void push_tok(synth_t *s, uint32_t v, uint32_t d) {
    if (s->ntok == s->cap) {
        buf_full(s);
        if (s->ntok == s->cap) {
            s->cap *= 2;
            s->tok = realloc(s->tok, s->cap * sizeof(hd_token));
            if (!s->tok) hd_die("OOM synth tokens");
        }
    }
    s->tok[s->ntok].v = v;
    s->tok[s->ntok].d = d;
    s->ntok++;
    s->res->n_tokens++;
}

static void push_toks(synth_t *s, const hd_token *t, size_t n) {
    while (n) {
        if (s->ntok + n > s->cap) {
            buf_full(s);
            if (s->ntok + n > s->cap) {
                size_t want = s->ntok + n;
                size_t nc = s->cap * 2;
                if (nc < want) nc = want;
                s->tok = realloc(s->tok, nc * sizeof(hd_token));
                if (!s->tok) hd_die("OOM synth tokens");
                s->cap = nc;
            }
        }
        memcpy(s->tok + s->ntok, t, n * sizeof(hd_token));
        s->ntok += n;
        s->res->n_tokens += n;
        t += n;
        n = 0;
    }
}

static void push_lits(synth_t *s, const uint8_t *p, size_t n) {
    while (n) {
        if (s->ntok == s->cap) {
            buf_full(s);
            if (s->ntok == s->cap) {
                s->cap *= 2;
                s->tok = realloc(s->tok, s->cap * sizeof(hd_token));
                if (!s->tok) hd_die("OOM synth tokens");
            }
        }
        size_t room = s->cap - s->ntok;
        size_t take = n < room ? n : room;
        hd_token *dst = s->tok + s->ntok;
        for (size_t i = 0; i < take; i++) {
            dst[i].v = p[i];
            dst[i].d = 0;
        }
        s->ntok += take;
        s->res->n_tokens += take;
        p += take;
        n -= take;
    }
}

static int load_obj(synth_t *s, const hd_objmeta *o, int idx) {
    double tl0 = hd_now_ms();
    uint8_t *comp;
    size_t clen;
    if (s->comp_cache && s->comp_cache[idx]) {
        comp = s->comp_cache[idx];
        clen = s->comp_len[idx];
    } else {
        comp = hd_store_get_object(s->st, o->sha, &clen);
        if (!comp) return -1;
    }
    double t0 = hd_now_ms();
    hd_ts_reset(&s->obj_ts);
    int rc = hd_tokens_from_deflate(comp, clen, &s->obj_ts, NULL);
    s->res->t_decode_ms += hd_now_ms() - t0;
    if (!(s->comp_cache && s->comp_cache[idx])) free(comp);
    if (rc) return -2;
    free(s->obj_raw);
    s->obj_raw = NULL;
    s->obj_raw_len = o->raw_len;
    s->obj_has_raw = 0;
    s->walk_k = 0;
    s->walk_pos = 0;
    s->res->t_load_ms += hd_now_ms() - tl0;
    return 0;
}

static int ensure_raw(synth_t *s, const hd_objmeta *o, int idx) {
    if (s->obj_has_raw) return 0;
    uint8_t *comp;
    size_t clen;
    if (s->comp_cache && s->comp_cache[idx]) {
        comp = s->comp_cache[idx];
        clen = s->comp_len[idx];
    } else {
        comp = hd_store_get_object(s->st, o->sha, &clen);
        if (!comp) return -1;
    }
    s->obj_raw = malloc(o->raw_len ? o->raw_len : 1);
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    inflateInit2(&zs, -15);
    zs.next_in = comp;
    zs.avail_in = (uInt)clen;
    zs.next_out = s->obj_raw;
    zs.avail_out = (uInt)o->raw_len;
    int irc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (!(s->comp_cache && s->comp_cache[idx])) free(comp);
    if (irc != Z_STREAM_END) return -2;
    s->obj_has_raw = 1;
    return 0;
}

static size_t synth_pass(synth_t *s, const hd_recipe *rec,
                         uint8_t *out, size_t outcap) {
    static const uint8_t hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255};
    hd_bw_init(&s->bw, out, outcap);
    for (int k = 0; k < 10; k++) hd_bw_put(&s->bw, hdr[k], 8);

    s->crc = crc32(0L, Z_NULL, 0);
    s->isize = 0;
    s->ntok = 0;
    s->loaded_idx = -1;
    s->run_obj_idx = -1;
    s->res->n_tokens = 0;
    s->res->tokens_from_obj = 0;
    s->res->tokens_literalized = 0;
    s->res->t_decode_ms = s->res->t_encode_ms = s->res->t_misc_ms = 0;
    s->par = NULL;
    if (s->cfg->threads > 1) {
        s->par = malloc(sizeof(par_ctl));
        par_start(s->par, s->cfg->threads, s->cfg->beta_tokens);
        s->chunk_toks = 128ull * s->cfg->beta_tokens;
    }

    double tloop0 = hd_now_ms();
    for (int i = 0; i < rec->n_segs; i++) {
        const hd_seg *seg = &rec->segs[i];
        if (seg->type == HD_SEG_META) {
            if (s->refsonly) continue;
            const uint8_t *p = rec->bytes + seg->b;
            uint32_t n = seg->a;
            push_lits(s, p, n);
            s->crc = crc32(s->crc, p, n);
            s->isize += n;
            s->run_obj_idx = -1;
        } else if (seg->type == HD_SEG_PAD) {
            s->run_obj_idx = -1;
            if (s->refsonly) continue;
            uint32_t z = seg->a;
            if (z) {
                push_tok(s, 0, 0);
                uint32_t rem = z - 1;
                while (rem >= 3) {
                    uint32_t l = rem > 258 ? 258 : rem;
                    push_tok(s, l, 1);
                    rem -= l;
                }
                while (rem > 0) {
                    push_tok(s, 0, 0);
                    rem--;
                }
                double t0 = hd_now_ms();
                while (z > 0) {
                    uint32_t take = z > (uint32_t)sizeof s->zeros ? (uint32_t)sizeof s->zeros : z;
                    s->crc = crc32(s->crc, s->zeros, take);
                    z -= take;
                }
                s->res->t_misc_ms += hd_now_ms() - t0;
                s->isize += seg->a;
            }
        } else {
            int idx = (int)seg->a;
            uint32_t off = seg->b, len = seg->c;
            if (idx < 0 || idx >= rec->n_objs) return 0;
            const hd_objmeta *o = &rec->objs[idx];
            if (len == 0) continue;
            int full = (off == 0 && len == o->raw_len);

            if (full) {
                if (s->loaded_idx != idx) {
                    if (load_obj(s, o, idx)) return 0;
                    s->loaded_idx = idx;
                }
                if (s->run_obj_idx != idx) {
                    s->run_obj_idx = idx;
                    s->run_off0 = 0;
                }
                double t0 = hd_now_ms();
                push_toks(s, s->obj_ts.tok, s->obj_ts.ntok);
                s->res->tokens_from_obj += s->obj_ts.ntok;
                s->res->t_misc_ms += hd_now_ms() - t0;
                s->crc = crc32_combine(s->crc, o->crc, (z_off_t)o->raw_len);
                s->isize += o->raw_len;
            } else {
                if (s->loaded_idx != idx) {
                    if (load_obj(s, o, idx)) return 0;
                    s->loaded_idx = idx;
                }
                if (ensure_raw(s, o, idx)) return 0;
                if (s->run_obj_idx != idx) {
                    s->run_obj_idx = idx;
                    s->run_off0 = off;
                } else if (off < s->run_off0) {
                    s->run_off0 = off;
                }
                if (off < s->walk_pos) {
                    s->walk_k = 0;
                    s->walk_pos = 0;
                }
                double t0 = hd_now_ms();
                uint64_t end = (uint64_t)off + len;
                while (s->walk_k < s->obj_ts.ntok && s->walk_pos < end) {
                    hd_token t = s->obj_ts.tok[s->walk_k];
                    if (t.d == 0) {
                        if (s->walk_pos >= off) push_tok(s, t.v, 0);
                        s->walk_pos += 1;
                        s->walk_k++;
                    } else {
                        uint64_t tl = t.v;
                        uint64_t src_ok_from = s->run_off0;
                        uint64_t tstart = s->walk_pos > off ? s->walk_pos : off;
                        uint64_t tend = s->walk_pos + tl < end ? s->walk_pos + tl : end;
                        if (tend > tstart) {
                            int keep = (tstart == s->walk_pos &&
                                        tend == s->walk_pos + tl &&
                                        s->walk_pos >= t.d &&
                                        s->walk_pos - t.d >= src_ok_from);
                            if (keep) {
                                push_tok(s, t.v, t.d);
                            } else {
                                for (uint64_t b = tstart; b < tend; b++) {
                                    push_tok(s, s->obj_raw[b], 0);
                                    s->res->tokens_literalized++;
                                }
                            }
                        }
                        s->walk_pos += tl;
                        s->walk_k++;
                    }
                }
                s->res->t_misc_ms += hd_now_ms() - t0;
                s->crc = crc32(s->crc, s->obj_raw + off, len);
                s->isize += len;
            }
        }
    }

    s->res->t_loop_ms = hd_now_ms() - tloop0;
    if (s->par) {
        uint32_t beta = s->cfg->beta_tokens;
        if (s->ntok >= beta) {
            size_t cut = (s->ntok / beta) * beta;
            par_dispatch(s->par, s, s->tok, cut);
            memmove(s->tok, s->tok + cut,
                    (s->ntok - cut) * sizeof(hd_token));
            s->ntok -= cut;
        }
        par_drain(s->par, s);
        par_stop(s->par, s->res);
        free(s->par);
        s->par = NULL;
    }
    flush_blocks(s, 1);
    hd_bw_align(&s->bw);

    /* trailer */
    uint32_t crcv = s->crc;
    uint32_t isz = (uint32_t)(s->isize & 0xFFFFFFFF);
    for (int k = 0; k < 4; k++) hd_bw_put(&s->bw, (uint32_t)((crcv >> (8 * k)) & 0xFF), 8);
    for (int k = 0; k < 4; k++) hd_bw_put(&s->bw, (uint32_t)((isz >> (8 * k)) & 0xFF), 8);
    return s->bw.len;
}

int hd_synth_layer(hd_store *st, const hd_synth_cfg *cfg,
                   const hd_recipe *rec,
                   hd_buf *out_buf, hd_synth_result *res) {
    hd_synth_cfg cfgd;
    if (!cfg) { hd_synth_cfg_default(&cfgd); cfg = &cfgd; }
    memset(res, 0, sizeof *res);

    synth_t s;
    memset(&s, 0, sizeof s);
    s.st = st;
    s.cfg = cfg;
    s.res = res;
    s.refsonly = hd_synth_refsonly_flag;
    s.cap = (size_t)cfg->beta_tokens * 2 + 4096;
    s.tok = malloc(s.cap * sizeof(hd_token));
    if (!s.tok) hd_die("OOM synth");
    memset(s.zeros, 0, sizeof s.zeros);
    hd_ts_init(&s.obj_ts);

    s.n_comp = rec->n_objs;
    if (rec->n_objs) {
        s.comp_cache = calloc(rec->n_objs, sizeof(uint8_t *));
        s.comp_len = calloc(rec->n_objs, sizeof(size_t));
        if (!s.comp_cache || !s.comp_len) hd_die("OOM comp cache");
        for (int i = 0; i < rec->n_objs; i++) {
            s.comp_cache[i] = hd_store_get_object(st, rec->objs[i].sha, &s.comp_len[i]);
            if (!s.comp_cache[i]) hd_die("missing object %.16s", rec->objs[i].sha);
        }
    }

    double t_total0 = hd_now_ms();

    uint64_t tar_exact = 0;
    for (int i = 0; i < rec->n_segs; i++) {
        const hd_seg *seg = &rec->segs[i];
        tar_exact += seg->type == HD_SEG_REF ? seg->c : seg->a;
    }
    if (!rec->tar_size) ((hd_recipe *)rec)->tar_size = tar_exact;

    size_t est = (size_t)tar_exact + (size_t)(tar_exact >> 3) + (1 << 20);
    double tres0 = hd_now_ms();
    out_buf->len = 0;
    hd_buf_reserve(out_buf, est + 64);
    res->t_reserve_ms = hd_now_ms() - tres0;
    double tp0 = hd_now_ms();
    size_t need = synth_pass(&s, rec, out_buf->data, out_buf->cap);
    res->t_pass1_ms = hd_now_ms() - tp0;
    if (need == 0) { free(s.tok); hd_ts_free(&s.obj_ts); free(s.obj_raw); hd_free_cache(&s); return -1; }
    if (need > out_buf->cap) {
        hd_buf_reserve(out_buf, need + 64);
        out_buf->len = 0;
        need = synth_pass(&s, rec, out_buf->data, out_buf->cap);
        if (need == 0 || need > out_buf->cap) {
            free(s.tok); hd_ts_free(&s.obj_ts); free(s.obj_raw); hd_free_cache(&s);
            return -2;
        }
    }
    out_buf->len = need;

    res->out_comp_size = need;
    res->tar_size = s.isize;
    res->crc = s.crc;
    res->t_total_ms = hd_now_ms() - t_total0;

    free(s.tok);
    hd_ts_free(&s.obj_ts);
    free(s.obj_raw);
    if (s.comp_cache) {
        for (int i = 0; i < s.n_comp; i++) free(s.comp_cache[i]);
        free(s.comp_cache);
        free(s.comp_len);
    }
    return 0;
}

char *hd_synth_verify(hd_store *st, const hd_recipe *rec,
                      const uint8_t *gz, size_t gzlen) {
    (void)st; (void)rec;
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return NULL;
    SHA256_CTX sha;
    SHA256_Init(&sha);
    uint8_t *win = malloc(1 << 20);
    size_t fed = 0;
    int rc = Z_OK;
    int ok = 0;
    for (;;) {
        if (zs.avail_in == 0 && fed < gzlen) {
            size_t chunk = gzlen - fed;
            if (chunk > (size_t)1 << 30) chunk = (size_t)1 << 30;
            zs.next_in = (Bytef *)(gz + fed);
            zs.avail_in = (uInt)chunk;
            fed += chunk;
        }
        zs.next_out = win;
        zs.avail_out = 1 << 20;
        rc = inflate(&zs, Z_NO_FLUSH);
        size_t got = (1 << 20) - zs.avail_out;
        if (got) SHA256_Update(&sha, win, got);
        if (rc == Z_STREAM_END) {
            ok = 1;
            break;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) break;
        if (rc == Z_BUF_ERROR && zs.avail_in == 0 && fed >= gzlen) break;
    }
    inflateEnd(&zs);
    free(win);
    if (!ok) return NULL;
    uint8_t dg[32];
    SHA256_Final(dg, &sha);
    char *hex = malloc(65);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i * 2] = hx[dg[i] >> 4];
        hex[i * 2 + 1] = hx[dg[i] & 15];
    }
    hex[64] = 0;
    return hex;
}
