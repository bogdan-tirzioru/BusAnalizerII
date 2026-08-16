#include "hyperram_capture.h"

#include "can_capture_buffer.h"
#include "can_sniffer.h"
#include "main.h"

#include <stdio.h>

extern OSPI_HandleTypeDef hospi1;
extern FDCAN_HandleTypeDef hfdcan1;

/* Physical S27KL0641 fitted on the board: 8 MiB. */
#define HYPERRAM_SIZE_BYTES             (8U * 1024U * 1024U)

/* Keep the first 4 KiB free for future capture metadata/header. */
#define HYPERRAM_CAPTURE_BASE_ADDRESS   0x00001000U
#define HYPERRAM_CAPTURE_SIZE_BYTES     \
    (HYPERRAM_SIZE_BYTES - HYPERRAM_CAPTURE_BASE_ADDRESS)

#define HYPERRAM_CAPTURE_CAPACITY       \
    (HYPERRAM_CAPTURE_SIZE_BYTES / sizeof(CAN_SnifferFrame))

/* Write path: 32 x 16-byte records = one 512-byte HyperBus transaction. */
#define HYPERRAM_CAPTURE_BATCH_FRAMES   32U

/* Verification deliberately uses a different transaction size. */
#define HYPERRAM_VERIFY_BATCH_FRAMES    31U

#define HYPERRAM_CAPTURE_FLUSH_MS       10U

/*
 * Stop well before the end of the 524032-frame HyperRAM region.  The spare
 * 24032 records are much larger than the 4096-frame SRAM queue plus the
 * 64-frame FDCAN FIFO, so all already-admitted data can be drained safely.
 */
#define HYPERRAM_VERIFY_STOP_AT_FRAMES  500000U
#define HYPERRAM_VERIFY_MAX_DETAILS     16U

_Static_assert(
    HYPERRAM_VERIFY_STOP_AT_FRAMES < HYPERRAM_CAPTURE_CAPACITY,
    "HyperRAM verify stop point must leave drain margin");

_Static_assert(
    (HYPERRAM_CAPTURE_CAPACITY - HYPERRAM_VERIFY_STOP_AT_FRAMES) >
        (CAN_CAPTURE_BUFFER_CAPACITY + 64U),
    "HyperRAM verify stop point does not leave enough drain margin");

static uint32_t write_index = 0U;
static uint32_t stored_count = 0U;
static uint32_t wrap_count = 0U;
static uint32_t write_errors = 0U;
static uint32_t write_lost_frames = 0U;
static uint32_t last_flush_tick = 0U;

static CAN_SnifferFrame batch[HYPERRAM_CAPTURE_BATCH_FRAMES];
static CAN_SnifferFrame verify_batch[HYPERRAM_VERIFY_BATCH_FRAMES];

/*
 * Diagnostic monitor of the exact SRAM records immediately before they are
 * handed to HAL_OSPI_Transmit(). This separates upstream CAN/SRAM errors from
 * errors introduced by the HyperRAM storage/readback path.
 *
 * External generator format:
 *   standard ID : 0x100
 *   DLC         : 8
 *   data[0..3]  : monotonically increasing little-endian counter
 *   data[4..7]  : AA 55 12 34
 */
static bool prewrite_have_counter = false;
static uint32_t prewrite_checked_count = 0U;
static uint32_t prewrite_expected_counter = 0U;
static uint32_t prewrite_first_counter = 0U;
static uint32_t prewrite_last_counter = 0U;
static uint32_t prewrite_sequence_errors = 0U;
static uint32_t prewrite_missing_frames = 0U;
static uint32_t prewrite_backward_events = 0U;
static uint32_t prewrite_large_jump_events = 0U;
static uint32_t prewrite_id_errors = 0U;
static uint32_t prewrite_dlc_errors = 0U;
static uint32_t prewrite_flags_errors = 0U;
static uint32_t prewrite_payload_errors = 0U;

static bool verify_started = false;
static bool verify_scan_started = false;
static bool verify_done = false;
static bool verify_passed = false;
static bool verify_fdcan_stopped = false;

static uint32_t verify_start_index = 0U;
static uint32_t verify_target_count = 0U;
static uint32_t verify_checked_count = 0U;

