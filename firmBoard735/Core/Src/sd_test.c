#include "sd_test.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

/* CubeMX owns these handles in main.c */
extern SD_HandleTypeDef hsd1;
extern UART_HandleTypeDef huart1;


/*
 * Aligned buffer is a good habit on STM32H7.
 * It will also help later if we move to DMA/cache operation.
 */
static uint8_t sd_sector[512] __attribute__((aligned(32)));


static void uart_print(const char *text)
{
    HAL_UART_Transmit(&huart1,
                      (uint8_t *)text,
                      strlen(text),
                      HAL_MAX_DELAY);
}


void SD_Test_ReadOnly(void)
{
    HAL_SD_CardInfoTypeDef card_info;
    char text[160];

    uart_print("\r\n--- SDMMC READ-ONLY TEST ---\r\n");


    /* =====================================================
     * 1. Read card information
     * ===================================================== */

    if (HAL_SD_GetCardInfo(&hsd1, &card_info) != HAL_OK)
    {
        snprintf(text, sizeof(text),
                 "SD ERROR: HAL_SD_GetCardInfo failed, ErrorCode=0x%08lX\r\n",
                 hsd1.ErrorCode);

        uart_print(text);
        return;
    }




    snprintf(text, sizeof(text),
             "CardType      : %lu\r\n"
             "CardVersion   : %lu\r\n"
             "RCA           : 0x%04lX\r\n"
             "Block count   : %lu\r\n"
             "Block size    : %lu bytes\r\n",
             card_info.CardType,
             card_info.CardVersion,
             card_info.RelCardAdd,
             card_info.LogBlockNbr,
             card_info.LogBlockSize);

    uart_print(text);


    uint32_t capacity_mib = card_info.LogBlockNbr / 2048U;

    snprintf(text, sizeof(text),
             "Capacity MiB  : %lu\r\n",
             capacity_mib);

    uart_print(text);


    /* =====================================================
     * 2. Clear destination buffer
     * ===================================================== */

    memset(sd_sector, 0, sizeof(sd_sector));


    /* =====================================================
     * 3. Read RAW sector 0
     *
     *    Block address = 0
     *    Number blocks = 1
     *    Block size    = 512 bytes
     * ===================================================== */

    uart_print("\r\nReading raw sector 0...\r\n");

    if (HAL_SD_ReadBlocks(&hsd1,
                          sd_sector,
                          0,
                          1,
                          2000) != HAL_OK)
    {
        snprintf(text, sizeof(text),
                 "SD ERROR: sector read failed, ErrorCode=0x%08lX\r\n",
                 hsd1.ErrorCode);

        uart_print(text);
        return;
    }


    /*
     * Wait until card returns to TRANSFER state.
     * Usually immediate for a read, but it makes the test robust.
     */

    uint32_t timeout = HAL_GetTick();

    while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
    {
        if ((HAL_GetTick() - timeout) > 1000)
        {
            uart_print("SD ERROR: card did not return to TRANSFER state\r\n");
            return;
        }
    }


    uart_print("Sector 0 read OK\r\n");


    /* =====================================================
     * 4. Hex dump first 64 bytes
     * ===================================================== */

    uart_print("\r\nSector 0 - first 64 bytes:\r\n");

    for (uint32_t i = 0; i < 64; i += 16)
    {
        int len = snprintf(text,
                           sizeof(text),
                           "%04lX : ",
                           i);

        for (uint32_t j = 0; j < 16; j++)
        {
            len += snprintf(&text[len],
                            sizeof(text) - len,
                            "%02X ",
                            sd_sector[i + j]);
        }

        snprintf(&text[len],
                 sizeof(text) - len,
                 "\r\n");

        uart_print(text);
    }


    /* =====================================================
     * 5. Check standard boot-sector signature
     *
     * This doesn't prove a particular filesystem,
     * but bytes 510/511 are commonly 55 AA for MBR/VBR.
     * ===================================================== */

    snprintf(text, sizeof(text),
             "\r\nSector signature [510:511] = %02X %02X\r\n",
             sd_sector[510],
             sd_sector[511]);

    uart_print(text);


    if ((sd_sector[510] == 0x55) &&
        (sd_sector[511] == 0xAA))
    {
        uart_print("Boot-sector signature 55 AA found\r\n");
    }
    else
    {
        uart_print("No 55 AA signature - raw read still completed successfully\r\n");
    }


    uart_print("--- SDMMC READ TEST PASSED ---\r\n\r\n");
}
