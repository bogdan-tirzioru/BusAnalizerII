#include "can_sniffer.h"

#include "main.h"

#include <stdio.h>


/*
 * Bosch M_CAN Rx FIFO element layout.
 *
 * Word 0:
 *   bits 28:0  Identifier
 *   bit  29    RTR
 *   bit  30    XTD
 *   bit  31    ESI
 *
 * Word 1:
 *   bits 15:0  Timestamp
 *   bits 19:16 DLC
 *   bit  20    BRS
 *   bit  21    FDF
 *   bits 30:24 Filter index
 *   bit  31    Accepted non-matching frame
 */

#define CAN_SNIFFER_RX_STDID_MASK    0x1FFC0000U
#define CAN_SNIFFER_RX_EXTID_MASK    0x1FFFFFFFU
#define CAN_SNIFFER_RX_RTR_MASK      0x20000000U
#define CAN_SNIFFER_RX_XTD_MASK      0x40000000U
#define CAN_SNIFFER_RX_ESI_MASK      0x80000000U

#define CAN_SNIFFER_RX_TS_MASK       0x0000FFFFU
#define CAN_SNIFFER_RX_DLC_MASK      0x000F0000U
#define CAN_SNIFFER_RX_BRS_MASK      0x00100000U
#define CAN_SNIFFER_RX_FDF_MASK      0x00200000U
#define CAN_SNIFFER_RX_FIDX_MASK     0x7F000000U
#define CAN_SNIFFER_RX_ANMF_MASK     0x80000000U


extern FDCAN_HandleTypeDef hfdcan1;

static FDCAN_RxHeaderTypeDef rx_header;
static uint8_t rx_data[8];
static uint8_t rx_length;

static uint32_t rx_count = 0;
static uint32_t error_count = 0;

