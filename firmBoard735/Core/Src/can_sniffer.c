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
 *
 * Word 2:
 *   data bytes 0..3
 *
 * Word 3:
 *   data bytes 4..7
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

#define CAN_RAW_DIAG_FRAMES          100000U
#define CAN_RAW_DIAG_SAVED_ANOMALIES 16U
#define CAN_RAW_EXPECTED_STD_ID      0x100U
#define CAN_RAW_EXPECTED_DLC         8U
#define CAN_RAW_EXPECTED_TAIL_WORD   0x341255AAU


extern FDCAN_HandleTypeDef hfdcan1;


typedef struct
{
    uint32_t record;
    uint32_t fifo_index;
    uint32_t expected_counter;
    uint32_t actual_counter;
    uint32_t word0;
    uint32_t word1;
    uint32_t word2;
    uint32_t word3;
} CAN_RawAnomaly;


static uint32_t rx_count = 0;
static uint32_t error_count = 0;

static HAL_StatusTypeDef CAN_Sniffer_ReadClassicFrame(
        FDCAN_RxHeaderTypeDef *pRxHeader,
        uint8_t pRxData[8],
        uint8_t *pDataLength);

static void CAN_Sniffer_RawDiagReset(void);
static void CAN_Sniffer_RawDiagObserve(
        uint32_t fifo_index,
        uint32_t word0,
        uint32_t word1,
        uint32_t word2,
        uint32_t word3);
static void CAN_Sniffer_RawDiagPrintIfPending(void);

static uint32_t fifo_lost_events = 0U;
static uint32_t max_fifo_fill = 0U;

static uint32_t stress_consumed = 0U;
static uint32_t stress_sequence_errors = 0U;
static uint32_t stress_expected_counter = 0U;

static uint64_t capture_cycles = 0U;
static uint32_t capture_measured_frames = 0U;

static uint32_t raw_diag_checked = 0U;
static uint32_t raw_diag_first_counter = 0U;
static uint32_t raw_diag_last_counter = 0U;
static uint32_t raw_diag_expected_counter = 0U;
static uint32_t raw_diag_sequence_errors = 0U;
static uint32_t raw_diag_forward_gap = 0U;
static uint32_t raw_diag_backward_events = 0U;
static uint32_t raw_diag_id_errors = 0U;
static uint32_t raw_diag_flags_errors = 0U;
static uint32_t raw_diag_dlc_errors = 0U;
static uint32_t raw_diag_control_errors = 0U;
static uint32_t raw_diag_payload_errors = 0U;
static uint32_t raw_diag_saved_count = 0U;
static uint8_t raw_diag_have_counter = 0U;
static uint8_t raw_diag_report_pending = 0U;
static uint8_t raw_diag_report_printed = 0U;
static CAN_RawAnomaly raw_diag_anomalies[CAN_RAW_DIAG_SAVED_ANOMALIES];


static void CAN_Sniffer_CycleCounter_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}


static void CAN_Sniffer_RawDiagReset(void)
{
    raw_diag_checked = 0U;
    raw_diag_first_counter = 0U;
    raw_diag_last_counter = 0U;
    raw_diag_expected_counter = 0U;
    raw_diag_sequence_errors = 0U;
    raw_diag_forward_gap = 0U;
    raw_diag_backward_events = 0U;
    raw_diag_id_errors = 0U;
    raw_diag_flags_errors = 0U;
    raw_diag_dlc_errors = 0U;
    raw_diag_control_errors = 0U;
    raw_diag_payload_errors = 0U;
    raw_diag_saved_count = 0U;
    raw_diag_have_counter = 0U;
    raw_diag_report_pending = 0U;
    raw_diag_report_printed = 0U;

    memset(raw_diag_anomalies, 0, sizeof(raw_diag_anomalies));
}


