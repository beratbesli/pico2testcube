#ifndef VMEM_FB_H
#define VMEM_FB_H

#include <stdint.h>
#include <stdbool.h>

// Virtual Canvas Dimensions (1,000 x 1,000 characters = 1,000,000 bytes = 1 MB)
// Exceeds Pico 2's total RAM (520 KB)!
#define VMEM_WIDTH          1000
#define VMEM_HEIGHT         1000
#define VMEM_TOTAL_BYTES    (VMEM_WIDTH * VMEM_HEIGHT)

// Page & Cache Configuration
#define VMEM_PAGE_SIZE      512  // Matches 1 SD Sector (512 bytes)
#define VMEM_TOTAL_PAGES    ((VMEM_TOTAL_BYTES + VMEM_PAGE_SIZE - 1) / VMEM_PAGE_SIZE) // 1954 pages
#define VMEM_CACHE_SLOTS    32   // Only 32 pages in RAM = 16 KB RAM footprint!

// Telemetry & Benchmark Statistics
typedef struct {
    uint64_t accesses;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint32_t sd_reads;
    uint32_t sd_writes;
    float    hit_ratio;
} vmem_stats_t;

// API
bool         vmem_init(uint32_t base_sd_sector);
void         vmem_clear(char fill_char);
void         vmem_put_pixel(int x, int y, char ch);
char         vmem_get_pixel(int x, int y);
void         vmem_draw_line(int x0, int y0, int x1, int y1, char ch);
void         vmem_draw_string(int x, int y, const char *str);
void         vmem_draw_box(int x, int y, int w, int h);
void         vmem_flush(void);
vmem_stats_t vmem_get_stats(void);
void         vmem_reset_stats(void);

#endif // VMEM_FB_H