static uint32_t verify_rx_count_at_stop = 0U;
static uint32_t verify_drop_count_at_stop = 0U;
static uint32_t verify_fifo_lost_at_stop = 0U;
static uint32_t verify_sniffer_errors_at_stop = 0U;

static uint32_t verify_read_errors = 0U;
static uint32_t verify_control_errors = 0U;
static uint32_t verify_sequence_errors = 0U;
static uint32_t verify_forward_gap_frames = 0U;
static uint32_t verify_large_jump_events = 0U;
static uint32_t verify_backward_events = 0U;
static uint32_t verify_id_errors = 0U;
static uint32_t verify_dlc_errors = 0U;
static uint32_t verify_flags_errors = 0U;
static uint32_t verify_payload_errors = 0U;
static uint32_t verify_detail_count = 0U;

static bool verify_have_counter = false;
static uint32_t verify_expected_counter = 0U;
static uint32_t verify_first_counter = 0U;
static uint32_t verify_last_counter = 0U;

static void HyperRAM_Capture_PrintVerifyReport(void);

static HAL_StatusTypeDef HyperRAM_Capture_Write(
        uint32_t address,
        const void *data,
        uint32_t size)
{
    OSPI_HyperbusCmdTypeDef cmd = {0};

    if ((data == NULL) || (size == 0U))
    {
        return HAL_ERROR;
    }

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

static HAL_StatusTypeDef HyperRAM_Capture_Read(
        uint32_t address,
        void *data,
        uint32_t size)
{
    OSPI_HyperbusCmdTypeDef cmd = {0};

    if ((data == NULL) || (size == 0U))
    {
        return HAL_ERROR;
    }

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
        (uint8_t *)data,
        HAL_OSPI_TIMEOUT_DEFAULT_VALUE);
}

static uint32_t HyperRAM_Capture_ExpectedId(uint32_t counter)
{
    (void)counter;
    return 0x100U;
}

static uint32_t HyperRAM_Capture_ReadCounter(const CAN_SnifferFrame *frame)
{
    return
          ((uint32_t)frame->data[0])
        | ((uint32_t)frame->data[1] << 8)
        | ((uint32_t)frame->data[2] << 16)
        | ((uint32_t)frame->data[3] << 24);
}

static void HyperRAM_Capture_MonitorPreWrite(const CAN_SnifferFrame *frame)
{
    uint32_t counter = HyperRAM_Capture_ReadCounter(frame);

    if (!prewrite_have_counter)
    {
        prewrite_have_counter = true;
        prewrite_first_counter = counter;
        prewrite_expected_counter = counter;
    }

    uint32_t expected_before = prewrite_expected_counter;

    if (counter != expected_before)
    {
        prewrite_sequence_errors++;

        if (counter > expected_before)
        {
            uint32_t gap = counter - expected_before;

            if (gap <= HYPERRAM_VERIFY_STOP_AT_FRAMES)
            {
                prewrite_missing_frames += gap;
            }
            else
            {
                prewrite_large_jump_events++;
            }
        }
        else
        {
            prewrite_backward_events++;
        }

        prewrite_expected_counter = counter;
    }

    prewrite_expected_counter++;
    prewrite_last_counter = counter;
    prewrite_checked_count++;

    if (frame->id != HyperRAM_Capture_ExpectedId(counter))
    {
        prewrite_id_errors++;
    }

    if (frame->dlc != 8U)
    {
        prewrite_dlc_errors++;
    }

    if (frame->flags != 0U)
    {
        prewrite_flags_errors++;
    }

    if ((frame->data[4] != 0xAAU) ||
        (frame->data[5] != 0x55U) ||
        (frame->data[6] != 0x12U) ||
        (frame->data[7] != 0x34U))
    {
        prewrite_payload_errors++;
    }
}

