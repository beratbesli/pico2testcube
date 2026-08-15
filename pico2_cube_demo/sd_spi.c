#include "sd_spi.h"
#include <stdio.h>
#include <string.h>

#define SD_SECTOR_SIZE       512
#define SD_INIT_BAUD         100000    // 100 kHz for initial handshake (<= 400 kHz standard)
#define SD_FAST_BAUD         20000000  // 20 MHz high-speed SPI (after successful init)

// SD SPI Commands (Command index with start bit 0x40)
#define CMD0_GO_IDLE_STATE           0x40  // arg: 0x00000000, crc: 0x95
#define CMD1_SEND_OP_COND            0x41  // arg: 0x00000000, crc: 0xF9 (MMC fallback)
#define CMD8_SEND_IF_COND            0x48  // arg: 0x000001AA, crc: 0x87
#define CMD16_SET_BLOCKLEN           0x50  // arg: 0x00000200 (512), crc: 0xFF
#define CMD17_READ_SINGLE_BLOCK      0x51
#define CMD24_WRITE_SINGLE_BLOCK     0x58
#define CMD55_APP_CMD                0x77  // arg: 0x00000000, crc: 0x65
#define CMD58_READ_OCR               0x7A  // arg: 0x00000000, crc: 0xFD
#define ACMD41_SD_SEND_OP_COND       0x69  // arg: 0x40000000 (HCS=1) or 0x00000000 (HCS=0)

static sd_card_info_t g_card_info = {
    .initialized = false,
    .type = SD_TYPE_UNKNOWN,
    .capacity_sectors = 0,
    .spi_baudrate = SD_INIT_BAUD
};

// Select SD Card (CS = LOW)
static inline void sd_cs_select(void) {
    gpio_put(SD_PIN_CS, 0);
    sleep_us(5);
}

// Deselect SD Card (CS = HIGH) and provide 8 clock cycles for card cleanup
static inline void sd_cs_deselect(void) {
    gpio_put(SD_PIN_CS, 1);
    sleep_us(5);
    uint8_t dummy = 0xFF;
    spi_write_blocking(SD_SPI_PORT, &dummy, 1);
}

