#include "can_sniffer.h"

#include "main.h"

#include <stdio.h>
#include "can_capture_buffer.h"

#include <string.h>

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



//#define CAN3_GENERATOR_PERIOD_MS  10U


extern FDCAN_HandleTypeDef hfdcan1;


static uint32_t rx_count = 0;
static uint32_t error_count = 0;

static HAL_StatusTypeDef CAN_Sniffer_ReadClassicFrame(
        FDCAN_RxHeaderTypeDef *pRxHeader,
        uint8_t pRxData[8],
        uint8_t *pDataLength);

extern FDCAN_HandleTypeDef hfdcan3;
static FDCAN_TxHeaderTypeDef can3_tx_header;
static uint32_t can3_tx_counter = 0;
static uint32_t can3_tx_tick = 0;

static uint32_t fifo_lost_events = 0U;
static uint32_t max_fifo_fill = 0U;

static uint32_t stress_consumed = 0U;
static uint32_t stress_sequence_errors = 0U;
static uint32_t stress_expected_counter = 0U;

static uint64_t capture_cycles = 0U;
static uint32_t capture_measured_frames = 0U;

static void CAN3_Generator_Init(void);
static void CAN3_Generator_Process(void);



static void CAN_Sniffer_CycleCounter_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
void CAN_Sniffer_Init(void)
{
	static uint64_t capture_cycles = 0U;
	static uint32_t capture_measured_frames = 0U;

	CAN_CaptureBuffer_Init();

	rx_count = 0U;
	error_count = 0U;

	printf("RAM buffer   : %lu frames / %lu bytes\r\n",
	       (unsigned long)CAN_CAPTURE_BUFFER_CAPACITY,
	       (unsigned long)(
	           CAN_CAPTURE_BUFFER_CAPACITY *
	           sizeof(CAN_SnifferFrame)));

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

    CAN3_Generator_Init();
}