static void CAN_Sniffer_RawDiagObserve(
        uint32_t fifo_index,
        uint32_t word0,
        uint32_t word1,
        uint32_t word2,
        uint32_t word3)
{
    if (raw_diag_checked >= CAN_RAW_DIAG_FRAMES)
    {
        return;
    }

    uint32_t counter = word2;
    uint32_t raw_id;
    uint32_t raw_dlc =
        (word1 & CAN_SNIFFER_RX_DLC_MASK) >> 16U;

    uint8_t anomaly = 0U;

    if (raw_diag_have_counter == 0U)
    {
        raw_diag_have_counter = 1U;
        raw_diag_first_counter = counter;
        raw_diag_expected_counter = counter;
    }

    uint32_t expected_before = raw_diag_expected_counter;

    if (counter != expected_before)
    {
        raw_diag_sequence_errors++;
        anomaly = 1U;

        if (counter > expected_before)
        {
            raw_diag_forward_gap += counter - expected_before;
        }
        else
        {
            raw_diag_backward_events++;
        }

        /* Resynchronise so one discontinuity does not poison later checks. */
        raw_diag_expected_counter = counter;
    }

    raw_diag_expected_counter++;
    raw_diag_last_counter = counter;

    if ((word0 & CAN_SNIFFER_RX_XTD_MASK) == 0U)
    {
        raw_id =
            (word0 & CAN_SNIFFER_RX_STDID_MASK) >> 18U;
    }
    else
    {
        raw_id = word0 & CAN_SNIFFER_RX_EXTID_MASK;
    }

    if (raw_id != CAN_RAW_EXPECTED_STD_ID)
    {
        raw_diag_id_errors++;
        anomaly = 1U;
    }

    if ((word0 & (CAN_SNIFFER_RX_RTR_MASK |
                  CAN_SNIFFER_RX_XTD_MASK |
                  CAN_SNIFFER_RX_ESI_MASK)) != 0U)
    {
        raw_diag_flags_errors++;
        anomaly = 1U;
    }

    if (raw_dlc != CAN_RAW_EXPECTED_DLC)
    {
        raw_diag_dlc_errors++;
        anomaly = 1U;
    }

    if ((word1 & (CAN_SNIFFER_RX_BRS_MASK |
                  CAN_SNIFFER_RX_FDF_MASK)) != 0U)
    {
        raw_diag_control_errors++;
        anomaly = 1U;
    }

    if (word3 != CAN_RAW_EXPECTED_TAIL_WORD)
    {
        raw_diag_payload_errors++;
        anomaly = 1U;
    }

    if ((anomaly != 0U) &&
        (raw_diag_saved_count < CAN_RAW_DIAG_SAVED_ANOMALIES))
    {
        CAN_RawAnomaly *saved =
            &raw_diag_anomalies[raw_diag_saved_count];

        saved->record = raw_diag_checked;
        saved->fifo_index = fifo_index;
        saved->expected_counter = expected_before;
        saved->actual_counter = counter;
        saved->word0 = word0;
        saved->word1 = word1;
        saved->word2 = word2;
        saved->word3 = word3;

        raw_diag_saved_count++;
    }

    raw_diag_checked++;

    if (raw_diag_checked == CAN_RAW_DIAG_FRAMES)
    {
        raw_diag_report_pending = 1U;
    }
}


static void CAN_Sniffer_RawDiagPrintIfPending(void)
{
    if ((raw_diag_report_pending == 0U) ||
        (raw_diag_report_printed != 0U))
    {
        return;
    }

    raw_diag_report_pending = 0U;
    raw_diag_report_printed = 1U;

    printf("\r\n--- RAW FDCAN MESSAGE RAM BEFORE DECODE ---\r\n");
    printf("Expected       : W0 STD ID=100, DLC=8, W3=341255AA\r\n");
    printf("Frames checked : %lu\r\n",
           (unsigned long)raw_diag_checked);
    printf("First counter  : %lu\r\n",
           (unsigned long)raw_diag_first_counter);
    printf("Last counter   : %lu\r\n",
           (unsigned long)raw_diag_last_counter);
    printf("Sequence errors: %lu\r\n",
           (unsigned long)raw_diag_sequence_errors);
    printf("Forward gap    : %lu frame(s)\r\n",
           (unsigned long)raw_diag_forward_gap);
    printf("Backward events: %lu\r\n",
           (unsigned long)raw_diag_backward_events);
    printf("ID errors      : %lu\r\n",
           (unsigned long)raw_diag_id_errors);
    printf("Flags errors   : %lu\r\n",
           (unsigned long)raw_diag_flags_errors);
    printf("DLC errors     : %lu\r\n",
           (unsigned long)raw_diag_dlc_errors);
    printf("Control errors : %lu\r\n",
           (unsigned long)raw_diag_control_errors);
    printf("Payload errors : %lu\r\n",
           (unsigned long)raw_diag_payload_errors);
    printf("Saved anomalies: %lu / %u\r\n",
           (unsigned long)raw_diag_saved_count,
           (unsigned int)CAN_RAW_DIAG_SAVED_ANOMALIES);

    for (uint32_t i = 0U; i < raw_diag_saved_count; i++)
    {
        const CAN_RawAnomaly *saved = &raw_diag_anomalies[i];

        printf(
            "RAW[%lu] rec=%lu GI=%lu exp=%lu got=%lu "
            "W0=%08lX W1=%08lX W2=%08lX W3=%08lX\r\n",
            (unsigned long)i,
            (unsigned long)saved->record,
            (unsigned long)saved->fifo_index,
            (unsigned long)saved->expected_counter,
            (unsigned long)saved->actual_counter,
            (unsigned long)saved->word0,
            (unsigned long)saved->word1,
            (unsigned long)saved->word2,
            (unsigned long)saved->word3);
    }

    printf("--- END RAW FDCAN MESSAGE RAM BEFORE DECODE ---\r\n\r\n");
}


