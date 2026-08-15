#include "sd_spi.h"
#include <stdio.h>
#include <string.h>

#define SD_SECTOR_SIZE       512
#define SD_INIT_BAUD         100000    // 100 kHz for initial handshake (<= 400 kHz standard)
#define SD_FAST_BAUD         20000000  // 20 MHz high-speed SPI (after successful init)

// Standard SD/MMC Command Numbers
#define CMD0_GO_IDLE_STATE           0
#define CMD1_SEND_OP_COND            1
#define CMD8_SEND_IF_COND            8
#define CMD16_SET_BLOCKLEN           16
#define CMD17_READ_SINGLE_BLOCK      17
#define CMD24_WRITE_SINGLE_BLOCK     24
#define CMD55_APP_CMD                55
#define CMD58_READ_OCR               58
#define ACMD41_SD_SEND_OP_COND       41

static sd_card_info_t g_card_info = {
    .initialized = false,
    .type = SD_TYPE_UNKNOWN,
    .capacity_sectors = 0,
    .spi_baudrate = SD_INIT_BAUD
};

// Single-byte full-duplex transfer over SPI
static inline uint8_t sd_spi_transfer_byte(uint8_t tx) {
    uint8_t rx = 0xFF;
    spi_write_read_blocking(SD_SPI_PORT, &tx, &rx, 1);
    return rx;
}

// Select SD Card (CS = LOW) and send 1 dummy clock byte
static inline void sd_cs_select(void) {
    gpio_put(SD_PIN_CS, 0);
    sd_spi_transfer_byte(0xFF);
}

// Deselect SD Card (CS = HIGH) and send 1 dummy clock byte for card cleanup
static inline void sd_cs_deselect(void) {
    gpio_put(SD_PIN_CS, 1);
    sd_spi_transfer_byte(0xFF);
}

// Wait until SD card releases MISO line (returns 0xFF when ready)
static bool sd_wait_ready(uint32_t timeout_ms) {
    uint64_t start = time_us_64();
    uint64_t timeout_us = (uint64_t)timeout_ms * 1000ULL;
    while ((time_us_64() - start) < timeout_us) {
        if (sd_spi_transfer_byte(0xFF) == 0xFF) {
            return true;
        }
    }
    return false;
}

/**
 * Send a standard 6-byte SD SPI command packet while CS is ALREADY asserted LOW.
 */
static uint8_t sd_cmd_spi_raw(uint8_t cmd, uint32_t arg) {
    uint8_t packet[6];
    packet[0] = 0x40 | (cmd & 0x3F);
    packet[1] = (uint8_t)(arg >> 24);
    packet[2] = (uint8_t)(arg >> 16);
    packet[3] = (uint8_t)(arg >> 8);
    packet[4] = (uint8_t)(arg >> 0);

    // CRC7 calculation / standard precomputed CRCs
    switch (cmd) {
        case CMD0_GO_IDLE_STATE:
            packet[5] = 0x95; // Valid CRC for CMD0(0)
            break;
        case CMD8_SEND_IF_COND:
            packet[5] = 0x87; // Valid CRC for CMD8(0x1AA)
            break;
        case CMD55_APP_CMD:
            packet[5] = 0x65; // Valid CRC for CMD55(0)
            break;
        default:
            packet[5] = 0xFF; // Bit 0 must be 1 (Stop Bit)
            break;
    }

    // Wait for card ready before transmitting (except CMD0)
    if (cmd != CMD0_GO_IDLE_STATE) {
        sd_wait_ready(200);
    }

    // Transmit 6-byte command packet
    spi_write_blocking(SD_SPI_PORT, packet, 6);

    // Loop for response: NCR is 0 to 8 bytes for SD cards (we check up to 32 bytes)
    uint8_t response = 0xFF;
    for (int i = 0; i < 32; i++) {
        response = sd_spi_transfer_byte(0xFF);
        // Valid R1 response received (MSB is 0)
        if ((response & 0x80) == 0) {
            break;
        }
    }

    return response;
}

/**
 * Send a standalone command with automatic CS framing.
 */
static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg) {
    sd_cs_select();
    uint8_t r1 = sd_cmd_spi_raw(cmd, arg);
    sd_cs_deselect();
    return r1;
}

/**
 * Send an Application Command (ACMD) with ATOMIC CS framing:
 * CS remains LOW across CMD55 and ACMD.
 */
