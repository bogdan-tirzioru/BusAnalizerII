#include "hyperram_test.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

extern OSPI_HandleTypeDef hospi1;
extern UART_HandleTypeDef huart1;

#define HYPERRAM_DIR0_ADDRESS    0x00000000U
#define HYPERRAM_DIR1_ADDRESS    0x00000002U

#define HYPERRAM_TEST_REPEATS    10U


static void uart_print(const char *text)
{
    HAL_UART_Transmit(&huart1,
                      (uint8_t *)text,
                      strlen(text),
                      HAL_MAX_DELAY);
}


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
    *value = ((uint16_t)raw[1] << 8) |
             ((uint16_t)raw[0]);

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


    uart_print("\r\n--- HYPERRAM ID REPEAT TEST ---\r\n");


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

        uart_print(text);


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


    uart_print("\r\n");


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

        uart_print(text);

        uart_print(
            "--- HYPERRAM ID TEST FAILED ---\r\n");

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

    uart_print(text);


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

    uart_print(text);


    snprintf(text,
             sizeof(text),

             "Valid reads        : %lu / %u\r\n",

             (unsigned long)valid_count,
             HYPERRAM_TEST_REPEATS);

    uart_print(text);


    /*
     * Require all repeated reads to be valid.
     *
     * This is intentional: for a hardware bring-up test,
     * one successful transaction among unstable reads isn't
     * good enough.
     */
    if (valid_count == HYPERRAM_TEST_REPEATS)
    {
        uart_print(
            "\r\nHYPERRAM ID VERIFY: PASS\r\n");

        uart_print(
            "--- HYPERRAM ID TEST PASSED ---\r\n");
    }
    else
    {
        uart_print(
            "\r\nHYPERRAM ID VERIFY: FAILED\r\n");

        uart_print(
            "--- HYPERRAM ID TEST FAILED ---\r\n");
    }
}