static void HyperRAM_Capture_StoreProcess(void)
{
    uint32_t available = CAN_CaptureBuffer_GetCount();

    if (available == 0U)
    {
        return;
    }

    uint32_t now = HAL_GetTick();

    if ((available < HYPERRAM_CAPTURE_BATCH_FRAMES) &&
        ((now - last_flush_tick) < HYPERRAM_CAPTURE_FLUSH_MS))
    {
        return;
    }

    uint32_t remaining_to_end =
        HYPERRAM_CAPTURE_CAPACITY - write_index;

    uint32_t count = available;

    if (count > HYPERRAM_CAPTURE_BATCH_FRAMES)
    {
        count = HYPERRAM_CAPTURE_BATCH_FRAMES;
    }

    if (count > remaining_to_end)
    {
        count = remaining_to_end;
    }

    uint32_t popped = 0U;

    while (popped < count)
    {
        if (!CAN_CaptureBuffer_Pop(&batch[popped]))
        {
            break;
        }

        popped++;
    }

    if (popped == 0U)
    {
        return;
    }

    for (uint32_t i = 0U; i < popped; i++)
    {
        HyperRAM_Capture_MonitorPreWrite(&batch[i]);
    }

    uint32_t address =
        HYPERRAM_CAPTURE_BASE_ADDRESS +
        (write_index * sizeof(CAN_SnifferFrame));

    if (HyperRAM_Capture_Write(
            address,
            batch,
            popped * sizeof(CAN_SnifferFrame)) != HAL_OK)
    {
        write_errors++;
        write_lost_frames += popped;
        last_flush_tick = now;
        return;
    }

    write_index += popped;
    last_flush_tick = now;

    if (stored_count < HYPERRAM_CAPTURE_CAPACITY)
    {
        uint32_t free_records =
            HYPERRAM_CAPTURE_CAPACITY - stored_count;

        if (popped <= free_records)
        {
            stored_count += popped;
        }
        else
        {
            stored_count = HYPERRAM_CAPTURE_CAPACITY;
        }
    }

    if (write_index >= HYPERRAM_CAPTURE_CAPACITY)
    {
        write_index = 0U;
        wrap_count++;
    }
}

static void HyperRAM_Capture_RequestVerification(void)
{
    verify_started = true;
    verify_scan_started = false;
    verify_done = false;
    verify_passed = false;

    verify_rx_count_at_stop = CAN_Sniffer_GetRxCount();
    verify_drop_count_at_stop = CAN_Sniffer_GetDroppedCount();
    verify_fifo_lost_at_stop = CAN_Sniffer_GetFifoLostEvents();
    verify_sniffer_errors_at_stop = CAN_Sniffer_GetErrorCount();

    printf("\r\nHyperRAM verify: stop point reached (%lu stored frames)\r\n",
           (unsigned long)stored_count);
    printf("HyperRAM verify: stopping FDCAN1 to freeze acquisition...\r\n");

    if (HAL_FDCAN_Stop(&hfdcan1) != HAL_OK)
    {
        verify_control_errors++;
        verify_target_count = stored_count;
        verify_done = true;
        verify_passed = false;
        printf("HyperRAM verify: ABORT - HAL_FDCAN_Stop failed\r\n");
        HyperRAM_Capture_PrintVerifyReport();
        return;
    }

    verify_fdcan_stopped = true;

    printf("HyperRAM verify: FDCAN1 stopped, RX snapshot=%lu\r\n",
           (unsigned long)verify_rx_count_at_stop);
    printf("HyperRAM verify: draining SRAM queue to HyperRAM...\r\n");
}