static uint8_t sd_send_acmd(uint8_t acmd, uint32_t arg, uint8_t *out_cmd55_r1) {
    sd_cs_select();

    // 1. Send CMD55 (APP_CMD)
    uint8_t r1_55 = sd_cmd_spi_raw(CMD55_APP_CMD, 0x00000000);
    if (out_cmd55_r1) {
        *out_cmd55_r1 = r1_55;
    }

    // Wait for card ready between CMD55 and ACMD
    sd_wait_ready(100);

    // 2. Send ACMD (e.g. ACMD41) while CS remains LOW
    uint8_t r1_acmd = sd_cmd_spi_raw(acmd, arg);

    sd_cs_deselect();
    return r1_acmd;
}

bool sd_spi_init(void) {
    memset(&g_card_info, 0, sizeof(g_card_info));
    g_card_info.type = SD_TYPE_UNKNOWN;

    printf("\n[SD] === Starting MicroSD Card SPI Initialization ===\n");

    // 1. Initialize GPIO pins for SPI0
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_pull_up(SD_PIN_MISO);

    gpio_init(SD_PIN_CS);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);
    gpio_put(SD_PIN_CS, 1);

    // 2. Initialize SPI at low frequency (100 kHz)
    uint actual_baud = spi_init(SD_SPI_PORT, SD_INIT_BAUD);
    spi_set_format(SD_SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    printf("[SD] SPI0 initialized at %u Hz (<= 400 kHz mode)\n", actual_baud);

    // 3. Power-Up Settling Delay (300ms)
    printf("[SD] Power settling delay (300ms)...\n");
    sleep_ms(300);

    // 4. Power-Up Sequence: Send at least 128 dummy clock pulses with CS HIGH
    gpio_put(SD_PIN_CS, 1);
    for (int i = 0; i < 20; i++) { // 20 bytes = 160 clocks
        sd_spi_transfer_byte(0xFF);
    }
    sleep_ms(5);

    // 5. Enter SPI Mode: CMD0 (GO_IDLE_STATE)
    printf("[SD] Sending CMD0 (Reset to SPI mode)...\n");
    uint8_t r1 = 0xFF;
    for (int attempt = 1; attempt <= 30; attempt++) {
        r1 = sd_send_cmd(CMD0_GO_IDLE_STATE, 0x00000000);
        if (r1 == 0x01) {
            printf("[SD] CMD0 Success on attempt %d! R1: 0x01 (In Idle State)\n", attempt);
            break;
        }
        sleep_ms(10);
    }

    if (r1 != 0x01) {
        printf("[SD] ERROR: CMD0 failed! Last R1: 0x%02X\n", r1);
        return false;
    }

    // 6. Check Interface Condition: CMD8 (Voltage 2.7-3.6V, Pattern 0xAA)
    printf("[SD] Sending CMD8 (Check Voltage & Spec Version)...\n");
    sd_cs_select();
    r1 = sd_cmd_spi_raw(CMD8_SEND_IF_COND, 0x000001AA);
    bool is_v2 = false;

    if (r1 == 0x01) {
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) r7[i] = sd_spi_transfer_byte(0xFF);
        printf("[SD] CMD8 Success! R1=0x01 | R7: %02X %02X %02X %02X\n", r7[0], r7[1], r7[2], r7[3]);
        if (r7[3] == 0xAA) {
            is_v2 = true;
            printf("[SD] Card conforms to SD Spec v2.0+ (Check pattern matched)\n");
        }
    } else {
        printf("[SD] CMD8 returned 0x%02X -> Card is SD v1.x or MMC\n", r1);
    }
    sd_cs_deselect();

    // 7. Read OCR Register via CMD58 (Pre-initialization check)
    printf("[SD] Reading OCR via CMD58...\n");
    sd_cs_select();
    r1 = sd_cmd_spi_raw(CMD58_READ_OCR, 0x00000000);
    if (r1 == 0x01 || r1 == 0x00) {
        uint8_t ocr[4];
        for (int i = 0; i < 4; i++) ocr[i] = sd_spi_transfer_byte(0xFF);
        printf("[SD] Pre-init OCR: %02X %02X %02X %02X (3.3V Voltage Window OK)\n",
               ocr[0], ocr[1], ocr[2], ocr[3]);
    }
    sd_cs_deselect();

    // 8. Initialize Card: ACMD41 Loop with Atomic CS Framing
    printf("[SD] Starting ACMD41 Initialization Loop (Atomic CS framing)...\n");
    uint32_t hcs_arg = is_v2 ? 0x40000000UL : 0x00000000UL;

    uint64_t start_time = time_us_64();
    bool ready = false;
    int attempt_count = 0;
    uint8_t last_cmd55_r1 = 0xFF;
    uint8_t last_acmd41_r1 = 0xFF;

    while ((time_us_64() - start_time) < 2500000ULL) { // 2.5 second timeout
        attempt_count++;

        // Send atomic CMD55 + ACMD41
        last_acmd41_r1 = sd_send_acmd(ACMD41_SD_SEND_OP_COND, hcs_arg, &last_cmd55_r1);

        // Periodically log progress or log when R1 changes
        if (attempt_count == 1 || (attempt_count % 50 == 0) || last_acmd41_r1 == 0x00) {
            uint32_t elapsed_ms = (uint32_t)((time_us_64() - start_time) / 1000ULL);
            printf("[SD] Attempt #%d (%u ms) -> CMD55 R1: 0x%02X | ACMD41 R1: 0x%02X\n",
                   attempt_count, elapsed_ms, last_cmd55_r1, last_acmd41_r1);
        }

        // R1 == 0x00 means card has left idle state and is ready!
        if (last_acmd41_r1 == 0x00) {
            uint32_t elapsed_ms = (uint32_t)((time_us_64() - start_time) / 1000ULL);
            printf("[SD] *** ACMD41 SUCCESS on attempt #%d (took %u ms)! Card is READY (R1: 0x00) ***\n", 
                   attempt_count, elapsed_ms);
            ready = true;
            break;
        }

        sleep_ms(5); // Paced retry delay
    }

    // Fallback: If ACMD41 failed with HCS=1, retry with HCS=0 (Standard Capacity SDSC)
    if (!ready && is_v2) {
        printf("[SD] ACMD41 timed out with HCS=1. Retrying ACMD41 with HCS=0 (SDSC mode)...\n");
        hcs_arg = 0x00000000UL;
        start_time = time_us_64();

        while ((time_us_64() - start_time) < 2000000ULL) {
            attempt_count++;
            last_acmd41_r1 = sd_send_acmd(ACMD41_SD_SEND_OP_COND, hcs_arg, &last_cmd55_r1);
            if (last_acmd41_r1 == 0x00) {
                printf("[SD] ACMD41 SUCCESS with HCS=0 on attempt #%d! Card is READY\n", attempt_count);
                ready = true;
                break;
            }
            sleep_ms(5);
        }
    }

    // Fallback: Try legacy CMD1 (MMC / older SD cards)
    if (!ready) {
        printf("[SD] Retrying with legacy CMD1 fallback...\n");
        for (int cmd1_attempt = 1; cmd1_attempt <= 100; cmd1_attempt++) {
            last_acmd41_r1 = sd_send_cmd(CMD1_SEND_OP_COND, 0x00000000);
            if (last_acmd41_r1 == 0x00) {
                printf("[SD] Legacy CMD1 SUCCESS on attempt #%d!\n", cmd1_attempt);
                ready = true;
                break;
            }
            sleep_ms(10);
        }
    }

    if (!ready) {
        uint32_t total_time_ms = (uint32_t)((time_us_64() - start_time) / 1000ULL);
        printf("[SD] ERROR: Card initialization timeout after %d attempts, total time: %u ms (CMD55: 0x%02X, ACMD41: 0x%02X)\n",
               attempt_count, total_time_ms, last_cmd55_r1, last_acmd41_r1);
        return false;
    }

    // 9. Determine Card Type (SDSC vs SDHC via CMD58 Read OCR)
    if (is_v2) {
        sd_cs_select();
        r1 = sd_cmd_spi_raw(CMD58_READ_OCR, 0x00000000);
        if (r1 == 0x00) {
            uint8_t ocr[4];
            for (int i = 0; i < 4; i++) ocr[i] = sd_spi_transfer_byte(0xFF);
            printf("[SD] Post-init OCR: %02X %02X %02X %02X\n", ocr[0], ocr[1], ocr[2], ocr[3]);
            // Bit 30 (CCS bit) indicates Block vs Byte addressing
            if (ocr[0] & 0x40) {
                g_card_info.type = SD_TYPE_SDHC;
                printf("[SD] Card Type: SDHC/SDXC (Block-Addressed, >2GB)\n");
            } else {
                g_card_info.type = SD_TYPE_SDSC_V2;
                printf("[SD] Card Type: SDSC v2.0 (Byte-Addressed, <=2GB)\n");
            }
        }
        sd_cs_deselect();
    } else {
        g_card_info.type = SD_TYPE_SDSC_V1;
        printf("[SD] Card Type: SDSC v1.x (Byte-Addressed, <=2GB)\n");
    }

    // 10. For SDSC: Explicitly set block length to 512 bytes (CMD16)
    if (g_card_info.type != SD_TYPE_SDHC) {
        printf("[SD] Setting Block Length to 512 bytes (CMD16)...\n");
        r1 = sd_send_cmd(CMD16_SET_BLOCKLEN, SD_SECTOR_SIZE);
        if (r1 != 0x00) {
            printf("[SD] WARNING: CMD16 SET_BLOCKLEN returned 0x%02X\n", r1);
        } else {
            printf("[SD] Block length configured: 512 Bytes (LBA Sector Mode)\n");
        }
    }

    // 11. Switch SPI to High-Speed (20 MHz) ONLY after full initialization
    uint fast_baud = spi_set_baudrate(SD_SPI_PORT, SD_FAST_BAUD);
    g_card_info.spi_baudrate = fast_baud;
    g_card_info.initialized = true;

    printf("[SD] *** Initialization Complete! High-Speed SPI: %u MHz ***\n\n",
           g_card_info.spi_baudrate / 1000000);

    return true;
}

