/* sd_backup.c - copies of the tank on the 2.16's microSD (sd_backup.h).
 *
 * The slot: SDMMC, 1 bit, CLK 2 / CMD 1 / D0 3 (Waveshare's BSP for the
 * board; D3 on GPIO 41 is pulled up and left alone), FAT, no card-detect.
 * Files (8.3 names: long file names are off in this build):
 *   /sdcard/PTANK/SAVE.BIN       the latest copy, replaced by each backup
 *   /sdcard/PTANK/Syymmdd.BIN    one a day (the RTC's date), the last 14 kept
 * A copy is the NVS blob byte for byte - the same thing progression.c reads,
 * whatever build wrote it - written to a temporary file and renamed in, so a
 * cut mid-write never leaves a half SAVE.BIN. */
#include "sd_backup.h"
#include "display_port.h"   /* board_is_sq216 */
#include "progression.h"    /* SAVE_NVS_NS / SAVE_NVS_KEY */
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "sd";
#define MNT       "/sdcard"
#define DIR_      MNT "/PTANK"
#define LATEST    DIR_ "/SAVE.BIN"
#define TMP       DIR_ "/SAVE.TMP"
#define KEEP_DAYS 14
#define SAVE_MAX  8192                          /* far over save_t (~1.7 KB): a newer build's longer save fits */
#define EVERY_US  (3LL * 3600 * 1000000)
#define FIRST_US  (60LL * 1000000)

static sdmmc_card_t *s_card;
static bool s_absent;                           /* the last mount found no card: said once, tried again later */

static bool mount(void) {
    if (!board_is_sq216()) return false;
    const esp_vfs_fat_sdmmc_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 2, .allocation_unit_size = 16 * 1024 };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1; slot.clk = GPIO_NUM_2; slot.cmd = GPIO_NUM_1; slot.d0 = GPIO_NUM_3;
    slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC; slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t e = esp_vfs_fat_sdmmc_mount(MNT, &host, &slot, &mc, &s_card);
    if (e != ESP_OK) {
        if (!s_absent) ESP_LOGI(TAG, "no card (or not FAT): %s - the copies wait for one", esp_err_to_name(e));
        s_absent = true; s_card = NULL; return false;
    }
    if (s_absent) ESP_LOGI(TAG, "a card is in");
    s_absent = false;
    mkdir(DIR_, 0777);
    return true;
}
static void unmount(void) { if (s_card) esp_vfs_fat_sdcard_unmount(MNT, s_card); s_card = NULL; }

static size_t nvs_blob(uint8_t *buf, size_t max) {   /* the save as NVS holds it; 0 = none */
    nvs_handle_t h; size_t len = 0;
    if (nvs_open(SAVE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_blob(h, SAVE_NVS_KEY, NULL, &len) != ESP_OK || len == 0 || len > max || nvs_get_blob(h, SAVE_NVS_KEY, buf, &len) != ESP_OK) len = 0;
    nvs_close(h);
    return len;
}
static size_t read_file(const char *path, uint8_t *buf, size_t max) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    size_t n = fread(buf, 1, max, f); bool more = fgetc(f) != EOF; fclose(f);
    return more ? 0 : n;                        /* longer than any save: not ours */
}
static bool write_file(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(TMP, "wb"); if (!f) return false;
    bool ok = fwrite(buf, 1, len, f) == len; ok = fflush(f) == 0 && ok; fclose(f);
    if (!ok) { remove(TMP); return false; }
    remove(path);                               /* FAT's rename does not replace */
    return rename(TMP, path) == 0;
}
static void prune(void) {                       /* the dated copies past KEEP_DAYS go, oldest first (the names sort by date) */
    DIR *d = opendir(DIR_); if (!d) return;
    char names[64][13]; int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < 64)
        if (e->d_name[0] == 'S' && strlen(e->d_name) == 11 && strcmp(e->d_name, "SAVE.BIN")) { strncpy(names[n], e->d_name, 12); names[n++][12] = 0; }
    closedir(d);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (strcmp(names[i], names[j]) > 0) { char t[13]; memcpy(t, names[i], 13); memcpy(names[i], names[j], 13); memcpy(names[j], t, 13); }
    for (int i = 0; i + KEEP_DAYS < n; i++) { char p[64]; snprintf(p, sizeof p, DIR_ "/%.12s", names[i]); remove(p); }
}