static HAL_StatusTypeDef CAN_Sniffer_ReadClassicFrame(
        FDCAN_RxHeaderTypeDef *pRxHeader,
        uint8_t pRxData[8],
        uint8_t *pDataLength);


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
        if (CAN_Sniffer_ReadClassicFrame(
                &rx_header,
                rx_data,
                &rx_length) != HAL_OK)
        {
            error_count++;
            return;
        }

        rx_count++;

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

        /*
         * For RTR preserve and display the raw DLC.
         */
        if (rx_header.RxFrameType == FDCAN_REMOTE_FRAME)
        {
            printf("RTR DLC=%lu\r\n",
                   (unsigned long)rx_header.DataLength);

            continue;
        }

        /*
         * Show both raw DLC and actual payload length.
         *
         * Usually:
         *
         * DLC=8 LEN=8
         *
         * But a legal Classic CAN DLC=15 frame becomes:
         *
         * DLC=15 LEN=8
         */
        printf("DLC=%lu LEN=%u DATA=",
               (unsigned long)rx_header.DataLength,
               rx_length);

        for (uint8_t i = 0U; i < rx_length; i++)
        {
            printf("%02X", rx_data[i]);

            if ((i + 1U) < rx_length)
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

static HAL_StatusTypeDef CAN_Sniffer_ReadClassicFrame(
        FDCAN_RxHeaderTypeDef *pRxHeader,
        uint8_t pRxData[8],
        uint8_t *pDataLength)
{
    uint32_t fifo_status;
    uint32_t get_index;

    volatile const uint32_t *rx_element;
    volatile const uint8_t *rx_payload;

    uint32_t word0;
    uint32_t word1;

    uint8_t raw_dlc;
    uint8_t data_length;

    if ((pRxHeader == NULL) ||
        (pRxData == NULL) ||
        (pDataLength == NULL))
    {
        return HAL_ERROR;
    }

    *pDataLength = 0U;

    /*
     * FDCAN must already have been started.
     */
    if (hfdcan1.State != HAL_FDCAN_STATE_BUSY)
    {
        return HAL_ERROR;
    }

    /*
     * Make sure RX FIFO0 is actually configured.
     */
    if ((hfdcan1.Instance->RXF0C & FDCAN_RXF0C_F0S) == 0U)
    {
        return HAL_ERROR;
    }

    /*
     * Read FIFO0 status once.
     */
    fifo_status = hfdcan1.Instance->RXF0S;

    /*
     * FIFO empty?
     */
    if ((fifo_status & FDCAN_RXF0S_F0FL) == 0U)
    {
        return HAL_ERROR;
    }

    /*
     * Index of oldest unread FIFO element.
     */
    get_index =
        (fifo_status & FDCAN_RXF0S_F0GI) >>
        FDCAN_RXF0S_F0GI_Pos;

    /*
     * Calculate address in FDCAN Message RAM.
     *
     * RxFifo0ElmtSize is expressed in 32-bit words.
     * For our 8-byte RX element this value is 4:
     *
     *   word 0 : header
     *   word 1 : header
     *   word 2 : data[0..3]
     *   word 3 : data[4..7]
     */
    rx_element =
        (volatile const uint32_t *)(uintptr_t)
        (
            hfdcan1.msgRam.RxFIFO0SA +
            (get_index *
             hfdcan1.Init.RxFifo0ElmtSize *
             4U)
        );

    word0 = rx_element[0];
    word1 = rx_element[1];

    /*
     * Decode identifier type.
     */
    pRxHeader->IdType =
        word0 & CAN_SNIFFER_RX_XTD_MASK;

    /*
     * Decode identifier.
     */
    if (pRxHeader->IdType == FDCAN_STANDARD_ID)
    {
        pRxHeader->Identifier =
            (word0 & CAN_SNIFFER_RX_STDID_MASK) >> 18U;
    }
    else
    {
        pRxHeader->Identifier =
            word0 & CAN_SNIFFER_RX_EXTID_MASK;
    }

    /*
     * Decode remaining first-word fields.
     */
    pRxHeader->RxFrameType =
        word0 & CAN_SNIFFER_RX_RTR_MASK;

    pRxHeader->ErrorStateIndicator =
        word0 & CAN_SNIFFER_RX_ESI_MASK;

    /*
     * Second header word.
     */
    pRxHeader->RxTimestamp =
        word1 & CAN_SNIFFER_RX_TS_MASK;

    /*
     * IMPORTANT:
     *
     * Preserve the RAW 4-bit DLC from the CAN frame.
     *
     * 0..15 is therefore deliberately stored here,
     * not converted through ST's CAN-FD DLC table.
     */
    raw_dlc =
        (uint8_t)
        ((word1 & CAN_SNIFFER_RX_DLC_MASK) >> 16U);

    pRxHeader->DataLength = raw_dlc;

    pRxHeader->BitRateSwitch =
        word1 & CAN_SNIFFER_RX_BRS_MASK;

    pRxHeader->FDFormat =
        word1 & CAN_SNIFFER_RX_FDF_MASK;

    pRxHeader->FilterIndex =
        (word1 & CAN_SNIFFER_RX_FIDX_MASK) >> 24U;

    pRxHeader->IsFilterMatchingFrame =
        (word1 & CAN_SNIFFER_RX_ANMF_MASK) >> 31U;

    /*
     * This reader deliberately supports Classic CAN only.
     *
     * Normally this cannot happen because CAN1 is configured
     * in Classic CAN mode, but never leave an unexpected
     * FIFO element unacknowledged.
     */
    if (pRxHeader->FDFormat != FDCAN_CLASSIC_CAN)
    {
        hfdcan1.Instance->RXF0A = get_index;

        return HAL_ERROR;
    }

    /*
     * Classical CAN DLC mapping:
     *
     * DLC 0..8  -> 0..8 bytes
     * DLC 9..15 -> 8 bytes
     */
    if (raw_dlc <= 8U)
    {
        data_length = raw_dlc;
    }
    else
    {
        data_length = 8U;
    }

    /*
     * RTR frames do not contain a data field.
     *
     * Preserve raw DLC because for an RTR frame it describes
     * the requested data length, but copy zero payload bytes.
     */
    if (pRxHeader->RxFrameType == FDCAN_REMOTE_FRAME)
    {
        data_length = 0U;
    }

    /*
     * Payload starts after the two 32-bit header words.
     */
    rx_payload =
        (volatile const uint8_t *)&rx_element[2];

    /*
     * Safe copy: never more than 8 bytes.
     */
    for (uint8_t i = 0U; i < data_length; i++)
    {
        pRxData[i] = rx_payload[i];
    }

    *pDataLength = data_length;

    /*
     * Tell M_CAN that this FIFO0 element has been consumed.
     *
     * Hardware advances FIFO0's GetIndex after this write.
     */
    hfdcan1.Instance->RXF0A = get_index;

    return HAL_OK;
}