static void HyperRAM_Capture_BeginVerificationScan(void)
{
    verify_start_index = 0U;
    verify_target_count = stored_count;
    verify_checked_count = 0U;

    verify_read_errors = 0U;
    verify_sequence_errors = 0U;
    verify_forward_gap_frames = 0U;
    verify_large_jump_events = 0U;
    verify_backward_events = 0U;
    verify_id_errors = 0U;
    verify_dlc_errors = 0U;
    verify_flags_errors = 0U;
    verify_payload_errors = 0U;
    verify_detail_count = 0U;

    verify_have_counter = false;
    verify_expected_counter = 0U;
    verify_first_counter = 0U;
    verify_last_counter = 0U;

    if (!verify_fdcan_stopped)
    {
        verify_control_errors++;
    }

    if (wrap_count != 0U)
    {
        verify_control_errors++;
    }

    if (verify_target_count < HYPERRAM_VERIFY_STOP_AT_FRAMES)
    {
        verify_control_errors++;
    }

    if (verify_target_count != verify_rx_count_at_stop)
    {
        verify_control_errors++;
    }

    if ((verify_drop_count_at_stop != 0U) ||
        (verify_fifo_lost_at_stop != 0U) ||
        (verify_sniffer_errors_at_stop != 0U) ||
        (write_errors != 0U) ||
        (write_lost_frames != 0U))
    {
        verify_control_errors++;
    }

    verify_scan_started = true;

    printf("\r\n--- HYPERRAM CAPTURE READBACK VERIFY ---\r\n");
    printf("Capture frozen  : %s\r\n",
           verify_fdcan_stopped ? "YES (FDCAN1 stopped)" : "NO");
    printf("Snapshot stable : %s\r\n",
           (wrap_count == 0U) ? "YES (no HyperRAM wrap)" : "NO");
    printf("Expected        : ID=100 DLC=8 tail=AA 55 12 34\r\n");
    printf("RX at stop      : %lu\r\n",
           (unsigned long)verify_rx_count_at_stop);
    printf("Stored frames   : %lu\r\n",
           (unsigned long)stored_count);
    printf("SRAM pending    : %lu\r\n",
           (unsigned long)CAN_CaptureBuffer_GetCount());
    printf("Snapshot frames : %lu\r\n",
           (unsigned long)verify_target_count);
    printf("Write batch     : %u records\r\n",
           HYPERRAM_CAPTURE_BATCH_FRAMES);
    printf("Read batch      : %u records\r\n",
           HYPERRAM_VERIFY_BATCH_FRAMES);
    printf("Pre-stop drop   : %lu\r\n",
           (unsigned long)verify_drop_count_at_stop);
    printf("Pre-stop fifoLost: %lu\r\n",
           (unsigned long)verify_fifo_lost_at_stop);
    printf("Pre-stop errors : %lu\r\n",
           (unsigned long)verify_sniffer_errors_at_stop);
}

