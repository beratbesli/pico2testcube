#ifndef SD_SPI_H
#define SD_SPI_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

// Hardware Pin Definitions for SPI0 on Pico 2
#define SD_PIN_MISO   16
#define SD_PIN_CS     17
#define SD_PIN_SCK    18
#define SD_PIN_MOSI   19
#define SD_SPI_PORT   spi0

// SD Card Types
typedef enum {
    SD_TYPE_UNKNOWN = 0,
    SD_TYPE_SDSC_V1,
    SD_TYPE_SDSC_V2,
    SD_TYPE_SDHC
} sd_card_type_t;

// SD Card Information
typedef struct {
    bool           initialized;
    sd_card_type_t type;
    uint32_t       capacity_sectors;
    uint32_t       spi_baudrate;
} sd_card_info_t;

// API Functions
bool           sd_spi_init(void);
bool           sd_read_sector(uint32_t sector_lba, uint8_t *buffer);
bool           sd_write_sector(uint32_t sector_lba, const uint8_t *buffer);
const char*    sd_type_string(sd_card_type_t type);
sd_card_info_t sd_get_info(void);

#endif // SD_SPI_H