void CAN_Sniffer_Process(void)
{
    FDCAN_RxHeaderTypeDef rx_header;

    uint8_t rx_data[8];
    uint8_t rx_length;

    CAN_SnifferFrame frame;


    /*
             * Temporary CAN3 test traffic generator.
             */
        CAN3_Generator_Process();

    uint32_t fifo_fill =
        HAL_FDCAN_GetRxFifoFillLevel(
            &hfdcan1,
            FDCAN_RX_FIFO0);

    if (fifo_fill > max_fifo_fill)
    {
        max_fifo_fill = fifo_fill;
    }

    if (__HAL_FDCAN_GET_FLAG(
            &hfdcan1,
            FDCAN_FLAG_RX_FIFO0_MESSAGE_LOST))
    {
        fifo_lost_events++;

        __HAL_FDCAN_CLEAR_FLAG(
            &hfdcan1,
            FDCAN_FLAG_RX_FIFO0_MESSAGE_LOST);
    }


    uint32_t frames_before = rx_count;
    uint32_t cycle_start = DWT->CYCCNT;

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

        /*
         * Convert ST/FDCAN representation into our own
         * analyzer-independent frame representation.
         */

        frame.id =
            rx_header.Identifier;

        frame.timestamp =
            (uint16_t)rx_header.RxTimestamp;

        frame.dlc =
            (uint8_t)rx_header.DataLength;

        frame.flags = 0U;

        if (rx_header.IdType == FDCAN_EXTENDED_ID)
        {
            frame.flags |= CAN_FRAME_FLAG_EXTENDED;
        }

        if (rx_header.RxFrameType == FDCAN_REMOTE_FRAME)
        {
            frame.flags |= CAN_FRAME_FLAG_RTR;
        }

        if (rx_header.ErrorStateIndicator ==
            FDCAN_ESI_PASSIVE)
        {
            frame.flags |= CAN_FRAME_FLAG_ESI;
        }

        /*
         * Make unused payload bytes deterministic.
         */
        memset(
            frame.data,
            0,
            sizeof(frame.data));

        /*
         * CAN_Sniffer_ReadClassicFrame() guarantees
         * rx_length <= 8.
         */
        memcpy(
            frame.data,
            rx_data,
            rx_length);

        /*
         * Count every valid frame removed from the
         * hardware FDCAN FIFO.
         */
        rx_count++;

        /*
         * Store it.
         *
         * If RAM buffer is full Push() increments its
         * own dropped counter.
         *
         * Most importantly: NO PRINTF HERE.
         */
        (void)CAN_CaptureBuffer_Push(&frame);
    }
    uint32_t cycle_end = DWT->CYCCNT;

    uint32_t frames_captured =
        rx_count - frames_before;

    if (frames_captured > 0U)
    {
        capture_cycles +=
            (uint32_t)(cycle_end - cycle_start);

        capture_measured_frames +=
            frames_captured;
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

static void CAN3_Generator_Init(void)
{
    /*
     * We do not care about frames internally received by CAN3.
     */
    if (HAL_FDCAN_ConfigGlobalFilter(
            &hfdcan3,
            FDCAN_REJECT,
            FDCAN_REJECT,
            FDCAN_REJECT_REMOTE,
            FDCAN_REJECT_REMOTE) != HAL_OK)
    {
        Error_Handler();
    }

    can3_tx_header.Identifier          = 0x123;
    can3_tx_header.IdType              = FDCAN_STANDARD_ID;
    can3_tx_header.TxFrameType         = FDCAN_DATA_FRAME;
    can3_tx_header.DataLength          = FDCAN_DLC_BYTES_8;
    can3_tx_header.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    can3_tx_header.BitRateSwitch       = FDCAN_BRS_OFF;
    can3_tx_header.FDFormat            = FDCAN_CLASSIC_CAN;
    can3_tx_header.TxEventFifoControl  = FDCAN_NO_TX_EVENTS;
    can3_tx_header.MessageMarker       = 0;

    if (HAL_FDCAN_Start(&hfdcan3) != HAL_OK)
    {
        Error_Handler();
    }

    can3_tx_tick = HAL_GetTick();

    printf("CAN3 generator started\r\n");
}

static void CAN3_Generator_Process(void)
{


#if CAN3_GENERATOR_PERIOD_MS > 0
	uint32_t now = HAL_GetTick();

if ((now - can3_tx_tick) < CAN3_GENERATOR_PERIOD_MS)
{
    return;
}

can3_tx_tick = now;

#else

/*
 * Maximum-rate mode.
 * No artificial delay.
 */

#endif

    uint8_t data[8];

    data[0] = (uint8_t)(can3_tx_counter);
    data[1] = (uint8_t)(can3_tx_counter >> 8);
    data[2] = (uint8_t)(can3_tx_counter >> 16);
    data[3] = (uint8_t)(can3_tx_counter >> 24);

    data[4] = 0x11;
    data[5] = 0x22;
    data[6] = 0x33;
    data[7] = 0x44;

    /*
     * Vary the CAN ID deliberately.
     *
     * This proves CAN1 really is unrestricted.
     */
    switch (can3_tx_counter % 4U)
    {
        case 0:
            can3_tx_header.Identifier = 0x123;
            break;

        case 1:
            can3_tx_header.Identifier = 0x321;
            break;

        case 2:
            can3_tx_header.Identifier = 0x555;
            break;

        default:
            can3_tx_header.Identifier = 0x7AA;
            break;
    }

    if (HAL_FDCAN_AddMessageToTxFifoQ(
            &hfdcan3,
            &can3_tx_header,
            data) == HAL_OK)
    {
        can3_tx_counter++;
    }
}


uint32_t CAN_Sniffer_GetBufferedCount(void)
{
    return CAN_CaptureBuffer_GetCount();
}


uint32_t CAN_Sniffer_GetDroppedCount(void)
{
    return CAN_CaptureBuffer_GetDroppedCount();
}


void CAN_Sniffer_DumpBufferedFrames(uint32_t count)
{
    CAN_SnifferFrame frame;

    for (uint32_t n = 0U; n < count; n++)
    {
        if (!CAN_CaptureBuffer_Pop(&frame))
        {
            break;
        }

        printf(
            "RAM[%lu] ID=%08lX DLC=%u FLAGS=%02X TS=%u DATA=",
            (unsigned long)n,
            (unsigned long)frame.id,
            frame.dlc,
            frame.flags,
            frame.timestamp);

        for (uint32_t i = 0U; i < 8U; i++)
        {
            printf("%02X ", frame.data[i]);
        }

        printf("\r\n");
    }
}


uint32_t CAN_Sniffer_GetFifoLostEvents(void)
{
    return fifo_lost_events;
}

uint32_t CAN_Sniffer_GetMaxFifoFill(void)
{
    return max_fifo_fill;
}


void CAN_Sniffer_StressConsume(void)
{
    CAN_SnifferFrame frame;

    while (CAN_CaptureBuffer_Pop(&frame))
    {
        uint32_t counter =
              ((uint32_t)frame.data[0])
            | ((uint32_t)frame.data[1] << 8)
            | ((uint32_t)frame.data[2] << 16)
            | ((uint32_t)frame.data[3] << 24);

        if (counter != stress_expected_counter)
        {
            stress_sequence_errors++;

            /*
             * Resynchronise so a single missing frame
             * does not make every later frame look wrong.
             */
            stress_expected_counter = counter;
        }

        stress_expected_counter++;
        stress_consumed++;
    }
}

uint32_t CAN_Sniffer_GetSequenceErrors(void)
{
    return stress_sequence_errors;
}


uint32_t CAN_Sniffer_GetConsumedCount(void)
{
    return stress_consumed;
}


uint64_t CAN_Sniffer_GetCaptureCycles(void)
{
    return capture_cycles;
}

uint32_t CAN_Sniffer_GetMeasuredFrames(void)
{
    return capture_measured_frames;
}
