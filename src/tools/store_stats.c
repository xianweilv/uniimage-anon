 * Usage: store_stats <store>
 * output: {"n_obj":N,"obj_comp":B,"obj_raw":B,"n_recipes":N,"recipe_bytes":B,
 *        "n_blockmaps":N,"meta_bytes":B}
 */
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../hd_common.h"
#include "../hd_store.h"

static uint64_t g_obj_comp, g_obj_raw, g_recipe_bytes, g_meta_bytes;
static uint64_t g_n_obj, g_n_recipes, g_n_blockmaps;

static void scan_dir(const char *root, const char *sub, int is_recipe) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", root, sub);
    DIR *d = opendir(p);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub2[1400];
        snprintf(sub2, sizeof sub2, "%s/%s", p, e->d_name);
        struct stat st;
        if (stat(sub2, &st) == 0 && S_ISDIR(st.st_mode)) {
            DIR *d2 = opendir(sub2);
            if (!d2) continue;
            struct dirent *e2;
            while ((e2 = readdir(d2))) {
                if (e2->d_name[0] == '.') continue;
                char f[1600];
                snprintf(f, sizeof f, "%s/%s", sub2, e2->d_name);
                struct stat st2;
                if (stat(f, &st2) != 0) continue;
                size_t n = strlen(e2->d_name);
                if (is_recipe) {
                    if (n >= 64) {
                        g_n_recipes++;
                        g_recipe_bytes += st2.st_size;
                    }
                } else {
                    if (n >= 64 + 4 && !strcmp(e2->d_name + 64, ".blk")) {
                        g_n_blockmaps++;
                    } else if (n >= 64) {
                        g_n_obj++;
                        g_obj_comp += st2.st_size;
                    }
                }
            }
            closedir(d2);
        }
    }
    closedir(d);
}

static void parse_meta(const char *root) {
    char p[1200];
    snprintf(p, sizeof p, "%s/objmeta.jsonl", root);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char sha[80] = {0};
        unsigned long long raw = 0, comp = 0;
        unsigned crc = 0;
        if (sscanf(line, "{\"sha\":\"%64s", sha) != 1) continue;
        char *q = strstr(line, "\"raw\":");
        if (q) raw = strtoull(q + 6, NULL, 10);
        q = strstr(line, "\"comp\":");
        if (q) comp = strtoull(q + 7, NULL, 10);
        g_meta_bytes += 0;
        (void)crc;
        g_obj_raw += raw;
    }
    fclose(f);
}

int main(int argc, char **argv) {
    hd_common_init();
    if (argc < 2) {
        fprintf(stderr, "usage: store_stats <store>\n");
        return 1;
    }
    scan_dir(argv[1], "objects", 0);
    scan_dir(argv[1], "recipes", 1);
    parse_meta(argv[1]);
    printf("{\"n_obj\":%llu,\"obj_comp\":%llu,\"obj_raw\":%llu,"
           "\"n_recipes\":%llu,\"recipe_bytes\":%llu,\"n_blockmaps\":%llu}\n",
           (unsigned long long)g_n_obj, (unsigned long long)g_obj_comp,
           (unsigned long long)g_obj_raw,
           (unsigned long long)g_n_recipes,
           (unsigned long long)g_recipe_bytes,
           (unsigned long long)g_n_blockmaps);
    return 0;
}
