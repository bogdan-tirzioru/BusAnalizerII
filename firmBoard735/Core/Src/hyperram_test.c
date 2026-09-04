#include "hyperram_test.h"
#include "console.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

extern OSPI_HandleTypeDef hospi1;
extern UART_HandleTypeDef huart1;

#define HYPERRAM_DIR0_ADDRESS    0x00000000U
#define HYPERRAM_DIR1_ADDRESS    0x00000002U

#define HYPERRAM_TEST_REPEATS    10U


/*
 * Read one 16-bit HyperRAM register.
 *
 * Return:
 *   0 = OK
 *   1 = HAL_OSPI_HyperbusCmd() failed
 *   2 = HAL_OSPI_Receive() failed
 *
 * raw[0] and raw[1] contain the bytes exactly as received
 * from HAL_OSPI_Receive().
 */
static int HyperRAM_ReadRegister16(uint32_t address,
                                   uint16_t *value,
                                   uint8_t raw[2])
{
    OSPI_HyperbusCmdTypeDef cmd = {0};

    if ((value == NULL) || (raw == NULL))
    {
        return -1;
    }

    raw[0] = 0x00U;
    raw[1] = 0x00U;
    *value = 0x0000U;

    cmd.AddressSpace = HAL_OSPI_REGISTER_ADDRESS_SPACE;
    cmd.AddressSize  = HAL_OSPI_ADDRESS_32_BITS;
    cmd.Address      = address;
    cmd.DQSMode      = HAL_OSPI_DQS_ENABLE;
    cmd.NbData       = 2U;

    /*
     * Send HyperBus register-read command.
     */
    if (HAL_OSPI_HyperbusCmd(&hospi1,
                             &cmd,
                             HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
    {
        return 1;
    }

    /*
     * Receive the two register bytes.
     */
    if (HAL_OSPI_Receive(&hospi1,
                         raw,
                         HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
    {
        return 2;
    }

    /*
     * STM32 is little-endian.
     *
     * Keep this explicit because we also print the raw bytes.
     */
    *value = ((uint16_t)raw[0] << 8) |
             ((uint16_t)raw[1]);

    return 0;
}


void HyperRAM_Test_ID(void)
{
    char text[180];

    uint16_t id0;
    uint16_t id1;

    uint8_t raw0[2];
    uint8_t raw1[2];

    int status0;
    int status1;

    uint32_t manufacturer;
    uint32_t column_field;
    uint32_t row_field;
    uint32_t die_address;
    uint32_t device_type;

    uint32_t valid_count = 0U;


    Console_Printf("\r\n--- HYPERRAM ID REPEAT TEST ---\r\n");


    /*
     * Perform several identical reads.
     *
     * This helps distinguish:
     *
     *   - stable but incorrect data
     *   - random / unstable data
     *   - correct communication
     */
    for (uint32_t i = 0U; i < HYPERRAM_TEST_REPEATS; i++)
    {
        id0 = 0U;
        id1 = 0U;

        raw0[0] = 0U;
        raw0[1] = 0U;

        raw1[0] = 0U;
        raw1[1] = 0U;


        status0 = HyperRAM_ReadRegister16(
                     HYPERRAM_DIR0_ADDRESS,
                     &id0,
                     raw0);


        status1 = HyperRAM_ReadRegister16(
                     HYPERRAM_DIR1_ADDRESS,
                     &id1,
                     raw1);


        snprintf(text,
                 sizeof(text),

                 "%02lu: "
                 "DIR0=0x%04X [%02X %02X] "
                 "DIR1=0x%04X [%02X %02X] "
                 "status=%d/%d\r\n",

                 (unsigned long)i,

                 id0,
                 raw0[0],
                 raw0[1],

                 id1,
                 raw1[0],
                 raw1[1],

                 status0,
                 status1);

        Console_Write(text);


        /*
         * Expected basic ID fields:
         *
         * DIR0:
         *   Manufacturer = 0x1
         *   Column field = 0x8
         *   Row field    = 0x0D
         *
         * DIR1:
         *   Device type  = 0x0
         *
         * Ignore die-address bits for this basic test.
         */
        if ((status0 == 0) &&
            (status1 == 0) &&
            ((id0 & 0x3FFFU) == 0x0C81U) &&
            ((id1 & 0x000FU) == 0x0000U))
        {
            valid_count++;
        }


        HAL_Delay(10);
    }


    Console_Printf("\r\n");


    /*
     * Read once more for decoded information.
     */
    status0 = HyperRAM_ReadRegister16(
                  HYPERRAM_DIR0_ADDRESS,
                  &id0,
                  raw0);

    status1 = HyperRAM_ReadRegister16(
                  HYPERRAM_DIR1_ADDRESS,
                  &id1,
                  raw1);


    if ((status0 != 0) || (status1 != 0))
    {
        snprintf(text,
                 sizeof(text),
                 "Final ID read failed: status=%d/%d\r\n",
                 status0,
                 status1);

        Console_Write(text);
        Console_Printf("--- HYPERRAM ID TEST FAILED ---\r\n");

        return;
    }


    manufacturer = id0 & 0x000FU;

    column_field =
        (id0 >> 4) & 0x000FU;

    row_field =
        (id0 >> 8) & 0x001FU;

    die_address =
        (id0 >> 14) & 0x0003U;

    device_type =
        id1 & 0x000FU;


    snprintf(text,
             sizeof(text),

             "\r\nFinal register values:\r\n"
             "DIR0 = 0x%04X  raw=[%02X %02X]\r\n"
             "DIR1 = 0x%04X  raw=[%02X %02X]\r\n",

             id0,
             raw0[0],
             raw0[1],

             id1,
             raw1[0],
             raw1[1]);

    Console_Write(text);


    snprintf(text,
             sizeof(text),

             "Manufacturer field : 0x%lX\r\n"
             "Column field       : 0x%lX\r\n"
             "Row field          : 0x%lX\r\n"
             "Die address        : 0x%lX\r\n"
             "Device type        : 0x%lX\r\n",

             (unsigned long)manufacturer,
             (unsigned long)column_field,
             (unsigned long)row_field,
             (unsigned long)die_address,
             (unsigned long)device_type);

    Console_Write(text);


    snprintf(text,
             sizeof(text),

             "Valid reads        : %lu / %u\r\n",

             (unsigned long)valid_count,
             HYPERRAM_TEST_REPEATS);

    Console_Write(text);


    /*
     * Require all repeated reads to be valid.
     *
     * This is intentional: for a hardware bring-up test,
     * one successful transaction among unstable reads isn't
     * good enough.
     */
    if (valid_count == HYPERRAM_TEST_REPEATS)
    {
        Console_Printf("\r\nHYPERRAM ID VERIFY: PASS\r\n");
        Console_Printf("--- HYPERRAM ID TEST PASSED ---\r\n");
    }
    else
    {
        Console_Printf("\r\nHYPERRAM ID VERIFY: FAILED\r\n");
        Console_Printf("--- HYPERRAM ID TEST FAILED ---\r\n");
    }
}


void HyperRAM_Test_Scope(void)
{
    static uint8_t buffer[512];

    OSPI_HyperbusCmdTypeDef cmd = {0};

    uint32_t start;
    uint32_t count = 0;

    Console_Printf("\r\n--- HYPERRAM SCOPE TEST ---\r\n");

    start = HAL_GetTick();

    while ((HAL_GetTick() - start) < 5000U)
    {
        cmd.AddressSpace = HAL_OSPI_MEMORY_ADDRESS_SPACE;
        cmd.AddressSize  = HAL_OSPI_ADDRESS_32_BITS;
        cmd.Address      = 0x00000000U;
        cmd.DQSMode      = HAL_OSPI_DQS_ENABLE;
        cmd.NbData       = sizeof(buffer);

        if (HAL_OSPI_HyperbusCmd(&hospi1,
                                 &cmd,
                                 HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
        {
            Console_Printf("HyperbusCmd ERROR\r\n");
            return;
        }

        if (HAL_OSPI_Receive(&hospi1,
                             buffer,
                             HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
        {
            Console_Printf("Receive ERROR\r\n");
            return;
        }

        count++;
    }

    char text[80];

    snprintf(text, sizeof(text),
             "Scope transactions: %lu\r\n",
             (unsigned long)count);

    Console_Write(text);
    Console_Printf("--- HYPERRAM SCOPE TEST DONE ---\r\n");
}


void HyperRAM_Test_Memory(void)
{
    uint8_t tx[64];
    uint8_t rx[64];

    OSPI_HyperbusCmdTypeDef cmd = {0};

    Console_Printf("\r\n--- HYPERRAM 64-BYTE MEMORY TEST ---\r\n");

    for (uint32_t i = 0; i < sizeof(tx); i++)
    {
        tx[i] = (uint8_t)(i ^ 0xA5U);
        rx[i] = 0;
    }

    /* WRITE */
    cmd.AddressSpace = HAL_OSPI_MEMORY_ADDRESS_SPACE;
    cmd.AddressSize  = HAL_OSPI_ADDRESS_32_BITS;
    cmd.Address      = 0x00000000U;
    cmd.DQSMode      = HAL_OSPI_DQS_ENABLE;
    cmd.NbData       = sizeof(tx);

    if (HAL_OSPI_HyperbusCmd(&hospi1,
                             &cmd,
                             HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
    {
        Console_Printf("WRITE command ERROR\r\n");
        return;
    }
    Console_Printf("WRITE CMD : OK\r\n");
    if (HAL_OSPI_Transmit(&hospi1,
                          tx,
                          HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
    {
        Console_Printf("WRITE data ERROR\r\n");
        return;
    }
    Console_Printf("WRITE DATA: OK\r\n");
    /* READ */
    cmd.Address      = 0x00000000U;
    cmd.NbData       = sizeof(rx);

    if (HAL_OSPI_HyperbusCmd(&hospi1,
                             &cmd,
                             HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
    {
        Console_Printf("READ command ERROR\r\n");
        return;
    }
    Console_Printf("READ CMD  : OK\r\n");
    if (HAL_OSPI_Receive(&hospi1,
                         rx,
                         HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
    {
        Console_Printf("READ data ERROR\r\n");
        return;
    }
    Console_Printf("READ DATA : OK\r\n");
    uint32_t errors = 0;

    for (uint32_t i = 0; i < sizeof(tx); i++)
    {
        if (rx[i] != tx[i])
        {
            Console_Printf("%02lu: wrote=%02X read=%02X\r\n",
                   (unsigned long)i,
                   tx[i],
                   rx[i]);

            errors++;
        }
    }

    if (errors == 0)
    {
        Console_Printf("HYPERRAM MEMORY TEST: PASS\r\n");
    }
    else
    {
        Console_Printf("HYPERRAM MEMORY TEST: FAIL (%lu errors)\r\n",
               (unsigned long)errors);
    }

    Console_Printf("--- END ---\r\n");
}
