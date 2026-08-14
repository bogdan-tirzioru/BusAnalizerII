#include "hyperram_test.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

extern OSPI_HandleTypeDef hospi1;

#define HYPERRAM_CR0_ADDRESS          0x00001000U
#define HYPERRAM_CR1_ADDRESS          0x00001002U

#define HYPERRAM_STRESS_BASE_ADDRESS  0x00001000U
#define HYPERRAM_STRESS_BLOCK_BYTES   512U
#define HYPERRAM_STRESS_READ1_BYTES   496U
#define HYPERRAM_STRESS_READ2_BYTES   16U
#define HYPERRAM_STRESS_BLOCKS        256U
#define HYPERRAM_STRESS_MAX_DETAILS   12U

_Static_assert(
    HYPERRAM_STRESS_READ1_BYTES + HYPERRAM_STRESS_READ2_BYTES ==
        HYPERRAM_STRESS_BLOCK_BYTES,
    "HyperRAM stress read split must cover one complete write block");

static uint8_t stress_tx[HYPERRAM_STRESS_BLOCK_BYTES];
static uint8_t stress_rx[HYPERRAM_STRESS_BLOCK_BYTES];

static HAL_StatusTypeDef HyperRAM_Stress_ReadRegister16(
        uint32_t address,
        uint16_t *value,
        uint8_t raw[2])
{
    OSPI_HyperbusCmdTypeDef cmd = {0};

    if ((value == NULL) || (raw == NULL))
    {
        return HAL_ERROR;
    }

    cmd.AddressSpace = HAL_OSPI_REGISTER_ADDRESS_SPACE;
    cmd.AddressSize  = HAL_OSPI_ADDRESS_32_BITS;
    cmd.Address      = address;
    cmd.DQSMode      = HAL_OSPI_DQS_ENABLE;
    cmd.NbData       = 2U;

    if (HAL_OSPI_HyperbusCmd(
            &hospi1,
            &cmd,
            HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
    {
        return HAL_ERROR;
    }

    if (HAL_OSPI_Receive(
            &hospi1,
            raw,
            HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
    {
        return HAL_ERROR;
    }

    *value = ((uint16_t)raw[0] << 8) | raw[1];
    return HAL_OK;
}

static HAL_StatusTypeDef HyperRAM_Stress_Write(
        uint32_t address,
        const uint8_t *data,
        uint32_t size)
{
    OSPI_HyperbusCmdTypeDef cmd = {0};

    cmd.AddressSpace = HAL_OSPI_MEMORY_ADDRESS_SPACE;
    cmd.AddressSize  = HAL_OSPI_ADDRESS_32_BITS;
    cmd.Address      = address;
    cmd.DQSMode      = HAL_OSPI_DQS_ENABLE;
    cmd.NbData       = size;

    if (HAL_OSPI_HyperbusCmd(
            &hospi1,
            &cmd,
            HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
    {
        return HAL_ERROR;
    }

    return HAL_OSPI_Transmit(
        &hospi1,
        (uint8_t *)(uintptr_t)data,
        HAL_OSPI_TIMEOUT_DEFAULT_VALUE);
}

static HAL_StatusTypeDef HyperRAM_Stress_Read(
        uint32_t address,
        uint8_t *data,
        uint32_t size)
{
    OSPI_HyperbusCmdTypeDef cmd = {0};

    cmd.AddressSpace = HAL_OSPI_MEMORY_ADDRESS_SPACE;
    cmd.AddressSize  = HAL_OSPI_ADDRESS_32_BITS;
    cmd.Address      = address;
    cmd.DQSMode      = HAL_OSPI_DQS_ENABLE;
    cmd.NbData       = size;

    if (HAL_OSPI_HyperbusCmd(
            &hospi1,
            &cmd,
            HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
    {
        return HAL_ERROR;
    }

    return HAL_OSPI_Receive(
        &hospi1,
        data,
        HAL_OSPI_TIMEOUT_DEFAULT_VALUE);
}

static void HyperRAM_Stress_FillBlock(uint32_t block)
{
    for (uint32_t record = 0U; record < 32U; record++)
    {
        uint32_t counter = (block * 32U) + record;
        uint32_t offset = record * 16U;

        stress_tx[offset + 0U] = (uint8_t)counter;
        stress_tx[offset + 1U] = (uint8_t)(counter >> 8);
        stress_tx[offset + 2U] = (uint8_t)(counter >> 16);
        stress_tx[offset + 3U] = (uint8_t)(counter >> 24);
        stress_tx[offset + 4U] = 0x11U;
        stress_tx[offset + 5U] = 0x22U;
        stress_tx[offset + 6U] = 0x33U;
        stress_tx[offset + 7U] = 0x44U;

        stress_tx[offset + 8U]  = (uint8_t)(0xA0U + (record & 0x0FU));
        stress_tx[offset + 9U]  = (uint8_t)block;
        stress_tx[offset + 10U] = (uint8_t)(block >> 8);
        stress_tx[offset + 11U] = (uint8_t)(record);
        stress_tx[offset + 12U] = 0x5AU;
        stress_tx[offset + 13U] = 0xC3U;
        stress_tx[offset + 14U] = 0x69U;
        stress_tx[offset + 15U] = 0x96U;
    }
}

void HyperRAM_Test_ConfigRegisters(void)
{
    uint16_t cr0 = 0U;
    uint16_t cr1 = 0U;
    uint8_t raw0[2] = {0U, 0U};
    uint8_t raw1[2] = {0U, 0U};

    printf("\r\n--- HYPERRAM CONFIG REGISTER TEST ---\r\n");

    HAL_StatusTypeDef st0 = HyperRAM_Stress_ReadRegister16(
        HYPERRAM_CR0_ADDRESS, &cr0, raw0);
    HAL_StatusTypeDef st1 = HyperRAM_Stress_ReadRegister16(
        HYPERRAM_CR1_ADDRESS, &cr1, raw1);

    printf("CR0 address : 0x%08lX status=%d value=0x%04X raw=%02X %02X\r\n",
           (unsigned long)HYPERRAM_CR0_ADDRESS,
           (int)st0,
           cr0,
           raw0[0], raw0[1]);
    printf("CR1 address : 0x%08lX status=%d value=0x%04X raw=%02X %02X\r\n",
           (unsigned long)HYPERRAM_CR1_ADDRESS,
           (int)st1,
           cr1,
           raw1[0], raw1[1]);

    printf("--- END HYPERRAM CONFIG REGISTER TEST ---\r\n");
}

void HyperRAM_Test_BurstStress(void)
{
    uint32_t write_errors = 0U;
    uint32_t read_errors = 0U;
    uint32_t bad_blocks = 0U;
    uint32_t byte_errors = 0U;
    uint32_t detail_count = 0U;

    printf("\r\n--- HYPERRAM 512-BYTE BURST STRESS ---\r\n");
    printf("Blocks       : %u\r\n", HYPERRAM_STRESS_BLOCKS);
    printf("Write size   : %u bytes\r\n", HYPERRAM_STRESS_BLOCK_BYTES);
    printf("Read split   : %u + %u bytes\r\n",
           HYPERRAM_STRESS_READ1_BYTES,
           HYPERRAM_STRESS_READ2_BYTES);

    for (uint32_t block = 0U; block < HYPERRAM_STRESS_BLOCKS; block++)
    {
        uint32_t address =
            HYPERRAM_STRESS_BASE_ADDRESS +
            (block * HYPERRAM_STRESS_BLOCK_BYTES);

        HyperRAM_Stress_FillBlock(block);
        memset(stress_rx, 0, sizeof(stress_rx));

        if (HyperRAM_Stress_Write(
                address,
                stress_tx,
                HYPERRAM_STRESS_BLOCK_BYTES) != HAL_OK)
        {
            write_errors++;
            bad_blocks++;
            continue;
        }

        if (HyperRAM_Stress_Read(
                address,
                stress_rx,
                HYPERRAM_STRESS_READ1_BYTES) != HAL_OK)
        {
            read_errors++;
            bad_blocks++;
            continue;
        }

        if (HyperRAM_Stress_Read(
                address + HYPERRAM_STRESS_READ1_BYTES,
                &stress_rx[HYPERRAM_STRESS_READ1_BYTES],
                HYPERRAM_STRESS_READ2_BYTES) != HAL_OK)
        {
            read_errors++;
            bad_blocks++;
            continue;
        }

        bool block_bad = false;

        for (uint32_t i = 0U; i < HYPERRAM_STRESS_BLOCK_BYTES; i++)
        {
            if (stress_rx[i] != stress_tx[i])
            {
                byte_errors++;
                block_bad = true;

                if (detail_count < HYPERRAM_STRESS_MAX_DETAILS)
                {
                    printf(
                        "Burst mismatch block=%lu addr=0x%08lX byte=%lu "
                        "wrote=%02X read=%02X\r\n",
                        (unsigned long)block,
                        (unsigned long)address,
                        (unsigned long)i,
                        stress_tx[i],
                        stress_rx[i]);
                    detail_count++;
                }
            }
        }

        if (block_bad)
        {
            bad_blocks++;
        }
    }

    printf("Blocks checked : %u\r\n", HYPERRAM_STRESS_BLOCKS);
    printf("Bad blocks     : %lu\r\n", (unsigned long)bad_blocks);
    printf("Byte errors    : %lu\r\n", (unsigned long)byte_errors);
    printf("Write HAL err  : %lu\r\n", (unsigned long)write_errors);
    printf("Read HAL err   : %lu\r\n", (unsigned long)read_errors);

    if ((bad_blocks == 0U) &&
        (write_errors == 0U) &&
        (read_errors == 0U))
    {
        printf("HYPERRAM BURST STRESS: PASS\r\n");
    }
    else
    {
        printf("HYPERRAM BURST STRESS: FAIL\r\n");
    }

    printf("--- END HYPERRAM 512-BYTE BURST STRESS ---\r\n");
}