// Single-byte full-duplex transfer over SPI
static inline uint8_t sd_spi_transfer_byte(uint8_t tx) {
    uint8_t rx = 0xFF;
    spi_write_read_blocking(SD_SPI_PORT, &tx, &rx, 1);
    return rx;
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

// Send a raw 6-byte SPI command packet and wait for R1 response
static uint8_t sd_send_command_raw(uint8_t cmd, uint32_t arg, uint8_t crc) {
    uint8_t packet[6];
    packet[0] = cmd;
    packet[1] = (uint8_t)(arg >> 24);
    packet[2] = (uint8_t)(arg >> 16);
    packet[3] = (uint8_t)(arg >> 8);
    packet[4] = (uint8_t)(arg);
    packet[5] = crc;

    // Wait for card ready before transmitting
    sd_wait_ready(200);

    // Transmit 6-byte command packet
    spi_write_blocking(SD_SPI_PORT, packet, 6);

    // Wait for response token (MSB must be 0 for valid R1)
    // Timeout up to 64 bytes
    for (int i = 0; i < 64; i++) {
        uint8_t res = sd_spi_transfer_byte(0xFF);
        if ((res & 0x80) == 0) {
            return res;
        }
    }
    return 0xFF; // Timeout
}

// Send standard command with automatic CS framing
static uint8_t sd_send_command(uint8_t cmd, uint32_t arg, uint8_t crc) {
    sd_cs_select();
    uint8_t r1 = sd_send_command_raw(cmd, arg, crc);
    sd_cs_deselect();
    return r1;
}

// Send ACMD (CMD55 followed immediately by ACMD<n>) with proper CS framing
static uint8_t sd_send_acmd(uint8_t acmd, uint32_t arg, uint8_t crc, uint8_t *out_cmd55_r1) {
    // 1. Send CMD55 (APP_CMD)
    sd_cs_select();
    uint8_t r1_55 = sd_send_command_raw(CMD55_APP_CMD, 0, 0x65);
    sd_cs_deselect();

    if (out_cmd55_r1) {
        *out_cmd55_r1 = r1_55;
    }

    if (r1_55 > 0x01) {
        return r1_55; // CMD55 rejected
    }

    // 2. Send ACMD (e.g. ACMD41)
    sd_cs_select();
    uint8_t r1_acmd = sd_send_command_raw(acmd, arg, crc);
    sd_cs_deselect();

    return r1_acmd;
}

bool sd_spi_init(void) {
    memset(&g_card_info, 0, sizeof(g_card_info));
    g_card_info.type = SD_TYPE_UNKNOWN;

    printf("[SD] --- Starting MicroSD Card Initialization ---\n");

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
    printf("[SD] Hardware SPI0 initialized at %u Hz (<= 400 kHz mode)\n", actual_baud);

    // 3. Power-Up Settling Delay (At least 250ms for card internal POR)
    printf("[SD] Power settling delay (300ms)...\n");
    sleep_ms(300);

    // 4. Power-Up Sequence: Send at least 128 dummy clock pulses with CS HIGH
    gpio_put(SD_PIN_CS, 1);
    for (int i = 0; i < 20; i++) { // 20 bytes = 160 clocks
        sd_spi_transfer_byte(0xFF);
    }
    sleep_ms(5);

    // 5. Enter SPI Mode: CMD0 (GO_IDLE_STATE)
    printf("[SD] Sending CMD0 (Reset to SPI Mode)...\n");
    uint8_t r1 = 0xFF;
    for (int attempt = 1; attempt <= 30; attempt++) {
        r1 = sd_send_command(CMD0_GO_IDLE_STATE, 0, 0x95);
        if (r1 == 0x01) {
            printf("[SD] CMD0 Success on attempt %d! R1: 0x01 (In Idle State)\n", attempt);
            break;
        }
        sleep_ms(10);
    }

    if (r1 != 0x01) {
        printf("[SD] ERROR: CMD0 failed after 30 attempts! Last R1: 0x%02X\n", r1);
        return false;
    }

    // 6. Check Interface Condition: CMD8 (Voltage 2.7-3.6V, Check Pattern 0xAA)
    printf("[SD] Sending CMD8 (Check Voltage & Spec Version)...\n");
    sd_cs_select();
    r1 = sd_send_command_raw(CMD8_SEND_IF_COND, 0x000001AA, 0x87);
    bool is_v2 = false;

    if (r1 == 0x01) {
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) r7[i] = sd_spi_transfer_byte(0xFF);
        printf("[SD] CMD8 Response: 0x01 | R7: %02X %02X %02X %02X\n", r7[0], r7[1], r7[2], r7[3]);
        if (r7[3] == 0xAA) {
            is_v2 = true;
            printf("[SD] Card conforms to SD Spec v2.0+ (Pattern 0xAA matched)\n");
        }
    } else {
        printf("[SD] CMD8 returned 0x%02X -> Card is SD v1.x or MMC\n", r1);
    }
    sd_cs_deselect();

    // 7. Initialize Card: ACMD41 Retry Loop
    // For SDSC cards (<= 2GB), HCS=0 (0x00000000) or HCS=1 (0x40000000) is used.
    // We start with HCS=1 if v2; if it stays idle, we dynamically try HCS=0.
    printf("[SD] Starting ACMD41 Initialization Loop (Timeout: 2500ms)...\n");
    uint32_t hcs_arg = is_v2 ? 0x40000000UL : 0x00000000UL;
    uint8_t  hcs_crc = (hcs_arg != 0) ? 0x77 : 0xE5;

    uint64_t start_time = time_us_64();
    bool ready = false;
    int attempt_count = 0;
    uint8_t last_cmd55_r1 = 0xFF;
    uint8_t last_acmd41_r1 = 0xFF;
    bool switched_to_sdsc_hcs0 = false;

    while ((time_us_64() - start_time) < 2500000ULL) { // 2.5 second timeout
        attempt_count++;

        // After 100 attempts (~500ms) with HCS=1, if card is still in Idle (0x01),
        // fallback to HCS=0 (Standard Capacity SDSC mode)
        if (attempt_count > 80 && is_v2 && !switched_to_sdsc_hcs0 && last_acmd41_r1 == 0x01) {
            printf("[SD] Card still idle with HCS=1. Switching ACMD41 to SDSC mode (HCS=0)...\n");
            hcs_arg = 0x00000000UL;
            hcs_crc = 0xE5;
            switched_to_sdsc_hcs0 = true;
        }

        // Send CMD55 + ACMD41 pair
        last_acmd41_r1 = sd_send_acmd(ACMD41_SD_SEND_OP_COND, hcs_arg, hcs_crc, &last_cmd55_r1);

        // Log progress periodically or on key events
        if (attempt_count == 1 || (attempt_count % 50 == 0)) {
            uint32_t elapsed_ms = (uint32_t)((time_us_64() - start_time) / 1000ULL);
            printf("[SD] Attempt #%d (%u ms) -> CMD55 R1: 0x%02X | ACMD41 R1: 0x%02X (HCS=0x%08lX)\n",
                   attempt_count, elapsed_ms, last_cmd55_r1, last_acmd41_r1, (unsigned long)hcs_arg);
        }

        // R1 == 0x00 means card has left idle state and is ready!
        if (last_acmd41_r1 == 0x00) {
            uint32_t elapsed_ms = (uint32_t)((time_us_64() - start_time) / 1000ULL);
            printf("[SD] ACMD41 SUCCESS on attempt #%d (took %u ms)! Card is READY (R1: 0x00)\n", 
                   attempt_count, elapsed_ms);
            ready = true;
            break;
        }

        sleep_ms(5); // Paced retry delay
    }

    // Fallback: If ACMD41 failed, try legacy CMD1 (MMC / older SD cards)
    if (!ready) {
        printf("[SD] ACMD41 timed out. Trying legacy CMD1 fallback...\n");
        for (int cmd1_attempt = 1; cmd1_attempt <= 100; cmd1_attempt++) {
            last_acmd41_r1 = sd_send_command(CMD1_SEND_OP_COND, 0, 0xF9);
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
        printf("[SD] ERROR: ACMD41 timeout after %d attempts, total time: %u ms (CMD55: 0x%02X, ACMD41: 0x%02X)\n",
               attempt_count, total_time_ms, last_cmd55_r1, last_acmd41_r1);
        return false;
    }

    // 8. Determine Card Type (SDSC vs SDHC via CMD58 Read OCR)
    if (is_v2) {
        sd_cs_select();
        r1 = sd_send_command_raw(CMD58_READ_OCR, 0, 0xFD);
        if (r1 == 0x00) {
            uint8_t ocr[4];
            for (int i = 0; i < 4; i++) ocr[i] = sd_spi_transfer_byte(0xFF);
            printf("[SD] OCR Register: %02X %02X %02X %02X\n", ocr[0], ocr[1], ocr[2], ocr[3]);
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

    // 9. For SDSC: Explicitly set block length to 512 bytes (CMD16)
    if (g_card_info.type != SD_TYPE_SDHC) {
        printf("[SD] Setting Block Length to 512 bytes (CMD16)...\n");
        r1 = sd_send_command(CMD16_SET_BLOCKLEN, SD_SECTOR_SIZE, 0xFF);
        if (r1 != 0x00) {
            printf("[SD] WARNING: CMD16 SET_BLOCKLEN returned 0x%02X\n", r1);
        } else {
            printf("[SD] Block length configured: 512 Bytes (LBA Sector Mode)\n");
        }
    }

    // 10. Switch SPI to High-Speed (20 MHz) ONLY after full initialization
    uint fast_baud = spi_set_baudrate(SD_SPI_PORT, SD_FAST_BAUD);
    g_card_info.spi_baudrate = fast_baud;
    g_card_info.initialized = true;

    printf("[SD] *** Initialization Complete! Switching to High-Speed SPI: %u MHz ***\n\n",
           g_card_info.spi_baudrate / 1000000);

    return true;
}

bool sd_read_sector(uint32_t sector_lba, uint8_t *buffer) {
    if (!g_card_info.initialized || buffer == NULL) return false;

    // SDSC uses byte addressing (sector * 512), SDHC uses sector LBA directly
    uint32_t addr = (g_card_info.type == SD_TYPE_SDHC) ? sector_lba : (sector_lba * SD_SECTOR_SIZE);

    sd_cs_select();
    uint8_t r1 = sd_send_command_raw(CMD17_READ_SINGLE_BLOCK, addr, 0xFF);
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
    uint8_t r1 = sd_send_command_raw(CMD24_WRITE_SINGLE_BLOCK, addr, 0xFF);
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

    // Read Data Response Token
    uint8_t resp = sd_spi_transfer_byte(0xFF);
    if ((resp & 0x1F) != 0x05) { // 0x05 = Data accepted
        sd_cs_deselect();
        return false;
    }

    // Wait while card writes block (busy state: MISO held LOW)
    if (!sd_wait_ready(350)) { // 350ms timeout
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
