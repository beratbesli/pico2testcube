/**
 * Pico 2 (RP2350) - Out-of-Core Virtual Memory 3D Render Engine
 * 
 * Demonstrates rendering a massive 1,000,000 byte (1 MB) 3D world
 * that exceeds Pico 2's total RAM by paging 512-byte blocks
 * to and from an SPI0 MicroSD card via an LRU Page Cache (16 KB RAM).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "sd_spi.h"
#include "vmem_fb.h"

#define VIEWPORT_WIDTH   80
#define VIEWPORT_HEIGHT  32

static char terminal_buf[VIEWPORT_HEIGHT][VIEWPORT_WIDTH];
static char output_str[16384];

// Draw a 3D wireframe cube directly into the Virtual Memory space
static void vmem_render_3d_cube(int center_x, int center_y, float size, float rx, float ry, float rz, char ch) {
    // 8 vertices of unit cube
    static const float base_v[8][3] = {
        {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
        {-1,-1,1},  {1,-1,1},  {1,1,1},  {-1,1,1}
    };
    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };

    float cx = cosf(rx), sx = sinf(rx);
    float cy = cosf(ry), sy = sinf(ry);
    float cz = cosf(rz), sz = sinf(rz);

    int px[8], py[8];

    for (int i = 0; i < 8; i++) {
        float x = base_v[i][0] * size;
        float y = base_v[i][1] * size;
        float z = base_v[i][2] * size;

        // Rotation
        float x1 = x;
        float y1 = y * cx - z * sx;
        float z1 = y * sx + z * cx;

        float x2 = x1 * cy + z1 * sy;
        float y2 = y1;
        float z2 = -x1 * sy + z1 * cy;

        float x3 = x2 * cz - y2 * sz;
        float y3 = x2 * sz + y2 * cz;

        px[i] = center_x + (int)(x3 * 2.0f); // Font aspect ratio correction
        py[i] = center_y - (int)y3;
    }

    for (int i = 0; i < 12; i++) {
        int i0 = edges[i][0];
        int i1 = edges[i][1];
        vmem_draw_line(px[i0], py[i0], px[i1], py[i1], ch);
    }
}

// Generate a massive 1,000,000 byte 3D landscape on the SD card
static void generate_massive_world(void) {
    printf("[RENDER] Initializing 1,000,000 byte Virtual World on MicroSD...\n");
    vmem_clear(' ');

    // 1. Draw outer boundary box (1000 x 1000)
    vmem_draw_box(0, 0, VMEM_WIDTH, VMEM_HEIGHT);

    // 2. Draw coordinate grid lines every 100 characters
    for (int x = 100; x < VMEM_WIDTH; x += 100) {
        for (int y = 10; y < VMEM_HEIGHT - 10; y += 4) {
            vmem_put_pixel(x, y, ':');
        }
    }
    for (int y = 100; y < VMEM_HEIGHT; y += 100) {
        for (int x = 10; x < VMEM_WIDTH - 10; x += 4) {
            vmem_put_pixel(x, y, '-');
        }
    }

    // 3. Draw a constellation of 3D cubes of various sizes across virtual space
    printf("[RENDER] Paging 3D Cubes into Virtual Framebuffer...\n");
    for (int gy = 150; gy < VMEM_HEIGHT - 100; gy += 200) {
        for (int gx = 150; gx < VMEM_WIDTH - 100; gx += 200) {
            float size = 25.0f + 10.0f * sinf((float)(gx + gy) * 0.05f);
            float angle = (float)(gx * 3 + gy * 7) * 0.02f;
            vmem_render_3d_cube(gx, gy, size, angle, angle * 1.5f, angle * 0.7f, '#');

            char label[32];
            snprintf(label, sizeof(label), "[SECTOR %03d,%03d]", gx, gy);
            vmem_draw_string(gx - 8, gy + (int)size + 3, label);
        }
    }

    // 4. Draw Center Mega Landmark
    vmem_draw_box(400, 420, 200, 160);
    vmem_draw_string(410, 430, "=== PICO 2 (RP2350) OUT-OF-CORE VIRTUAL MEMORY ===");
    vmem_draw_string(410, 445, "CANVAS SIZE : 1000 x 1000 CHARACTERS (1.00 MEGABYTE)");
    vmem_draw_string(410, 460, "RAM FOOTPRINT : ONLY 16 KB (32-PAGE LRU CACHE)");
    vmem_draw_string(410, 475, "SECONDARY DISK: 2GB SPI0 MICROSD CARD @ 20 MHz");
    vmem_render_3d_cube(500, 520, 35.0f, 0.6f, 0.8f, 0.4f, '@');

    // 5. Commit all modified pages to MicroSD card
    printf("[RENDER] Flushing dirty pages to SD card...\n");
    vmem_flush();
    printf("[RENDER] Virtual World Generation Complete!\n");
}

int main(void) {
    stdio_init_all();

#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif

    sleep_ms(2000);

    printf("\n======================================================\n");
    printf("  PICO 2 (RP2350) OUT-OF-CORE VIRTUAL MEMORY DEMO\n");
    printf("  SPI0 Pinout: MISO=GP16, CS=GP17, SCK=GP18, MOSI=GP19\n");
    printf("======================================================\n\n");

    // Initialize SD card on SPI0
    bool sd_ok = sd_spi_init();
    if (!sd_ok) {
        printf("\n[ERROR] MicroSD card initialization failed on SPI0!\n");
        printf("Please verify:\n");
        printf("  - MISO -> GP16\n  - CS   -> GP17\n  - SCK  -> GP18\n  - MOSI -> GP19\n  - VCC  -> 3V3(OUT)\n");
        while (1) {
            tight_loop_contents();
        }
    }

    // Initialize Virtual Memory manager starting at SD sector 4096
    vmem_init(4096);

    // Generate the 1,000,000 byte world onto the SD card
    generate_massive_world();

    // Camera animation parameters
    float cam_t = 0.0f;
    uint32_t frame_count = 0;
    uint32_t sys_hz = clock_get_hz(clk_sys);

    printf("\nStarting Real-time Paging Viewport Streamer...\n");
    sleep_ms(1000);

    while (1) {
        uint64_t frame_start_us = time_us_64();

        // 1. Calculate camera position (Smooth Lissajous path across the 1000x1000 world)
        int cam_x = 500 + (int)(380.0f * sinf(cam_t * 0.4f)) - (VIEWPORT_WIDTH / 2);
        int cam_y = 500 + (int)(380.0f * cosf(cam_t * 0.28f)) - (VIEWPORT_HEIGHT / 2);

        if (cam_x < 0) cam_x = 0;
        if (cam_x > VMEM_WIDTH - VIEWPORT_WIDTH) cam_x = VMEM_WIDTH - VIEWPORT_WIDTH;
        if (cam_y < 0) cam_y = 0;
        if (cam_y > VMEM_HEIGHT - VIEWPORT_HEIGHT) cam_y = VMEM_HEIGHT - VIEWPORT_HEIGHT;

        // 2. Stream pixels from Virtual Memory into terminal viewport
        // This triggers Page-In / LRU Cache hits & misses dynamically!
        for (int y = 0; y < VIEWPORT_HEIGHT; y++) {
            for (int x = 0; x < VIEWPORT_WIDTH; x++) {
                terminal_buf[y][x] = vmem_get_pixel(cam_x + x, cam_y + y);
            }
        }

        // 3. Overlay Viewport HUD & Telemetry
        vmem_stats_t stats = vmem_get_stats();

        // Top Banner
        char hud_top[VIEWPORT_WIDTH + 1];
        snprintf(hud_top, sizeof(hud_top), " [ PICO 2 OUT-OF-CORE VIRTUAL FRAMEBUFFER: 1.0 MB | RAM CACHE: 16 KB ] ");
        int top_x = (VIEWPORT_WIDTH - (int)strlen(hud_top)) / 2;
        for (int i = 0; hud_top[i] != '\0' && (top_x + i) < VIEWPORT_WIDTH - 1; i++) {
            terminal_buf[0][top_x + i] = hud_top[i];
        }

        // Bottom Telemetry
        char hud_bot[VIEWPORT_WIDTH + 1];
        snprintf(hud_bot, sizeof(hud_bot), " POS:(%04d,%04d) | HIT:%.1f%% | SD-R:%lu SD-W:%lu | %luMHz ",
                 cam_x, cam_y, stats.hit_ratio, 
                 (unsigned long)stats.sd_reads, (unsigned long)stats.sd_writes,
                 sys_hz / 1000000UL);
        for (int i = 0; hud_bot[i] != '\0' && (2 + i) < VIEWPORT_WIDTH - 1; i++) {
            terminal_buf[VIEWPORT_HEIGHT - 1][2 + i] = hud_bot[i];
        }

        // 4. Build single ANSI frame
        int buf_pos = 0;
        buf_pos += snprintf(output_str + buf_pos, sizeof(output_str) - buf_pos,
                            "\033[2J\033[H\033[1;32m"); // Green ANSI

        for (int y = 0; y < VIEWPORT_HEIGHT; y++) {
            for (int x = 0; x < VIEWPORT_WIDTH; x++) {
                output_str[buf_pos++] = terminal_buf[y][x];
            }
            output_str[buf_pos++] = '\r';
            output_str[buf_pos++] = '\n';
        }
        buf_pos += snprintf(output_str + buf_pos, sizeof(output_str) - buf_pos, "\033[0m");
        output_str[buf_pos] = '\0';

        // 5. Send out over USB CDC Serial
        printf("%s", output_str);

        // 6. Update Camera
        cam_t += 0.04f;
        frame_count++;

#ifdef PICO_DEFAULT_LED_PIN
        if (frame_count % 10 == 0) {
            gpio_put(PICO_DEFAULT_LED_PIN, (frame_count / 10) % 2);
        }
#endif

        // 7. Frame rate limiter (~30 FPS / 33ms)
        uint64_t frame_end_us = time_us_64();
        uint64_t elapsed_us = frame_end_us - frame_start_us;
        const uint64_t target_frame_us = 33000;

        if (elapsed_us < target_frame_us) {
            sleep_us(target_frame_us - elapsed_us);
        }
    }

    return 0;
}
