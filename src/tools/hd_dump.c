 * Usage: hd_dump <store> <recipe_sha> [--around OFFSET]
 */
#include <string.h>
#include "../hd_common.h"
#include "../hd_store.h"
#include "../hd_recipe.h"

int main(int argc, char **argv) {
    hd_common_init();
    if (argc < 3) {
        fprintf(stderr, "usage: hd_dump <store> <recipe_sha> [--around off]\n");
        return 1;
    }
    hd_store st;
    hd_store_open(&st, argv[1], 0);
    size_t rlen;
    uint8_t *p = hd_store_get_recipe(&st, argv[2], &rlen);
    if (!p) { fprintf(stderr, "recipe not found\n"); return 1; }
    hd_recipe rec;
    if (hd_recipe_deserialize(&rec, p, rlen)) { fprintf(stderr, "parse fail\n"); return 1; }

    uint64_t around = (uint64_t)-1;
    if (argc > 4 && !strcmp(argv[3], "--around")) around = strtoull(argv[4], 0, 10);

    uint64_t pos = 0;
    for (int i = 0; i < rec.n_segs; i++) {
        const hd_seg *s = &rec.segs[i];
        uint64_t len = s->type == HD_SEG_META ? s->a
                     : s->type == HD_SEG_PAD ? s->a : s->c;
        if (around == (uint64_t)-1 ||
            (pos + len > around - 2048 && pos < around + 2048)) {
            if (s->type == HD_SEG_META) {
                printf("[%5d] %8llu META len=%u bytes=", i, (unsigned long long)pos, s->a);
                for (uint32_t k = 0; k < s->a && k < 48; k++)
                    printf("%c", (rec.bytes[s->b + k] >= 32 && rec.bytes[s->b + k] < 127) ? rec.bytes[s->b + k] : '.');
                printf("\n");
            } else if (s->type == HD_SEG_PAD) {
                printf("[%5d] %8llu PAD  z=%u\n", i, (unsigned long long)pos, s->a);
            } else {
                printf("[%5d] %8llu REF  obj=%u off=%u len=%u (obj_raw=%llu sha=%.12s)\n",
                       i, (unsigned long long)pos, s->a, s->b, s->c,
                       (unsigned long long)rec.objs[s->a].raw_len,
                       rec.objs[s->a].sha);
            }
        }
        pos += len;
    }
    printf("total bytes: %llu (tar=%llu) nseg=%d nobj=%d\n",
           (unsigned long long)pos, (unsigned long long)rec.tar_size,
           rec.n_segs, rec.n_objs);
    return 0;
}
