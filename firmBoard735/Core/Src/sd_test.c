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


void SD_Test_ReadOnly(void)
{
    HAL_SD_CardInfoTypeDef card_info;
    char text[160];

    printf("\r\n--- SDMMC READ-ONLY TEST ---\r\n");


    /* =====================================================
     * 1. Read card information
     * ===================================================== */

    if (HAL_SD_GetCardInfo(&hsd1, &card_info) != HAL_OK)
    {
        snprintf(text, sizeof(text),
                 "SD ERROR: HAL_SD_GetCardInfo failed, ErrorCode=0x%08lX\r\n",
                 hsd1.ErrorCode);

        printf("%s", text);
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

    printf("%s", text);


    uint32_t capacity_mib = card_info.LogBlockNbr / 2048U;

    snprintf(text, sizeof(text),
             "Capacity MiB  : %lu\r\n",
             capacity_mib);

    printf("%s", text);


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

    printf("\r\nReading raw sector 0...\r\n");

    if (HAL_SD_ReadBlocks(&hsd1,
                          sd_sector,
                          0,
                          1,
                          2000) != HAL_OK)
    {
        snprintf(text, sizeof(text),
                 "SD ERROR: sector read failed, ErrorCode=0x%08lX\r\n",
                 hsd1.ErrorCode);

        printf("%s", text);
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
            printf("SD ERROR: card did not return to TRANSFER state\r\n");
            return;
        }
    }


    printf("Sector 0 read OK\r\n");


    /* =====================================================
     * 4. Hex dump first 64 bytes
     * ===================================================== */

    printf("\r\nSector 0 - first 64 bytes:\r\n");

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

        printf("%s", text);
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

    printf("%s", text);


    if ((sd_sector[510] == 0x55) &&
        (sd_sector[511] == 0xAA))
    {
        printf("Boot-sector signature 55 AA found\r\n");
    }
    else
    {
        printf("No 55 AA signature - raw read still completed successfully\r\n");
    }

    SD_CheckFilesystem();

    printf("--- SDMMC READ TEST PASSED ---\r\n\r\n");
}
static void SD_CheckFilesystem(void)
{
    char text[128];

    /* Sector 0 is already readable */
    if (HAL_SD_ReadBlocks(&hsd1,
                          sd_sector,
                          0,
                          1,
                          2000) != HAL_OK)
    {
        printf("FS CHECK: cannot read sector 0\r\n");
        return;
    }

    /* Check boot signature */
    if ((sd_sector[510] != 0x55) ||
        (sd_sector[511] != 0xAA))
    {
        printf("FS CHECK: no 55 AA signature\r\n");
        return;
    }

    /*
     * First MBR partition entry begins at byte 446.
     */
    uint8_t partition_type = sd_sector[450];

    uint32_t start_lba =
          ((uint32_t)sd_sector[454])
        | ((uint32_t)sd_sector[455] << 8)
        | ((uint32_t)sd_sector[456] << 16)
        | ((uint32_t)sd_sector[457] << 24);

    snprintf(text, sizeof(text),
             "Partition 1: type=0x%02X, start LBA=%lu\r\n",
             partition_type,
             start_lba);

    printf("%s", text);

    /*
     * If start_lba is zero, sector 0 may itself be the
     * filesystem boot sector ("superfloppy" layout).
     */
    if (start_lba != 0)
    {
        if (HAL_SD_ReadBlocks(&hsd1,
                              sd_sector,
                              start_lba,
                              1,
                              2000) != HAL_OK)
        {
            printf("FS CHECK: cannot read partition boot sector\r\n");
            return;
        }
    }

    printf("Filesystem: ");

    /*
     * exFAT OEM name is at bytes 3..10.
     */
    if (memcmp(&sd_sector[3], "EXFAT   ", 8) == 0)
    {
        printf("exFAT\r\n");
    }

    /*
     * FAT12/FAT16 filesystem type string is normally
     * at bytes 54..61.
     */
    else if (memcmp(&sd_sector[54], "FAT12   ", 8) == 0)
    {
        printf("FAT12\r\n");
    }
    else if (memcmp(&sd_sector[54], "FAT16   ", 8) == 0)
    {
        printf("FAT16\r\n");
    }

    /*
     * FAT32 filesystem type string is normally
     * at bytes 82..89.
     */
    else if (memcmp(&sd_sector[82], "FAT32   ", 8) == 0)
    {
        printf("FAT32\r\n");
    }
    else
    {
        printf("unknown / not recognized\r\n");

        printf("Boot sector first 16 bytes: ");

        for (uint32_t i = 0; i < 16; i++)
        {
            snprintf(text, sizeof(text), "%02X ", sd_sector[i]);
            printf("%s", text);
        }

        printf("\r\n");
    }
}