void CAN_Sniffer_Init(void)
{

	CAN_CaptureBuffer_Init();

	rx_count = 0U;
	error_count = 0U;
    fifo_lost_events = 0U;
    max_fifo_fill = 0U;
    capture_cycles = 0U;
    capture_measured_frames = 0U;
    CAN_Sniffer_RawDiagReset();

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
    printf("Source      : EXTERNAL CAN GENERATOR\r\n");
    printf("Test pattern: ID=100 DLC=8 tail=AA 55 12 34\r\n");
    printf("STD IDs     : ACCEPT ALL\r\n");
    printf("EXT IDs     : ACCEPT ALL\r\n");
    printf("Remote      : ACCEPT\r\n");
    printf("RX FIFO0    : 64 frames\r\n");
    printf("RX read path: DIRECT MESSAGE RAM\r\n");
    printf("RAW verify  : 100000 frames before decode / FIFO ACK\r\n");
    printf("CAN1 TX     : DISABLED\r\n");
    printf("CAN3        : DISABLED FOR THIS TEST\r\n");
    printf("--------------------\r\n");
}


void CAN_Sniffer_Process(void)
{
    FDCAN_RxHeaderTypeDef rx_header;

    uint8_t rx_data[64];
    uint8_t rx_length;

    CAN_SnifferFrame frame;

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

    if (fifo_fill == 0U)
    {
        CAN_Sniffer_RawDiagPrintIfPending();
        return;
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
    	    CAN_Sniffer_RawDiagPrintIfPending();
    	    return;
    	}

    	/*
    	 * The direct Message RAM reader returns the raw DLC in DataLength.
    	 *
    	 * Classic CAN can physically contain at most 8 data bytes.
    	 */
    	if (rx_header.DataLength <= 8U)
    	{
    	    rx_length = (uint8_t)rx_header.DataLength;
    	}
    	else
    	{
    	    rx_length = 8U;
    	}

    	if (rx_header.RxFrameType == FDCAN_REMOTE_FRAME)
    	{
    	    rx_length = 0U;
    	}

        /*
         * Convert ST/FDCAN representation into our own
         * analyzer-independent frame representation.
         */
        frame.id = rx_header.Identifier;
        frame.timestamp = (uint16_t)rx_header.RxTimestamp;
        frame.dlc = (uint8_t)rx_header.DataLength;
        frame.flags = 0U;

        if (rx_header.IdType == FDCAN_EXTENDED_ID)
        {
            frame.flags |= CAN_FRAME_FLAG_EXTENDED;
        }

        if (rx_header.RxFrameType == FDCAN_REMOTE_FRAME)
        {
            frame.flags |= CAN_FRAME_FLAG_RTR;
        }

        if (rx_header.ErrorStateIndicator == FDCAN_ESI_PASSIVE)
        {
            frame.flags |= CAN_FRAME_FLAG_ESI;
        }

        /* Make unused payload bytes deterministic. */
        memset(frame.data, 0, sizeof(frame.data));

        memcpy(frame.data, rx_data, rx_length);

        /* Count every valid frame removed from the hardware FDCAN FIFO. */
        rx_count++;

        /*
         * Store it. If RAM buffer is full Push() increments its own dropped
         * counter. Most importantly: NO PRINTF HERE.
         */
        (void)CAN_CaptureBuffer_Push(&frame);
    }

    uint32_t cycle_end = DWT->CYCCNT;

    uint32_t frames_captured = rx_count - frames_before;

    if (frames_captured > 0U)
    {
        capture_cycles += (uint32_t)(cycle_end - cycle_start);
        capture_measured_frames += frames_captured;
    }

    /* Print only after the FIFO has been drained and every raw element ACKed. */
    CAN_Sniffer_RawDiagPrintIfPending();
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
    uint32_t word2;
    uint32_t word3;

    uint8_t raw_dlc;
    uint8_t data_length;

    if ((pRxHeader == NULL) ||
        (pRxData == NULL) ||
        (pDataLength == NULL))
    {
        return HAL_ERROR;
    }

    *pDataLength = 0U;

    /* FDCAN must already have been started. */
    if (hfdcan1.State != HAL_FDCAN_STATE_BUSY)
    {
        return HAL_ERROR;
    }

    /* Make sure RX FIFO0 is actually configured. */
    if ((hfdcan1.Instance->RXF0C & FDCAN_RXF0C_F0S) == 0U)
    {
        return HAL_ERROR;
    }

    fifo_status = hfdcan1.Instance->RXF0S;

    if ((fifo_status & FDCAN_RXF0S_F0FL) == 0U)
    {
        return HAL_ERROR;
    }

    get_index =
        (fifo_status & FDCAN_RXF0S_F0GI) >>
        FDCAN_RXF0S_F0GI_Pos;

    rx_element =
        (volatile const uint32_t *)(uintptr_t)
        (
            hfdcan1.msgRam.RxFIFO0SA +
            (get_index * hfdcan1.Init.RxFifo0ElmtSize * 4U)
        );

    /*
     * Snapshot the complete classic-CAN element before any decode and before
     * writing RXF0A. The raw validator sees exactly what M_CAN exposed in
     * Message RAM for this FIFO index.
     */
    word0 = rx_element[0];
    word1 = rx_element[1];
    word2 = rx_element[2];
    word3 = rx_element[3];

    CAN_Sniffer_RawDiagObserve(
        get_index,
        word0,
        word1,
        word2,
        word3);

    pRxHeader->IdType = word0 & CAN_SNIFFER_RX_XTD_MASK;

    if (pRxHeader->IdType == FDCAN_STANDARD_ID)
    {
        pRxHeader->Identifier =
            (word0 & CAN_SNIFFER_RX_STDID_MASK) >> 18U;
    }
    else
    {
        pRxHeader->Identifier = word0 & CAN_SNIFFER_RX_EXTID_MASK;
    }

    pRxHeader->RxFrameType = word0 & CAN_SNIFFER_RX_RTR_MASK;
    pRxHeader->ErrorStateIndicator = word0 & CAN_SNIFFER_RX_ESI_MASK;
    pRxHeader->RxTimestamp = word1 & CAN_SNIFFER_RX_TS_MASK;

    raw_dlc =
        (uint8_t)
        ((word1 & CAN_SNIFFER_RX_DLC_MASK) >> 16U);

    pRxHeader->DataLength = raw_dlc;
    pRxHeader->BitRateSwitch = word1 & CAN_SNIFFER_RX_BRS_MASK;
    pRxHeader->FDFormat = word1 & CAN_SNIFFER_RX_FDF_MASK;
    pRxHeader->FilterIndex =
        (word1 & CAN_SNIFFER_RX_FIDX_MASK) >> 24U;
    pRxHeader->IsFilterMatchingFrame =
        (word1 & CAN_SNIFFER_RX_ANMF_MASK) >> 31U;

    if (pRxHeader->FDFormat != FDCAN_CLASSIC_CAN)
    {
        hfdcan1.Instance->RXF0A = get_index;
        return HAL_ERROR;
    }

    if (raw_dlc <= 8U)
    {
        data_length = raw_dlc;
    }
    else
    {
        data_length = 8U;
    }

    if (pRxHeader->RxFrameType == FDCAN_REMOTE_FRAME)
    {
        data_length = 0U;
    }

    rx_payload = (volatile const uint8_t *)&rx_element[2];

    for (uint8_t i = 0U; i < data_length; i++)
    {
        pRxData[i] = rx_payload[i];
    }

    *pDataLength = data_length;

    hfdcan1.Instance->RXF0A = get_index;

    return HAL_OK;
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
