#include "can_sniffer.h"

#include "main.h"

#include <stdio.h>

extern FDCAN_HandleTypeDef hfdcan1;

static FDCAN_RxHeaderTypeDef rx_header;
static uint8_t rx_data[8];

static uint32_t rx_count = 0;
static uint32_t error_count = 0;


static uint8_t CAN_Sniffer_DlcToLength(uint32_t dlc)
{
    switch (dlc)
    {
        case FDCAN_DLC_BYTES_0: return 0;
        case FDCAN_DLC_BYTES_1: return 1;
        case FDCAN_DLC_BYTES_2: return 2;
        case FDCAN_DLC_BYTES_3: return 3;
        case FDCAN_DLC_BYTES_4: return 4;
        case FDCAN_DLC_BYTES_5: return 5;
        case FDCAN_DLC_BYTES_6: return 6;
        case FDCAN_DLC_BYTES_7: return 7;
        case FDCAN_DLC_BYTES_8: return 8;

        default:
            return 0;
    }
}


void CAN_Sniffer_Init(void)
{
    /*
     * No dedicated ID filters are configured.
     *
     * Therefore every standard and extended frame is
     * a "non-matching" frame.
     *
     * Route all of them to RX FIFO0.
     *
     * Remote frames are allowed to pass through the
     * normal acceptance mechanism too.
     */
    if (HAL_FDCAN_ConfigGlobalFilter(
            &hfdcan1,
            FDCAN_ACCEPT_IN_RX_FIFO0,
            FDCAN_ACCEPT_IN_RX_FIFO0,
            FDCAN_FILTER_REMOTE,
            FDCAN_FILTER_REMOTE) != HAL_OK)
    {
        Error_Handler();
    }

    /*
     * Enable the FDCAN hardware timestamp counter.
     *
     * We will improve timestamp handling later.
     */
    if (HAL_FDCAN_ConfigTimestampCounter(
            &hfdcan1,
            FDCAN_TIMESTAMP_PRESC_1) != HAL_OK)
    {
        Error_Handler();
    }

    if (HAL_FDCAN_EnableTimestampCounter(
            &hfdcan1,
            FDCAN_TIMESTAMP_INTERNAL) != HAL_OK)
    {
        Error_Handler();
    }

    if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK)
    {
        Error_Handler();
    }

    printf("\r\n");
    printf("--- CAN1 SNIFFER ---\r\n");
    printf("Mode        : BUS MONITORING\r\n");
    printf("Bitrate     : 500 kbit/s\r\n");
    printf("STD IDs     : ACCEPT ALL\r\n");
    printf("EXT IDs     : ACCEPT ALL\r\n");
    printf("Remote      : ACCEPT\r\n");
    printf("RX FIFO0    : 64 frames\r\n");
    printf("CAN1 TX     : DISABLED\r\n");
    printf("--------------------\r\n");
}


void CAN_Sniffer_Process(void)
{
    while (HAL_FDCAN_GetRxFifoFillLevel(
               &hfdcan1,
               FDCAN_RX_FIFO0) > 0U)
    {
        if (HAL_FDCAN_GetRxMessage(
                &hfdcan1,
                FDCAN_RX_FIFO0,
                &rx_header,
                rx_data) != HAL_OK)
        {
            error_count++;
            return;
        }

        rx_count++;

        uint8_t length =
            CAN_Sniffer_DlcToLength(rx_header.DataLength);

        printf("%08lu  TS=%05lu  ",
               (unsigned long)rx_count,
               (unsigned long)rx_header.RxTimestamp);

        if (rx_header.IdType == FDCAN_STANDARD_ID)
        {
            printf("STD %03lX ",
                   (unsigned long)rx_header.Identifier);
        }
        else
        {
            printf("EXT %08lX ",
                   (unsigned long)rx_header.Identifier);
        }

        if (rx_header.RxFrameType == FDCAN_REMOTE_FRAME)
        {
            printf("RTR DLC=%u\r\n", length);
            continue;
        }

        printf("DLC=%u DATA=", length);

        for (uint8_t i = 0; i < length; i++)
        {
            printf("%02X", rx_data[i]);

            if (i + 1U < length)
            {
                printf(" ");
            }
        }

        printf("\r\n");
    }
}


uint32_t CAN_Sniffer_GetRxCount(void)
{
    return rx_count;
}


uint32_t CAN_Sniffer_GetErrorCount(void)
{
    return error_count;
}
