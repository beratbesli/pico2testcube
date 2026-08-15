#include "vmem_fb.h"
#include "sd_spi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    uint32_t page_id;
    bool     valid;
    bool     dirty;
    uint32_t last_access;
    uint8_t  data[VMEM_PAGE_SIZE];
} vmem_slot_t;

static vmem_slot_t  g_cache[VMEM_CACHE_SLOTS];
static uint32_t     g_base_sector = 0;
static uint32_t     g_access_counter = 0;
static vmem_stats_t g_stats = {0};

static int vmem_acquire_slot(uint32_t page_id) {
    g_stats.accesses++;
    g_access_counter++;

    // 1. Search for Cache Hit
    for (int i = 0; i < VMEM_CACHE_SLOTS; i++) {
        if (g_cache[i].valid && g_cache[i].page_id == page_id) {
            g_cache[i].last_access = g_access_counter;
            g_stats.cache_hits++;
            return i;
        }
    }

    // 2. Cache Miss - Find empty or LRU victim slot
    g_stats.cache_misses++;
    int victim_idx = -1;
    uint32_t oldest_time = 0xFFFFFFFF;

    for (int i = 0; i < VMEM_CACHE_SLOTS; i++) {
        if (!g_cache[i].valid) {
            victim_idx = i;
            break;
        }
        if (g_cache[i].last_access < oldest_time) {
            oldest_time = g_cache[i].last_access;
            victim_idx = i;
        }
    }

    // 3. Write-back dirty victim page to SD card
    if (g_cache[victim_idx].valid && g_cache[victim_idx].dirty) {
        sd_write_sector(g_base_sector + g_cache[victim_idx].page_id, g_cache[victim_idx].data);
        g_stats.sd_writes++;
        g_cache[victim_idx].dirty = false;
    }

    // 4. Page-In requested sector from SD card
    sd_read_sector(g_base_sector + page_id, g_cache[victim_idx].data);
    g_stats.sd_reads++;

    g_cache[victim_idx].page_id = page_id;
    g_cache[victim_idx].valid = true;
    g_cache[victim_idx].dirty = false;
    g_cache[victim_idx].last_access = g_access_counter;

    return victim_idx;
}

bool vmem_init(uint32_t base_sd_sector) {
    g_base_sector = base_sd_sector;
    g_access_counter = 0;
    memset(&g_stats, 0, sizeof(g_stats));

    for (int i = 0; i < VMEM_CACHE_SLOTS; i++) {
        g_cache[i].valid = false;
        g_cache[i].dirty = false;
        g_cache[i].last_access = 0;
    }

    printf("[VMEM] Virtual Canvas: %dx%d (%u KB) | Cache: %d pages (%d KB RAM)\n",
           VMEM_WIDTH, VMEM_HEIGHT, VMEM_TOTAL_BYTES / 1024, 
           VMEM_CACHE_SLOTS, (VMEM_CACHE_SLOTS * VMEM_PAGE_SIZE) / 1024);

    return true;
}

void vmem_clear(char fill_char) {
    uint8_t block[VMEM_PAGE_SIZE];
    memset(block, fill_char, VMEM_PAGE_SIZE);

    // Write directly to all virtual pages on SD card
    for (uint32_t p = 0; p < VMEM_TOTAL_PAGES; p++) {
        sd_write_sector(g_base_sector + p, block);
    }

    // Invalidate RAM cache
    for (int i = 0; i < VMEM_CACHE_SLOTS; i++) {
        g_cache[i].valid = false;
        g_cache[i].dirty = false;
    }
}

void vmem_put_pixel(int x, int y, char ch) {
    if (x < 0 || x >= VMEM_WIDTH || y < 0 || y >= VMEM_HEIGHT) return;

    uint32_t offset = (uint32_t)y * VMEM_WIDTH + (uint32_t)x;
    uint32_t page_id = offset / VMEM_PAGE_SIZE;
    uint32_t page_offset = offset % VMEM_PAGE_SIZE;

    int slot = vmem_acquire_slot(page_id);
    g_cache[slot].data[page_offset] = (uint8_t)ch;
    g_cache[slot].dirty = true;
}

char vmem_get_pixel(int x, int y) {
    if (x < 0 || x >= VMEM_WIDTH || y < 0 || y >= VMEM_HEIGHT) return ' ';

    uint32_t offset = (uint32_t)y * VMEM_WIDTH + (uint32_t)x;
    uint32_t page_id = offset / VMEM_PAGE_SIZE;
    uint32_t page_offset = offset % VMEM_PAGE_SIZE;

    int slot = vmem_acquire_slot(page_id);
    return (char)g_cache[slot].data[page_offset];
}

void vmem_draw_line(int x0, int y0, int x1, int y1, char ch) {
    int dx = abs(x1 - x0);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (1) {
        vmem_put_pixel(x0, y0, ch);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void vmem_draw_string(int x, int y, const char *str) {
    int len = (int)strlen(str);
    for (int i = 0; i < len; i++) {
        vmem_put_pixel(x + i, y, str[i]);
    }
}

void vmem_draw_box(int x, int y, int w, int h) {
    for (int px = x; px < x + w; px++) {
        vmem_put_pixel(px, y, '=');
        vmem_put_pixel(px, y + h - 1, '=');
    }
    for (int py = y; py < y + h; py++) {
        vmem_put_pixel(x, py, '|');
        vmem_put_pixel(x + w - 1, py, '|');
    }
    vmem_put_pixel(x, y, '+');
    vmem_put_pixel(x + w - 1, y, '+');
    vmem_put_pixel(x, y + h - 1, '+');
    vmem_put_pixel(x + w - 1, y + h - 1, '+');
}

void vmem_flush(void) {
    for (int i = 0; i < VMEM_CACHE_SLOTS; i++) {
        if (g_cache[i].valid && g_cache[i].dirty) {
            sd_write_sector(g_base_sector + g_cache[i].page_id, g_cache[i].data);
            g_cache[i].dirty = false;
            g_stats.sd_writes++;
        }
    }
}

vmem_stats_t vmem_get_stats(void) {
    if (g_stats.accesses > 0) {
        g_stats.hit_ratio = ((float)g_stats.cache_hits / (float)g_stats.accesses) * 100.0f;
    } else {
        g_stats.hit_ratio = 100.0f;
    }
    return g_stats;
}

void vmem_reset_stats(void) {
    memset(&g_stats, 0, sizeof(g_stats));
}
