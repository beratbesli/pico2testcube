#include "sd_spi.h"
#include <stdio.h>
#include <string.h>

#define SD_SECTOR_SIZE       512
#define SD_INIT_BAUD         250000    // 250 kHz for initial handshake
#define SD_FAST_BAUD         20000000  // 20 MHz high-speed SPI

// SD SPI Commands
#define CMD0_GO_IDLE_STATE           0x40
#define CMD8_SEND_IF_COND            0x48
#define CMD16_SET_BLOCKLEN           0x50
#define CMD17_READ_SINGLE_BLOCK      0x51
#define CMD24_WRITE_SINGLE_BLOCK     0x58
#define CMD55_APP_CMD                0x77
#define CMD58_READ_OCR               0x7A
#define ACMD41_SD_SEND_OP_COND       0x69

static sd_card_info_t g_card_info = {
    .initialized = false,
    .type = SD_TYPE_UNKNOWN,
    .capacity_sectors = 0,
    .spi_baudrate = SD_INIT_BAUD
};

static inline void sd_cs_select(void) {
    gpio_put(SD_PIN_CS, 0);
    sleep_us(2);
}

static inline void sd_cs_deselect(void) {
    gpio_put(SD_PIN_CS, 1);
    sleep_us(2);
    // Send 8 clocks (0xFF) to release MISO line
    uint8_t dummy = 0xFF;
    spi_write_blocking(SD_SPI_PORT, &dummy, 1);
}

static inline uint8_t sd_spi_transfer_byte(uint8_t tx) {
    uint8_t rx = 0xFF;
    spi_write_read_blocking(SD_SPI_PORT, &tx, &rx, 1);
    return rx;
}

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

static uint8_t sd_send_command(uint8_t cmd, uint32_t arg, uint8_t crc) {
    uint8_t packet[6];
    packet[0] = cmd;
    packet[1] = (uint8_t)(arg >> 24);
    packet[2] = (uint8_t)(arg >> 16);
    packet[3] = (uint8_t)(arg >> 8);
    packet[4] = (uint8_t)(arg);
    packet[5] = crc;

    sd_wait_ready(100);
    spi_write_blocking(SD_SPI_PORT, packet, 6);

    // Wait for response token (MSB must be 0)
    for (int i = 0; i < 16; i++) {
        uint8_t res = sd_spi_transfer_byte(0xFF);
        if ((res & 0x80) == 0) {
            return res;
        }
    }
    return 0xFF; // Timeout
}