static void HyperRAM_Capture_PrintVerifyReport(void)
{
    if (!verify_done)
    {
        verify_passed =
            verify_fdcan_stopped &&
            (wrap_count == 0U) &&
            (verify_checked_count == verify_target_count) &&
            (verify_target_count == verify_rx_count_at_stop) &&
            (prewrite_checked_count == verify_target_count) &&
            (verify_drop_count_at_stop == 0U) &&
            (verify_fifo_lost_at_stop == 0U) &&
            (verify_sniffer_errors_at_stop == 0U) &&
            (write_errors == 0U) &&
            (write_lost_frames == 0U) &&
            (prewrite_sequence_errors == 0U) &&
            (prewrite_id_errors == 0U) &&
            (prewrite_dlc_errors == 0U) &&
            (prewrite_flags_errors == 0U) &&
            (prewrite_payload_errors == 0U) &&
            (verify_control_errors == 0U) &&
            (verify_read_errors == 0U) &&
            (verify_sequence_errors == 0U) &&
            (verify_id_errors == 0U) &&
            (verify_dlc_errors == 0U) &&
            (verify_flags_errors == 0U) &&
            (verify_payload_errors == 0U);

        verify_done = true;
    }

    printf("\r\n--- PRE-WRITE SRAM SEQUENCE ---\r\n");
    printf("Expected        : ID=100 DLC=8 tail=AA 55 12 34\r\n");
    printf("Frames checked  : %lu\r\n",
           (unsigned long)prewrite_checked_count);
    printf("First counter   : %lu\r\n",
           (unsigned long)prewrite_first_counter);
    printf("Last counter    : %lu\r\n",
           (unsigned long)prewrite_last_counter);
    printf("Sequence errors : %lu\r\n",
           (unsigned long)prewrite_sequence_errors);
    printf("Missing frames  : %lu\r\n",
           (unsigned long)prewrite_missing_frames);
    printf("Large jumps     : %lu\r\n",
           (unsigned long)prewrite_large_jump_events);
    printf("Backward events : %lu\r\n",
           (unsigned long)prewrite_backward_events);
    printf("ID errors       : %lu\r\n",
           (unsigned long)prewrite_id_errors);
    printf("DLC errors      : %lu\r\n",
           (unsigned long)prewrite_dlc_errors);
    printf("Flags errors    : %lu\r\n",
           (unsigned long)prewrite_flags_errors);
    printf("Payload errors  : %lu\r\n",
           (unsigned long)prewrite_payload_errors);
    printf("--- END PRE-WRITE SRAM SEQUENCE ---\r\n");

    FDCAN_ProtocolStatusTypeDef ps1 = {0};
    FDCAN_ErrorCountersTypeDef ec1 = {0};

    HAL_StatusTypeDef st_ps1 =
        HAL_FDCAN_GetProtocolStatus(&hfdcan1, &ps1);

    HAL_StatusTypeDef st_ec1 =
        HAL_FDCAN_GetErrorCounters(&hfdcan1, &ec1);

    printf("\r\n--- FDCAN1 DIAGNOSTIC ---\r\n");
    printf(
        "CAN1 HAL=%u/%u LEC=%lu DLEC=%lu "
        "REC=%lu TEC=%lu LOG=%lu "
        "EP=%lu WARN=%lu BO=%lu\r\n",
        (unsigned int)st_ps1,
        (unsigned int)st_ec1,
        (unsigned long)ps1.LastErrorCode,
        (unsigned long)ps1.DataLastErrorCode,
        (unsigned long)ec1.RxErrorCnt,
        (unsigned long)ec1.TxErrorCnt,
        (unsigned long)ec1.ErrorLogging,
        (unsigned long)ps1.ErrorPassive,
        (unsigned long)ps1.Warning,
        (unsigned long)ps1.BusOff);
    printf("--- END FDCAN1 DIAGNOSTIC ---\r\n");

    printf("Frames checked  : %lu / %lu\r\n",
           (unsigned long)verify_checked_count,
           (unsigned long)verify_target_count);
    printf("First counter   : %lu\r\n",
           (unsigned long)verify_first_counter);
    printf("Last counter    : %lu\r\n",
           (unsigned long)verify_last_counter);
    printf("Control errors  : %lu\r\n",
           (unsigned long)verify_control_errors);
    printf("Read errors     : %lu\r\n",
           (unsigned long)verify_read_errors);
    printf("Sequence errors : %lu\r\n",
           (unsigned long)verify_sequence_errors);
    printf("Forward gap     : %lu frame(s)\r\n",
           (unsigned long)verify_forward_gap_frames);
    printf("Large jumps     : %lu\r\n",
           (unsigned long)verify_large_jump_events);
    printf("Backward events : %lu\r\n",
           (unsigned long)verify_backward_events);
    printf("ID errors       : %lu\r\n",
           (unsigned long)verify_id_errors);
    printf("DLC errors      : %lu\r\n",
           (unsigned long)verify_dlc_errors);
    printf("Flags errors    : %lu\r\n",
           (unsigned long)verify_flags_errors);
    printf("Payload errors  : %lu\r\n",
           (unsigned long)verify_payload_errors);

    if (verify_passed)
    {
        printf("HYPERRAM CAPTURE VERIFY: PASS\r\n");
    }
    else
    {
        printf("HYPERRAM CAPTURE VERIFY: FAIL\r\n");
    }

    printf("--- END HYPERRAM CAPTURE VERIFY ---\r\n\r\n");
}

