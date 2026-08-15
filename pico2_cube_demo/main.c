/**
 * Pico 2 (RP2350) - 125-Cube 3D ASCII Stress Test & Benchmark
 * 
 * Hardware Benchmark for Raspberry Pi Pico 2:
 * - 125 Simultaneous 3D Cubes (5x5x5 Matrix)
 * - 1,000 Vertices transformed & projected per frame
 * - 1,500 Wireframe Edges rasterized with hardware FPU
 * - Full 2D ASCII Z-Buffer (Depth Buffer) with distance-shading
 * - Real-time Microsecond Hardware Benchmark Telemetry
 * - USB CDC Stdio Serial Output
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

// Screen buffer dimensions (terminal columns & rows)
#define SCREEN_WIDTH   80
#define SCREEN_HEIGHT  34

// Aspect ratio compensation for typical monospace terminal fonts (~2.1:1 height:width)
#define FONT_ASPECT    2.15f

// Grid Configuration (5 x 5 x 5 = 125 Cubes)
#define GRID_DIM       5
#define TOTAL_CUBES    (GRID_DIM * GRID_DIM * GRID_DIM) // 125
#define VERTS_PER_CUBE 8
#define EDGES_PER_CUBE 12
#define TOTAL_VERTS    (TOTAL_CUBES * VERTS_PER_CUBE)   // 1,000
#define TOTAL_EDGES    (TOTAL_CUBES * EDGES_PER_CUBE)   // 1,500

typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    int v0, v1;
} Edge;

// Unit cube vertices (-1 to +1)
static const Vec3 base_cube_verts[VERTS_PER_CUBE] = {
    { -1.0f, -1.0f, -1.0f },
    {  1.0f, -1.0f, -1.0f },
    {  1.0f,  1.0f, -1.0f },
    { -1.0f,  1.0f, -1.0f },
    { -1.0f, -1.0f,  1.0f },
    {  1.0f, -1.0f,  1.0f },
    {  1.0f,  1.0f,  1.0f },
    { -1.0f,  1.0f,  1.0f }
};

// 12 edges per cube
static const Edge base_cube_edges[EDGES_PER_CUBE] = {
    { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, // Back
    { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, // Front
    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }  // Connectors
};

// Global frame and depth buffer
static char  frame_buf[SCREEN_HEIGHT][SCREEN_WIDTH];
static float z_buffer[SCREEN_HEIGHT][SCREEN_WIDTH];
static char  output_buf[32768];

// Projected vertex cache
static int   proj_x[TOTAL_VERTS];
static int   proj_y[TOTAL_VERTS];
static float proj_z[TOTAL_VERTS];

// Clear the frame buffer and reset Z-buffer
static void clear_buffers(void) {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            frame_buf[y][x] = ' ';
            z_buffer[y][x]  = 1e9f; // Far plane
        }
    }
}

// Select ASCII character based on depth (closer = denser/brighter)
static inline char depth_to_char(float z) {
    if (z < 3.8f) return '@';
    if (z < 4.8f) return '#';
    if (z < 6.0f) return '*';
    if (z < 7.4f) return '+';
    if (z < 9.0f) return ':';
    return '.';
}

// Bresenham's line algorithm with Z-Buffer depth interpolation
static void draw_line_z(int x0, int y0, float z0, int x1, int y1, float z1) {
    int dx = abs(x1 - x0);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int steps = (dx > -dy) ? dx : -dy;
    if (steps == 0) steps = 1;
    float z_step = (z1 - z0) / (float)steps;
    float cur_z = z0;

    int cur_x = x0;
    int cur_y = y0;

    while (1) {
        if (cur_x >= 1 && cur_x < SCREEN_WIDTH - 1 && cur_y >= 1 && cur_y < SCREEN_HEIGHT - 1) {
            if (cur_z < z_buffer[cur_y][cur_x]) {
                z_buffer[cur_y][cur_x] = cur_z;
                frame_buf[cur_y][cur_x] = depth_to_char(cur_z);
            }
        }
        if (cur_x == x1 && cur_y == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            cur_x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            cur_y += sy;
        }
        cur_z += z_step;
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
        if (px >= 1 && px < SCREEN_WIDTH - 1 && y >= 0 && y < SCREEN_HEIGHT) {
            frame_buf[y][px] = str[i];
        }
    }
}

int main(void) {
    // Initialize standard I/O (USB CDC)
    stdio_init_all();

    // Initialize onboard LED (GPIO 25)
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif

    // Cluster & Local Rotation Angles (radians)
    float world_rx = 0.4f;
    float world_ry = 0.0f;
    float world_rz = 0.2f;

    float local_rx = 0.0f;
    float local_ry = 0.0f;

    uint32_t frame_count = 0;
    uint32_t sys_hz = clock_get_hz(clk_sys);

    // Warm-up delay for USB CDC serial host connection
    sleep_ms(1500);

    const float cube_radius   = 0.22f; // Size of each individual cube
    const float grid_spacing  = 0.70f; // Distance between cube centers
    const float camera_dist   = 7.0f;  // Camera distance
    const float fov           = 42.0f; // Perspective FOV

    const int center_x = SCREEN_WIDTH / 2;
    const int center_y = (SCREEN_HEIGHT / 2) - 1;

    while (1) {
        uint64_t frame_start_us = time_us_64();

        // 1. Clear frame and depth buffer
        clear_buffers();

        // 2. Pre-calculate rotation trigonometric constants (RP2350 FPU)
        float wcx = cosf(world_rx), wsx = sinf(world_rx);
        float wcy = cosf(world_ry), wsy = sinf(world_ry);
        float wcz = cosf(world_rz), wsz = sinf(world_rz);

        float lcx = cosf(local_rx), lsx = sinf(local_rx);
        float lcy = cosf(local_ry), lsy = sinf(local_ry);

        uint64_t compute_start_us = time_us_64();

        // 3. Transform & Project all 1,000 Vertices (125 Cubes x 8 Verts)
        int vert_idx = 0;
        for (int gx = -2; gx <= 2; gx++) {
            for (int gy = -2; gy <= 2; gy++) {
                for (int gz = -2; gz <= 2; gz++) {
                    
                    // Center position of this cube in the 3D grid
                    float ox = (float)gx * grid_spacing;
                    float oy = (float)gy * grid_spacing;
                    float oz = (float)gz * grid_spacing;

                    // Compute all 8 vertices for this cube
                    for (int v = 0; v < VERTS_PER_CUBE; v++) {
                        // A. Local Cube Rotation
                        float lx = base_cube_verts[v].x * cube_radius;
                        float ly = base_cube_verts[v].y * cube_radius;
                        float lz = base_cube_verts[v].z * cube_radius;

                        // Rotate local X
                        float ly1 = ly * lcx - lz * lsx;
                        float lz1 = ly * lsx + lz * lcx;
                        float lx1 = lx;

                        // Rotate local Y
                        float lx2 = lx1 * lcy + lz1 * lsy;
                        float ly2 = ly1;
                        float lz2 = -lx1 * lsy + lz1 * lcy;

                        // B. Offset to grid center
                        float px_w = lx2 + ox;
                        float py_w = ly2 + oy;
                        float pz_w = lz2 + oz;

                        // C. Global World Cluster Rotation
                        // Rotate World X
                        float wx1 = px_w;
                        float wy1 = py_w * wcx - pz_w * wsx;
                        float wz1 = py_w * wsx + pz_w * wcx;

                        // Rotate World Y
                        float wx2 = wx1 * wcy + wz1 * wsy;
                        float wy2 = wy1;
                        float wz2 = -wx1 * wsy + wz1 * wcy;

                        // Rotate World Z
                        float wx3 = wx2 * wcz - wy2 * wsz;
                        float wy3 = wx2 * wsz + wy2 * wcz;
                        float wz3 = wz2;

                        // D. Perspective Projection & Depth Buffer Z
                        float z_depth = wz3 + camera_dist;
                        if (z_depth < 0.3f) z_depth = 0.3f;

                        proj_x[vert_idx] = center_x + (int)((wx3 / z_depth) * fov * FONT_ASPECT);
                        proj_y[vert_idx] = center_y - (int)((wy3 / z_depth) * fov);
                        proj_z[vert_idx] = z_depth;

                        vert_idx++;
                    }
                }
            }
        }

        // 4. Rasterize all 1,500 Edges with Z-Buffer
        for (int c = 0; c < TOTAL_CUBES; c++) {
            int base_v = c * VERTS_PER_CUBE;
            for (int e = 0; e < EDGES_PER_CUBE; e++) {
                int i0 = base_v + base_cube_edges[e].v0;
                int i1 = base_v + base_cube_edges[e].v1;
                draw_line_z(proj_x[i0], proj_y[i0], proj_z[i0],
                            proj_x[i1], proj_y[i1], proj_z[i1]);
            }
        }

        uint64_t compute_end_us = time_us_64();
        uint32_t compute_duration_us = (uint32_t)(compute_end_us - compute_start_us);

        // 5. Draw Decorative Border & Status Headers
        draw_border();

        char title[80];
        snprintf(title, sizeof(title), " [ PICO 2 (RP2350) 125-CUBE 3D STRESS BENCHMARK ] ");
        draw_string((SCREEN_WIDTH - (int)strlen(title)) / 2, 0, title);

        char stats1[80];
        snprintf(stats1, sizeof(stats1), "SYS: %lu MHz | CUBES: %d | VERTS: %d | EDGES: %d",
                 sys_hz / 1000000UL, TOTAL_CUBES, TOTAL_VERTS, TOTAL_EDGES);
        draw_string(3, 1, stats1);

        char stats2[80];
        uint32_t raw_fps = (compute_duration_us > 0) ? (1000000UL / compute_duration_us) : 9999;
        snprintf(stats2, sizeof(stats2), "FPU CALC: %lu us (%.2f ms) | POTENTIAL: ~%lu FPS | Z-BUF: ON",
                 (unsigned long)compute_duration_us, (float)compute_duration_us / 1000.0f, (unsigned long)raw_fps);
        draw_string(3, SCREEN_HEIGHT - 2, stats2);

        // 6. Build double-buffered ANSI output string
        int buf_pos = 0;
        buf_pos += snprintf(output_buf + buf_pos, sizeof(output_buf) - buf_pos, 
                            "\033[2J\033[H\033[1;36m"); // Cyan ANSI

        for (int y = 0; y < SCREEN_HEIGHT; y++) {
            for (int x = 0; x < SCREEN_WIDTH; x++) {
                output_buf[buf_pos++] = frame_buf[y][x];
            }
            output_buf[buf_pos++] = '\r';
            output_buf[buf_pos++] = '\n';
        }
        buf_pos += snprintf(output_buf + buf_pos, sizeof(output_buf) - buf_pos, "\033[0m");
        output_buf[buf_pos] = '\0';

        // 7. Transmit to USB CDC Serial
        printf("%s", output_buf);

        // 8. Increment Rotation Angles
        world_ry += 0.045f;
        world_rx += 0.025f;
        world_rz += 0.015f;
        local_rx += 0.060f;
        local_ry += 0.080f;

        if (world_rx > 6.28318f) world_rx -= 6.28318f;
        if (world_ry > 6.28318f) world_ry -= 6.28318f;
        if (world_rz > 6.28318f) world_rz -= 6.28318f;
        if (local_rx > 6.28318f) local_rx -= 6.28318f;
        if (local_ry > 6.28318f) local_ry -= 6.28318f;

        frame_count++;

        // LED toggle every 10 frames
#ifdef PICO_DEFAULT_LED_PIN
        if (frame_count % 10 == 0) {
            gpio_put(PICO_DEFAULT_LED_PIN, (frame_count / 10) % 2);
        }
#endif

        // 9. Frame interval limiter (~33 FPS / 30ms for smooth terminal display)
        uint64_t frame_end_us = time_us_64();
        uint64_t elapsed_us = frame_end_us - frame_start_us;
        const uint64_t target_frame_us = 30000; // 30ms

        if (elapsed_us < target_frame_us) {
            sleep_us(target_frame_us - elapsed_us);
        }
    }

    return 0;
}