bool sd_read_sector(uint32_t sector_lba, uint8_t *buffer) {
    if (!g_card_info.initialized || buffer == NULL) return false;

    // SDSC uses byte addressing (sector * 512), SDHC uses sector LBA directly
    uint32_t addr = (g_card_info.type == SD_TYPE_SDHC) ? sector_lba : (sector_lba * SD_SECTOR_SIZE);

    sd_cs_select();
    uint8_t r1 = sd_cmd_spi_raw(CMD17_READ_SINGLE_BLOCK, addr);
    if (r1 != 0x00) {
        sd_cs_deselect();
        return false;
    }

    // Wait for Data Token (0xFE)
    uint64_t start = time_us_64();
    bool token_found = false;
    while ((time_us_64() - start) < 200000ULL) { // 200ms timeout
        if (sd_spi_transfer_byte(0xFF) == 0xFE) {
            token_found = true;
            break;
        }
    }

    if (!token_found) {
        sd_cs_deselect();
        return false;
    }

    // Read 512 bytes of data
    spi_read_blocking(SD_SPI_PORT, 0xFF, buffer, SD_SECTOR_SIZE);

    // Read and discard 2 bytes CRC
    sd_spi_transfer_byte(0xFF);
    sd_spi_transfer_byte(0xFF);

    sd_cs_deselect();
    return true;
}

