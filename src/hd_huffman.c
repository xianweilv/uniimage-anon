#include "hd_huffman.h"

/* ---------- Decode table construction ---------- */
int hd_huffdec_build2(hd_huffdec *h, const uint8_t *lens, int n, int allow_incomplete) {
    int count[HD_MAX_BITS + 1] = {0};
    int maxlen = 0, used = 0;
    for (int i = 0; i < n; i++) {
        int l = lens[i];
        if (l < 0 || l > HD_MAX_BITS) return -1;
        if (l) { count[l]++; used++; if (l > maxlen) maxlen = l; }
    }
    if (used == 0) return -1;

    if (used == 1) {
        h->maxlen = h->nbits = 1;
        h->mask = 1;
        h->tab = malloc(2 * sizeof(uint32_t));
        if (!h->tab) hd_die("OOM huffdec");
        int sym = -1;
        for (int i = 0; i < n; i++) if (lens[i]) { sym = i; break; }
        h->tab[0] = ((uint32_t)sym << 16) | 1;
        h->tab[1] = ((uint32_t)sym << 16) | 1;
        return 0;
    }

    int64_t left = 1;
    for (int l = 1; l <= HD_MAX_BITS; l++) {
        left <<= 1;
        left -= count[l];
        if (left < 0) return -2;   /* oversubscribed */
    }
    if (left > 0 && !allow_incomplete) return -3;

    uint32_t next_code[HD_MAX_BITS + 1] = {0};
    uint32_t code = 0;
    for (int l = 1; l <= maxlen; l++) {
        code = (code + count[l - 1]) << 1;
        next_code[l] = code;
    }

    h->maxlen = h->nbits = maxlen;
    h->mask = (1u << maxlen) - 1;
    size_t tsize = (size_t)1 << maxlen;
    h->tab = malloc(tsize * sizeof(uint32_t));
    if (!h->tab) hd_die("OOM huffdec %d", maxlen);
    memset(h->tab, 0, tsize * sizeof(uint32_t));

    for (int i = 0; i < n; i++) {
        int l = lens[i];
        if (!l) continue;
        uint32_t c = next_code[l]++;
        uint32_t r = 0;
        for (int b = 0; b < l; b++) { r = (r << 1) | (c & 1); c >>= 1; }
        size_t step = (size_t)1 << l;
        uint32_t entry = ((uint32_t)i << 16) | (uint32_t)l;
        for (size_t j = r; j < tsize; j += step)
            h->tab[j] = entry;
    }
    return 0;
}

/* ---------- Encoding code table ---------- */
static uint32_t rev_bits(uint32_t c, int l) {
    uint32_t r = 0;
    for (int b = 0; b < l; b++) { r = (r << 1) | (c & 1); c >>= 1; }
    return r;
}

int hd_huffenc_build(hd_huffenc *e, const uint8_t *lens, int n) {
    int count[HD_MAX_BITS + 1] = {0};
    memset(e->code, 0, sizeof(e->code));
    memset(e->len, 0, sizeof(e->len));
    e->nsyms = n;
    for (int i = 0; i < n; i++) {
        int l = lens[i];
        e->len[i] = l;
        if (l) count[l]++;
    }
    uint32_t next_code[HD_MAX_BITS + 1] = {0};
    uint32_t code = 0;
    for (int l = 1; l <= HD_MAX_BITS; l++) {
        code = (code + count[l - 1]) << 1;
        next_code[l] = code;
    }
    int used = 0;
    for (int i = 0; i < n; i++) {
        int l = lens[i];
        if (!l) continue;
        e->code[i] = rev_bits(next_code[l]++, l);
        used++;
    }
    return used;
}

typedef struct pm_node {
    uint64_t w;
    int      is_leaf;
    int      sym;
    struct pm_node *a, *b;
} pm_node;

typedef struct {
    pm_node *v[640];
    int n;
} pm_list;

static int pm_le_first(const pm_node *leaf, const pm_node *pkg) {
    return leaf->w < pkg->w;
}