static void HyperRAM_Capture_VerifyFrame(
        const CAN_SnifferFrame *frame,
        uint32_t record_index)
{
    uint32_t counter = HyperRAM_Capture_ReadCounter(frame);

    if (!verify_have_counter)
    {
        verify_have_counter = true;
        verify_first_counter = counter;
        verify_expected_counter = counter;
    }

    uint32_t expected_before = verify_expected_counter;
    bool sequence_bad = (counter != expected_before);
    bool id_bad =
        (frame->id != HyperRAM_Capture_ExpectedId(counter));
    bool dlc_bad = (frame->dlc != 8U);
    bool flags_bad = (frame->flags != 0U);
    bool payload_bad =
        (frame->data[4] != 0xAAU) ||
        (frame->data[5] != 0x55U) ||
        (frame->data[6] != 0x12U) ||
        (frame->data[7] != 0x34U);

    if (sequence_bad)
    {
        verify_sequence_errors++;

        if (counter > expected_before)
        {
            uint32_t gap = counter - expected_before;

            if (gap <= HYPERRAM_VERIFY_STOP_AT_FRAMES)
            {
                verify_forward_gap_frames += gap;
            }
            else
            {
                verify_large_jump_events++;
            }
        }
        else
        {
            verify_backward_events++;
        }

        verify_expected_counter = counter;
    }

    verify_expected_counter++;
    verify_last_counter = counter;

    if (id_bad)      { verify_id_errors++; }
    if (dlc_bad)     { verify_dlc_errors++; }
    if (flags_bad)   { verify_flags_errors++; }
    if (payload_bad) { verify_payload_errors++; }

    if ((sequence_bad || id_bad || dlc_bad || flags_bad || payload_bad) &&
        (verify_detail_count < HYPERRAM_VERIFY_MAX_DETAILS))
    {
        printf(
            "Mismatch rec=%lu RB=%u WB=%u exp=%lu got=%lu "
            "ID=%03lX DLC=%u FL=%02X "
            "DATA=%02X %02X %02X %02X %02X %02X %02X %02X\r\n",
            (unsigned long)record_index,
            ((record_index % HYPERRAM_VERIFY_BATCH_FRAMES) == 0U) ? 1U : 0U,
            ((record_index % HYPERRAM_CAPTURE_BATCH_FRAMES) == 0U) ? 1U : 0U,
            (unsigned long)expected_before,
            (unsigned long)counter,
            (unsigned long)frame->id,
            frame->dlc,
            frame->flags,
            frame->data[0], frame->data[1],
            frame->data[2], frame->data[3],
            frame->data[4], frame->data[5],
            frame->data[6], frame->data[7]);

        verify_detail_count++;
    }
}

static void HyperRAM_Capture_VerifyProcess(void)
{
    if (verify_done)
    {
        return;
    }

    if (!verify_started)
    {
        /*
         * CAN_Sniffer_Process() runs immediately before this function in the
         * main loop. Only stop FDCAN when FIFO0 is empty, minimizing the
         * freeze boundary window while the external generator remains active.
         */
        if ((wrap_count == 0U) &&
            (stored_count >= HYPERRAM_VERIFY_STOP_AT_FRAMES) &&
            (HAL_FDCAN_GetRxFifoFillLevel(
                 &hfdcan1,
                 FDCAN_RX_FIFO0) == 0U))
        {
            HyperRAM_Capture_RequestVerification();
        }

        return;
    }

    if (!verify_scan_started)
    {
        if (!verify_fdcan_stopped)
        {
            return;
        }

        if (wrap_count != 0U)
        {
            verify_control_errors++;
            verify_target_count = stored_count;
            verify_done = true;
            verify_passed = false;
            printf("HyperRAM verify: ABORT - capture wrapped while draining\r\n");
            HyperRAM_Capture_PrintVerifyReport();
            return;
        }

        /* StoreProcess() runs first, so wait until every SRAM record is stored. */
        if (CAN_CaptureBuffer_GetCount() != 0U)
        {
            return;
        }

        HyperRAM_Capture_BeginVerificationScan();
        return;
    }

    uint32_t remaining =
        verify_target_count - verify_checked_count;

    if (remaining == 0U)
    {
        HyperRAM_Capture_PrintVerifyReport();
        return;
    }

    uint32_t physical_index =
        verify_start_index + verify_checked_count;

    if (physical_index >= HYPERRAM_CAPTURE_CAPACITY)
    {
        physical_index -= HYPERRAM_CAPTURE_CAPACITY;
    }

    uint32_t count = remaining;

    if (count > HYPERRAM_VERIFY_BATCH_FRAMES)
    {
        count = HYPERRAM_VERIFY_BATCH_FRAMES;
    }

    uint32_t remaining_to_end =
        HYPERRAM_CAPTURE_CAPACITY - physical_index;

    if (count > remaining_to_end)
    {
        count = remaining_to_end;
    }

    uint32_t address =
        HYPERRAM_CAPTURE_BASE_ADDRESS +
        (physical_index * sizeof(CAN_SnifferFrame));

    if (HyperRAM_Capture_Read(
            address,
            verify_batch,
            count * sizeof(CAN_SnifferFrame)) != HAL_OK)
    {
        verify_read_errors++;
        verify_done = true;
        verify_passed = false;
        HyperRAM_Capture_PrintVerifyReport();
        return;
    }

    for (uint32_t i = 0U; i < count; i++)
    {
        HyperRAM_Capture_VerifyFrame(
            &verify_batch[i],
            verify_checked_count + i);
    }

    verify_checked_count += count;

    if (verify_checked_count >= verify_target_count)
    {
        HyperRAM_Capture_PrintVerifyReport();
    }
}

