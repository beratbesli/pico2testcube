/**
 * Pico 2 (RP2350) - 3D ASCII Rotating Cube Demo
 * 
 * Standalone demonstration for Raspberry Pi Pico 2:
 * - USB CDC Stdio Serial Output
 * - Real-time 3D Wireframe ASCII Rendering
 * - Single-precision Floating Point Math (RP2350 FPU)
 * - Zero external dependencies (No SD card, No FatFs)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

// Screen buffer dimensions (terminal columns & rows)
#define SCREEN_WIDTH   68
#define SCREEN_HEIGHT  28

// Aspect ratio compensation for typical monospace terminal fonts (~2:1 height:width)
#define FONT_ASPECT    2.05f

// 3D Model Parameters
#define NUM_VERTICES   8
#define NUM_EDGES     12

typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    int v0, v1;
} Edge;

// Unit cube vertices (-1 to +1)
static const Vec3 cube_vertices[NUM_VERTICES] = {
    { -1.0f, -1.0f, -1.0f },
    {  1.0f, -1.0f, -1.0f },
    {  1.0f,  1.0f, -1.0f },
    { -1.0f,  1.0f, -1.0f },
    { -1.0f, -1.0f,  1.0f },
    {  1.0f, -1.0f,  1.0f },
    {  1.0f,  1.0f,  1.0f },
    { -1.0f,  1.0f,  1.0f }
};

// 12 edges connecting the 8 vertices
static const Edge cube_edges[NUM_EDGES] = {
    { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, // Back face
    { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, // Front face
    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }  // Connecting edges
};

// Frame buffer
static char frame_buf[SCREEN_HEIGHT][SCREEN_WIDTH];
static char output_buf[16384];

// Clear the frame buffer with spaces
static void clear_buffer(void) {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            frame_buf[y][x] = ' ';
        }
    }
}

// Bresenham's line algorithm for ASCII grid
static void draw_line(int x0, int y0, int x1, int y1, char ch) {
    int dx = abs(x1 - x0);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (1) {
        if (x0 >= 1 && x0 < SCREEN_WIDTH - 1 && y0 >= 1 && y0 < SCREEN_HEIGHT - 1) {
            // Do not overwrite vertex markers with regular edge chars
            if (frame_buf[y0][x0] != '+' && frame_buf[y0][x0] != 'O') {
                frame_buf[y0][x0] = ch;
            }
        }
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

// Draw decorative border frame
static void draw_border(void) {
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        frame_buf[0][x] = '=';
        frame_buf[SCREEN_HEIGHT - 1][x] = '=';
    }
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        frame_buf[y][0] = '|';
        frame_buf[y][SCREEN_WIDTH - 1] = '|';
    }
    frame_buf[0][0] = '+';
    frame_buf[0][SCREEN_WIDTH - 1] = '+';
    frame_buf[SCREEN_HEIGHT - 1][0] = '+';
    frame_buf[SCREEN_HEIGHT - 1][SCREEN_WIDTH - 1] = '+';
}

// Render text into the frame buffer at given position
static void draw_string(int x, int y, const char *str) {
    int len = (int)strlen(str);
    for (int i = 0; i < len; i++) {
        int px = x + i;
        if (px >= 1 && px < SCREEN_WIDTH - 1 && y >= 1 && y < SCREEN_HEIGHT - 1) {
            frame_buf[y][px] = str[i];
        }
    }
}

int main(void) {
    // Initialize standard I/O (USB CDC)
    stdio_init_all();

    // Initialize onboard LED if available
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif

    // Rotation angles (radians)
    float rot_x = 0.0f;
    float rot_y = 0.0f;
    float rot_z = 0.0f;

    uint32_t frame_count = 0;
    uint32_t sys_hz = clock_get_hz(clk_sys);

    // Give host USB serial client time to connect
    sleep_ms(1500);

    while (1) {
        uint64_t frame_start_us = time_us_64();

        // 1. Clear frame buffer
        clear_buffer();
        draw_border();

        // 2. Compute 3D rotation & 2D projection for all 8 vertices
        int proj_x[NUM_VERTICES];
        int proj_y[NUM_VERTICES];

        float cx = cosf(rot_x), sx = sinf(rot_x);
        float cy = cosf(rot_y), sy = sinf(rot_y);
        float cz = cosf(rot_z), sz = sinf(rot_z);

        const float cube_scale = 1.35f;
        const float camera_dist = 3.6f;
        const float fov = 26.0f;

        const int center_x = SCREEN_WIDTH / 2;
        const int center_y = SCREEN_HEIGHT / 2;

        for (int i = 0; i < NUM_VERTICES; i++) {
            float vx = cube_vertices[i].x * cube_scale;
            float vy = cube_vertices[i].y * cube_scale;
            float vz = cube_vertices[i].z * cube_scale;

            // Rotate around X axis
            float x1 = vx;
            float y1 = vy * cx - vz * sx;
            float z1 = vy * sx + vz * cx;

            // Rotate around Y axis
            float x2 = x1 * cy + z1 * sy;
            float y2 = y1;
            float z2 = -x1 * sy + z1 * cy;

            // Rotate around Z axis
            float x3 = x2 * cz - y2 * sz;
            float y3 = x2 * sz + y2 * cz;
            float z3 = z2;

            // Perspective projection
            float z_depth = z3 + camera_dist;
            if (z_depth < 0.2f) z_depth = 0.2f;

            proj_x[i] = center_x + (int)((x3 / z_depth) * fov * FONT_ASPECT);
            proj_y[i] = center_y - (int)((y3 / z_depth) * fov);
        }

        // 3. Draw 12 edges
        for (int i = 0; i < NUM_EDGES; i++) {
            int v0 = cube_edges[i].v0;
            int v1 = cube_edges[i].v1;
            draw_line(proj_x[v0], proj_y[v0], proj_x[v1], proj_y[v1], '#');
        }

        // 4. Draw vertices on top of lines
        for (int i = 0; i < NUM_VERTICES; i++) {
            int px = proj_x[i];
            int py = proj_y[i];
            if (px >= 1 && px < SCREEN_WIDTH - 1 && py >= 1 && py < SCREEN_HEIGHT - 1) {
                frame_buf[py][px] = 'O';
            }
        }

        // 5. Draw Header & Diagnostics
        char header[64];
        snprintf(header, sizeof(header), " [ PICO 2 (RP2350) 3D CUBE DEMO ] ");
        draw_string((SCREEN_WIDTH - (int)strlen(header)) / 2, 0, header);

        char stats_top[64];
        snprintf(stats_top, sizeof(stats_top), "SYS: %lu MHz | CDC USB STDIO: OK", sys_hz / 1000000UL);
        draw_string(3, 1, stats_top);

        char stats_bot[64];
        uint32_t uptime_sec = (uint32_t)(frame_start_us / 1000000ULL);
        snprintf(stats_bot, sizeof(stats_bot), "FRAME: %lu | UP: %02lu:%02lu | P:%.0f Y:%.0f R:%.0f", 
                 frame_count, uptime_sec / 60, uptime_sec % 60,
                 rot_x * (180.0f / 3.14159f),
                 rot_y * (180.0f / 3.14159f),
                 rot_z * (180.0f / 3.14159f));
        draw_string(3, SCREEN_HEIGHT - 2, stats_bot);

        // 6. Build single string frame with ANSI clear and cursor home
        // \033[2J = Clear entire screen, \033[H = Cursor to Home (1,1)
        int buf_pos = 0;
        buf_pos += snprintf(output_buf + buf_pos, sizeof(output_buf) - buf_pos, 
                            "\033[2J\033[H\033[1;36m"); // ANSI Cyan

        for (int y = 0; y < SCREEN_HEIGHT; y++) {
            for (int x = 0; x < SCREEN_WIDTH; x++) {
                char c = frame_buf[y][x];
                output_buf[buf_pos++] = c;
            }
            output_buf[buf_pos++] = '\r';
            output_buf[buf_pos++] = '\n';
        }
        
        buf_pos += snprintf(output_buf + buf_pos, sizeof(output_buf) - buf_pos, "\033[0m"); // Reset ANSI
        output_buf[buf_pos] = '\0';

        // 7. Flush out to USB CDC serial port
        printf("%s", output_buf);

        // 8. Update rotation angles for next frame
        rot_x += 0.055f;
        rot_y += 0.080f;
        rot_z += 0.035f;
        if (rot_x > 6.28318f) rot_x -= 6.28318f;
        if (rot_y > 6.28318f) rot_y -= 6.28318f;
        if (rot_z > 6.28318f) rot_z -= 6.28318f;

        frame_count++;

        // Toggle onboard LED every 10 frames (~500ms)
#ifdef PICO_DEFAULT_LED_PIN
        if (frame_count % 10 == 0) {
            gpio_put(PICO_DEFAULT_LED_PIN, (frame_count / 10) % 2);
        }
#endif

        // 9. Frame rate limiter (~50ms / 20 FPS)
        uint64_t frame_end_us = time_us_64();
        uint64_t elapsed_us = frame_end_us - frame_start_us;
        const uint64_t target_frame_time_us = 50000; // 50ms

        if (elapsed_us < target_frame_time_us) {
            sleep_us(target_frame_time_us - elapsed_us);
        }
    }

    return 0;
}