int hd_huff_lengths(const uint32_t *freq, int n, int maxbits, uint8_t *lens) {
    int syms[288];
    int m = 0;
    for (int i = 0; i < n; i++) {
        lens[i] = 0;
        if (freq[i]) syms[m++] = i;
    }
    if (m == 0) return 0;
    if (m == 1) { lens[syms[0]] = 1; return 1; }
    if (m == 2) { lens[syms[0]] = 1; lens[syms[1]] = 1; return 2; }
    if (maxbits < 2) maxbits = 2;

    for (int i = 1; i < m; i++) {
        int s = syms[i];
        int j = i - 1;
        while (j >= 0 && (freq[syms[j]] > freq[s] ||
               (freq[syms[j]] == freq[s] && syms[j] > s))) {
            syms[j + 1] = syms[j];
            j--;
        }
        syms[j + 1] = s;
    }

    static _Thread_local pm_node pool[16 * 1400];
    static _Thread_local pm_list lists[16];
    int pool_n = 0;

    static _Thread_local pm_node *leaf_at[16][288];
    for (int l = 1; l <= maxbits; l++) {
        for (int k = 0; k < m; k++) {
            if (pool_n >= (int)(sizeof pool / sizeof pool[0])) hd_die("pm pool");
            pm_node *nd = &pool[pool_n++];
            nd->w = freq[syms[k]];
            nd->is_leaf = 1;
            nd->sym = syms[k];
            nd->a = nd->b = NULL;
            leaf_at[l][k] = nd;
        }
    }
    lists[maxbits].n = m;
    for (int k = 0; k < m; k++) lists[maxbits].v[k] = leaf_at[maxbits][k];

    static _Thread_local pm_node *pkg_buf[640];
    for (int l = maxbits - 1; l >= 1; l--) {
        pm_list *deeper = &lists[l + 1];
        int npkg = 0;
        for (int k = 0; k + 1 < deeper->n; k += 2) {
            if (pool_n >= (int)(sizeof pool / sizeof pool[0])) hd_die("pm pool");
            pm_node *nd = &pool[pool_n++];
            nd->w = deeper->v[k]->w + deeper->v[k + 1]->w;
            nd->is_leaf = 0;
            nd->sym = -1;
            nd->a = deeper->v[k];
            nd->b = deeper->v[k + 1];
            pkg_buf[npkg++] = nd;
        }
        pm_list *cur = &lists[l];
        int i = 0, j = 0, t = 0;
        while (i < m && j < npkg) {
            if (pm_le_first(leaf_at[l][i], pkg_buf[j]))
                cur->v[t++] = leaf_at[l][i++];
            else
                cur->v[t++] = pkg_buf[j++];
        }
        while (i < m) cur->v[t++] = leaf_at[l][i++];
        while (j < npkg) cur->v[t++] = pkg_buf[j++];
        cur->n = t;
        if (t > 640) hd_die("pm merge overflow");
    }

    int take = 2 * m - 2;
    if (take > lists[1].n) take = lists[1].n;
    static _Thread_local pm_node *stack[16 * 1400];
    int sp = 0;
    for (int k = 0; k < take; k++) stack[sp++] = lists[1].v[k];
    while (sp > 0) {
        pm_node *nd = stack[--sp];
        if (nd->is_leaf) {
            lens[nd->sym]++;
        } else {
            stack[sp++] = nd->a;
            stack[sp++] = nd->b;
        }
    }

    int64_t K = 0;
    for (int i = 0; i < n; i++)
        if (lens[i]) {
            if (lens[i] > maxbits) hd_die("pm length overflow %d", lens[i]);
            K += (int64_t)1 << (maxbits - lens[i]);
        }
    if (K != ((int64_t)1 << maxbits)) hd_die("pm kraft=%lld != %lld (m=%d)",
                                             (long long)K,
                                             (long long)((int64_t)1 << maxbits), m);
    return m;
}