void HyperRAM_Capture_Init(void)
{
    write_index = 0U;
    stored_count = 0U;
    wrap_count = 0U;
    write_errors = 0U;
    write_lost_frames = 0U;
    last_flush_tick = HAL_GetTick();

    prewrite_have_counter = false;
    prewrite_checked_count = 0U;
    prewrite_expected_counter = 0U;
    prewrite_first_counter = 0U;
    prewrite_last_counter = 0U;
    prewrite_sequence_errors = 0U;
    prewrite_missing_frames = 0U;
    prewrite_backward_events = 0U;
    prewrite_large_jump_events = 0U;
    prewrite_id_errors = 0U;
    prewrite_dlc_errors = 0U;
    prewrite_flags_errors = 0U;
    prewrite_payload_errors = 0U;

    verify_started = false;
    verify_scan_started = false;
    verify_done = false;
    verify_passed = false;
    verify_fdcan_stopped = false;
    verify_start_index = 0U;
    verify_target_count = 0U;
    verify_checked_count = 0U;
    verify_rx_count_at_stop = 0U;
    verify_drop_count_at_stop = 0U;
    verify_fifo_lost_at_stop = 0U;
    verify_sniffer_errors_at_stop = 0U;
    verify_read_errors = 0U;
    verify_control_errors = 0U;
    verify_sequence_errors = 0U;
    verify_forward_gap_frames = 0U;
    verify_large_jump_events = 0U;
    verify_backward_events = 0U;
    verify_id_errors = 0U;
    verify_dlc_errors = 0U;
    verify_flags_errors = 0U;
    verify_payload_errors = 0U;
    verify_detail_count = 0U;
    verify_have_counter = false;
    verify_expected_counter = 0U;
    verify_first_counter = 0U;
    verify_last_counter = 0U;

    printf("HyperRAM CAN : %lu frames / %lu bytes\r\n",
           (unsigned long)HYPERRAM_CAPTURE_CAPACITY,
           (unsigned long)HYPERRAM_CAPTURE_SIZE_BYTES);
    printf("HyperRAM CAN : base=0x%08lX, write batch=%u frames\r\n",
           (unsigned long)HYPERRAM_CAPTURE_BASE_ADDRESS,
           HYPERRAM_CAPTURE_BATCH_FRAMES);
    printf("HyperRAM verify: stop at >=%lu frames, then freeze + drain + scan\r\n",
           (unsigned long)HYPERRAM_VERIFY_STOP_AT_FRAMES);
    printf("HyperRAM verify: read batch=%u frames\r\n",
           HYPERRAM_VERIFY_BATCH_FRAMES);
    printf("HyperRAM diagnostic: external pattern ID=100 tail=AA 55 12 34\r\n");
    printf("HyperRAM diagnostic: SRAM sequence checked before each write\r\n");
}

void HyperRAM_Capture_Process(void)
{
    HyperRAM_Capture_StoreProcess();
    HyperRAM_Capture_VerifyProcess();
}

uint32_t HyperRAM_Capture_GetStoredCount(void)
{
    return stored_count;
}

uint32_t HyperRAM_Capture_GetWriteErrors(void)
{
    return write_errors;
}

uint32_t HyperRAM_Capture_GetWriteLostFrames(void)
{
    return write_lost_frames;
}

uint32_t HyperRAM_Capture_GetWrapCount(void)
{
    return wrap_count;
}

bool HyperRAM_Capture_IsVerifyDone(void)
{
    return verify_done;
}

bool HyperRAM_Capture_IsVerifyPassed(void)
{
    return verify_done && verify_passed;
}

uint32_t HyperRAM_Capture_GetVerifiedCount(void)
{
    return verify_checked_count;
}