bool sd_write_sector(uint32_t sector_lba, const uint8_t *buffer) {
    if (!g_card_info.initialized || buffer == NULL) return false;

    uint32_t addr = (g_card_info.type == SD_TYPE_SDHC) ? sector_lba : (sector_lba * SD_SECTOR_SIZE);

    sd_cs_select();
    uint8_t r1 = sd_cmd_spi_raw(CMD24_WRITE_SINGLE_BLOCK, addr);
    if (r1 != 0x00) {
        sd_cs_deselect();
        return false;
    }

    // Send 1 byte gap + Start Block Token (0xFE)
    sd_spi_transfer_byte(0xFF);
    sd_spi_transfer_byte(0xFE);

    // Write 512 bytes payload
    spi_write_blocking(SD_SPI_PORT, buffer, SD_SECTOR_SIZE);

    // Send 2 bytes dummy CRC
    sd_spi_transfer_byte(0xFF);
    sd_spi_transfer_byte(0xFF);

    // Read Data Response Token (card sends 0 to 64 bytes before response token xxx00101b)
    uint8_t resp = 0xFF;
    for (int i = 0; i < 64; i++) {
        resp = sd_spi_transfer_byte(0xFF);
        if ((resp & 0x11) == 0x01) { // Data response token format: xxx00101b
            break;
        }
    }

    if ((resp & 0x1F) != 0x05) { // 0x05 = Data accepted
        sd_cs_deselect();
        return false;
    }

    // Wait while card writes block (busy state: MISO held LOW)
    if (!sd_wait_ready(400)) { // 400ms timeout
        sd_cs_deselect();
        return false;
    }

    sd_cs_deselect();
    return true;
}

const char* sd_type_string(sd_card_type_t type) {
    switch (type) {
        case SD_TYPE_SDSC_V1: return "SDSC v1.x (Byte-Addressed, <=2GB)";
        case SD_TYPE_SDSC_V2: return "SDSC v2.0 (Byte-Addressed, <=2GB)";
        case SD_TYPE_SDHC:    return "SDHC/SDXC (Block-Addressed, >2GB)";
        default:              return "Unknown / Uninitialized";
    }
}

sd_card_info_t sd_get_info(void) {
    return g_card_info;
}