bool sd_backup_now(const char *why) {
    if (!board_is_sq216()) return false;
    uint8_t *buf = malloc(SAVE_MAX); if (!buf) return false;
    size_t len = nvs_blob(buf, SAVE_MAX);
    bool ok = false;
    if (len && mount()) {
        ok = write_file(LATEST, buf, len);
        time_t now = time(NULL);
        if (ok && now > 1700000000) {           /* the RTC's date: one copy a day */
            struct tm tm; localtime_r(&now, &tm);
            char p[40]; snprintf(p, sizeof p, DIR_ "/S%02d%02d%02d.BIN", tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday);
            ok = write_file(p, buf, len);
            prune();
        }
        unmount();
        ESP_LOGI(TAG, "backup (%s): %u bytes %s", why, (unsigned)len, ok ? "on the card" : "FAILED to write");
    }
    free(buf);
    return ok;
}

static bool restore_from(const char *path) {
    uint8_t *buf = malloc(SAVE_MAX); if (!buf) return false;
    size_t len = read_file(path, buf, SAVE_MAX);
    bool ok = false;
    if (len) {
        nvs_handle_t h;
        if (nvs_open(SAVE_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            ok = nvs_set_blob(h, SAVE_NVS_KEY, buf, len) == ESP_OK && nvs_commit(h) == ESP_OK;
            nvs_close(h);
        }
    }
    ESP_LOGI(TAG, "restore %s: %u bytes %s", path, (unsigned)len, ok ? "-> the save" : "FAILED");
    free(buf);
    return ok;
}
bool sd_backup_restore_if_empty(void) {
    if (!board_is_sq216()) return false;
    nvs_handle_t h; size_t len = 0;
    if (nvs_open(SAVE_NVS_NS, NVS_READONLY, &h) == ESP_OK) { nvs_get_blob(h, SAVE_NVS_KEY, NULL, &len); nvs_close(h); }
    if (len) return false;                      /* a tank is saved: the card never overrides it */
    if (!mount()) return false;
    struct stat st; bool ok = stat(LATEST, &st) == 0 && restore_from(LATEST);
    unmount();
    if (ok) ESP_LOGW(TAG, "no tank in NVS: the card's latest copy brought back");
    return ok;
}
bool sd_backup_restore(const char *name) {
    if (!mount()) return false;
    char p[300]; snprintf(p, sizeof p, DIR_ "/%s", name && *name ? name : "SAVE.BIN");
    bool ok = restore_from(p);
    unmount();
    return ok;
}
void sd_backup_status(void) {
    if (!board_is_sq216()) { ESP_LOGI(TAG, "no SD slot on this board"); return; }
    if (!mount()) return;
    ESP_LOGI(TAG, "card: %s, %llu MB", s_card->cid.name, (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024));
    DIR *d = opendir(DIR_); struct dirent *e; int n = 0;
    while (d && (e = readdir(d))) {
        char p[300]; struct stat st; snprintf(p, sizeof p, DIR_ "/%s", e->d_name);
        if (stat(p, &st) == 0) { ESP_LOGI(TAG, "  %-12s %6ld bytes", e->d_name, (long)st.st_size); n++; }
    }
    if (d) closedir(d);
    if (!n) ESP_LOGI(TAG, "  (no copies yet)");
    unmount();
}
void sd_backup_poll(int64_t now_us) {
    static int64_t next = FIRST_US;
    if (!board_is_sq216() || now_us < next) return;
    next = now_us + EVERY_US;
    sd_backup_now(now_us < EVERY_US ? "boot" : "every 3 h");
}