bool sd_spi_init(void) {
    memset(&g_card_info, 0, sizeof(g_card_info));
    g_card_info.type = SD_TYPE_UNKNOWN;

    // 1. Initialize GPIO pins
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_pull_up(SD_PIN_MISO);

    gpio_init(SD_PIN_CS);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);
    gpio_put(SD_PIN_CS, 1);

    // 2. Initialize SPI at low frequency (250 kHz)
    spi_init(SD_SPI_PORT, SD_INIT_BAUD);
    spi_set_format(SD_SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    // 3. Power-up sequence: At least 74 dummy clock cycles with CS HIGH
    gpio_put(SD_PIN_CS, 1);
    sleep_ms(20);
    for (int i = 0; i < 16; i++) {
        sd_spi_transfer_byte(0xFF);
    }

    // 4. Enter SPI Mode: CMD0 (GO_IDLE_STATE)
    sd_cs_select();
    uint8_t r1 = 0xFF;
    for (int attempt = 0; attempt < 20; attempt++) {
        r1 = sd_send_command(CMD0_GO_IDLE_STATE, 0, 0x95);
        if (r1 == 0x01) break;
        sleep_ms(5);
    }
    sd_cs_deselect();

    if (r1 != 0x01) {
        printf("[SD] CMD0 failed! R1: 0x%02X\n", r1);
        return false;
    }

    // 5. Check Interface Condition: CMD8 (Voltage 2.7-3.6V, Check Pattern 0xAA)
    sd_cs_select();
    r1 = sd_send_command(CMD8_SEND_IF_COND, 0x000001AA, 0x87);
    bool is_v2 = false;

    if (r1 == 0x01) {
        // SD v2.0+ Card - Read 4-byte R7 payload
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) r7[i] = sd_spi_transfer_byte(0xFF);
        if (r7[3] == 0xAA) {
            is_v2 = true;
        }
    }
    sd_cs_deselect();

    // 6. Initialize Card: ACMD41 loop
    uint32_t hcs = is_v2 ? (1UL << 30) : 0;
    uint64_t start_time = time_us_64();
    bool ready = false;

    while ((time_us_64() - start_time) < 2000000ULL) { // 2 second timeout
        sd_cs_select();
        sd_send_command(CMD55_APP_CMD, 0, 0x65);
        sd_cs_deselect();

        sd_cs_select();
        r1 = sd_send_command(ACMD41_SD_SEND_OP_COND, hcs, 0x77);
        sd_cs_deselect();

        if (r1 == 0x00) {
            ready = true;
            break;
        }
        sleep_ms(10);
    }

    if (!ready) {
        printf("[SD] ACMD41 initialization timeout! R1: 0x%02X\n", r1);
        return false;
    }

    // 7. Check Card Type (SDSC vs SDHC via CMD58 Read OCR)
    if (is_v2) {
        sd_cs_select();
        r1 = sd_send_command(CMD58_READ_OCR, 0, 0xFD);
        if (r1 == 0x00) {
            uint8_t ocr[4];
            for (int i = 0; i < 4; i++) ocr[i] = sd_spi_transfer_byte(0xFF);
            if (ocr[0] & 0x40) {
                g_card_info.type = SD_TYPE_SDHC;
            } else {
                g_card_info.type = SD_TYPE_SDSC_V2;
            }
        }
        sd_cs_deselect();
    } else {
        g_card_info.type = SD_TYPE_SDSC_V1;
    }

    // 8. For SDSC: Set block length to 512 bytes (CMD16)
    if (g_card_info.type != SD_TYPE_SDHC) {
        sd_cs_select();
        r1 = sd_send_command(CMD16_SET_BLOCKLEN, SD_SECTOR_SIZE, 0xFF);
        sd_cs_deselect();
        if (r1 != 0x00) {
            printf("[SD] CMD16 SET_BLOCKLEN failed! R1: 0x%02X\n", r1);
            return false;
        }
    }

    // 9. Switch SPI to High-Speed (20 MHz)
    spi_set_baudrate(SD_SPI_PORT, SD_FAST_BAUD);
    g_card_info.spi_baudrate = SD_FAST_BAUD;
    g_card_info.initialized = true;

    printf("[SD] Init OK! Type: %s | High-Speed SPI: %u MHz\n", 
           sd_type_string(g_card_info.type), g_card_info.spi_baudrate / 1000000);

    return true;
}

bool sd_read_sector(uint32_t sector_lba, uint8_t *buffer) {
    if (!g_card_info.initialized || buffer == NULL) return false;

    // SDSC uses byte addressing, SDHC uses sector (LBA) addressing
    uint32_t addr = (g_card_info.type == SD_TYPE_SDHC) ? sector_lba : (sector_lba * SD_SECTOR_SIZE);

    sd_cs_select();
    uint8_t r1 = sd_send_command(CMD17_READ_SINGLE_BLOCK, addr, 0xFF);
    if (r1 != 0x00) {
        sd_cs_deselect();
        return false;
    }

    // Wait for Data Token (0xFE)
    uint64_t start = time_us_64();
    bool token_found = false;
    while ((time_us_64() - start) < 150000ULL) { // 150ms timeout
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
    uint8_t r1 = sd_send_command(CMD24_WRITE_SINGLE_BLOCK, addr, 0xFF);
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
    if (!sd_wait_ready(300)) { // 300ms timeout
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
