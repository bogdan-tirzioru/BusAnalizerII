#include "hyperram_capture.h"

#include "can_capture_buffer.h"
#include "main.h"

#include <stdio.h>

extern OSPI_HandleTypeDef hospi1;
extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan3;

/* Physical S27KL0641 fitted on the board: 8 MiB. */
#define HYPERRAM_SIZE_BYTES             (8U * 1024U * 1024U)

/* Keep the first 4 KiB free for future capture metadata/header. */
#define HYPERRAM_CAPTURE_BASE_ADDRESS   0x00001000U
#define HYPERRAM_CAPTURE_SIZE_BYTES     \
    (HYPERRAM_SIZE_BYTES - HYPERRAM_CAPTURE_BASE_ADDRESS)

#define HYPERRAM_CAPTURE_CAPACITY       \
    (HYPERRAM_CAPTURE_SIZE_BYTES / sizeof(CAN_SnifferFrame))

/* 32 x 16-byte Classic CAN records = one 512-byte HyperBus transfer. */
#define HYPERRAM_CAPTURE_BATCH_FRAMES   32U

/* Flush a partial batch if traffic becomes sparse or stops. */
#define HYPERRAM_CAPTURE_FLUSH_MS       10U

/* First functional readback milestone. */
#define HYPERRAM_CAPTURE_VERIFY_FRAMES  100000U

/* Allow the final CAN frame already in flight to arrive before scanning. */
#define HYPERRAM_VERIFY_QUIET_MS        20U

/* Keep serial output bounded even if the image is badly damaged. */
#define HYPERRAM_VERIFY_MAX_DETAILS     12U

_Static_assert(
    HYPERRAM_CAPTURE_VERIFY_FRAMES <= HYPERRAM_CAPTURE_CAPACITY,
    "HyperRAM verify target exceeds capture capacity");

static uint32_t write_index = 0U;
static uint32_t stored_count = 0U;
static uint32_t wrap_count = 0U;
static uint32_t write_errors = 0U;
static uint32_t write_lost_frames = 0U;
static uint32_t last_flush_tick = 0U;

static CAN_SnifferFrame batch[HYPERRAM_CAPTURE_BATCH_FRAMES];
static CAN_SnifferFrame verify_batch[HYPERRAM_CAPTURE_BATCH_FRAMES];

static bool verify_started = false;
static bool verify_scan_started = false;
static bool verify_done = false;
static bool verify_passed = false;

static uint32_t verify_stop_tick = 0U;
static uint32_t verify_start_index = 0U;
static uint32_t verify_target_count = 0U;
static uint32_t verify_checked_count = 0U;

static uint32_t verify_read_errors = 0U;
static uint32_t verify_control_errors = 0U;
static uint32_t verify_sequence_errors = 0U;
static uint32_t verify_forward_gap_frames = 0U;
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

static HAL_StatusTypeDef HyperRAM_Capture_Write(
        uint32_t address,
        const void *data,
        uint32_t size);

static HAL_StatusTypeDef HyperRAM_Capture_Read(
        uint32_t address,
        void *data,
        uint32_t size);

static void HyperRAM_Capture_StoreProcess(void);
static void HyperRAM_Capture_VerifyProcess(void);
static void HyperRAM_Capture_RequestVerification(void);
static void HyperRAM_Capture_BeginVerificationScan(void);
static void HyperRAM_Capture_PrintVerifyReport(void);
static uint32_t HyperRAM_Capture_ExpectedId(uint32_t counter);

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

void HyperRAM_Capture_Init(void)
{
    write_index = 0U;
    stored_count = 0U;
    wrap_count = 0U;
    write_errors = 0U;
    write_lost_frames = 0U;
    last_flush_tick = HAL_GetTick();

    verify_started = false;
    verify_scan_started = false;
    verify_done = false;
    verify_passed = false;
    verify_stop_tick = 0U;
    verify_start_index = 0U;
    verify_target_count = 0U;
    verify_checked_count = 0U;
    verify_read_errors = 0U;
    verify_control_errors = 0U;
    verify_sequence_errors = 0U;
    verify_forward_gap_frames = 0U;
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
    printf("HyperRAM CAN : base=0x%08lX, batch=%u frames\r\n",
           (unsigned long)HYPERRAM_CAPTURE_BASE_ADDRESS,
           HYPERRAM_CAPTURE_BATCH_FRAMES);
    printf("HyperRAM verify target: %lu frames\r\n",
           (unsigned long)HYPERRAM_CAPTURE_VERIFY_FRAMES);
}

void HyperRAM_Capture_Process(void)
{
    HyperRAM_Capture_StoreProcess();
    HyperRAM_Capture_VerifyProcess();
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
    verify_stop_tick = HAL_GetTick();

    /*
     * Freeze our only test traffic source. CAN1 remains active so an already
     * transmitted final frame can still be drained from FIFO0 into SRAM.
     */
    if (HAL_FDCAN_Stop(&hfdcan3) != HAL_OK)
    {
        verify_control_errors++;
        printf("HyperRAM verify: CAN3 stop ERROR\r\n");
    }
    else
    {
        printf("\r\nHyperRAM verify: CAN3 generator stopped\r\n");
        printf("HyperRAM verify: draining CAN1 FIFO + SRAM...\r\n");
    }
}

static void HyperRAM_Capture_BeginVerificationScan(void)
{
    verify_start_index = 0U;
    verify_target_count = HYPERRAM_CAPTURE_VERIFY_FRAMES;
    verify_checked_count = 0U;

    verify_read_errors = 0U;
    verify_sequence_errors = 0U;
    verify_forward_gap_frames = 0U;
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
    verify_scan_started = true;

    printf("\r\n--- HYPERRAM CAPTURE READBACK VERIFY ---\r\n");
    printf("Capture frozen  : YES\r\n");
    printf("Stored frames   : %lu\r\n",
           (unsigned long)stored_count);
    printf("Snapshot frames : %lu\r\n",
           (unsigned long)verify_target_count);
    printf("Snapshot start  : record %lu\r\n",
           (unsigned long)verify_start_index);
}

static uint32_t HyperRAM_Capture_ExpectedId(uint32_t counter)
{
    switch (counter & 3U)
    {
        case 0U:
            return 0x123U;
        case 1U:
            return 0x321U;
        case 2U:
            return 0x555U;
        default:
            return 0x7AAU;
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
        if ((wrap_count == 0U) &&
            (stored_count >= HYPERRAM_CAPTURE_VERIFY_FRAMES))
        {
            HyperRAM_Capture_RequestVerification();
        }

        return;
    }

    if (!verify_scan_started)
    {
        uint32_t now = HAL_GetTick();

        if ((now - verify_stop_tick) < HYPERRAM_VERIFY_QUIET_MS)
        {
            return;
        }

        if (HAL_FDCAN_GetRxFifoFillLevel(
                &hfdcan1,
                FDCAN_RX_FIFO0) != 0U)
        {
            return;
        }

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

    if (count > HYPERRAM_CAPTURE_BATCH_FRAMES)
    {
        count = HYPERRAM_CAPTURE_BATCH_FRAMES;
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
        const CAN_SnifferFrame *frame = &verify_batch[i];
        uint32_t record_index = verify_checked_count + i;

        uint32_t counter =
              ((uint32_t)frame->data[0])
            | ((uint32_t)frame->data[1] << 8)
            | ((uint32_t)frame->data[2] << 16)
            | ((uint32_t)frame->data[3] << 24);

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
            (frame->data[4] != 0x11U) ||
            (frame->data[5] != 0x22U) ||
            (frame->data[6] != 0x33U) ||
            (frame->data[7] != 0x44U);

        if (sequence_bad)
        {
            verify_sequence_errors++;

            if (counter > expected_before)
            {
                verify_forward_gap_frames +=
                    counter - expected_before;
            }
            else
            {
                verify_backward_events++;
            }

            verify_expected_counter = counter;
        }

        verify_expected_counter++;
        verify_last_counter = counter;

        if (id_bad)
        {
            verify_id_errors++;
        }

        if (dlc_bad)
        {
            verify_dlc_errors++;
        }

        if (flags_bad)
        {
            verify_flags_errors++;
        }

        if (payload_bad)
        {
            verify_payload_errors++;
        }

        if ((sequence_bad || id_bad || dlc_bad || flags_bad || payload_bad) &&
            (verify_detail_count < HYPERRAM_VERIFY_MAX_DETAILS))
        {
            printf(
                "Mismatch rec=%lu exp=%lu got=%lu ID=%03lX DLC=%u FL=%02X "
                "DATA=%02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                (unsigned long)record_index,
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

    verify_checked_count += count;

    if (verify_checked_count >= verify_target_count)
    {
        HyperRAM_Capture_PrintVerifyReport();
    }
}

static void HyperRAM_Capture_PrintVerifyReport(void)
{
    if (!verify_done)
    {
        verify_passed =
            (verify_checked_count == verify_target_count) &&
            (verify_control_errors == 0U) &&
            (verify_read_errors == 0U) &&
            (verify_sequence_errors == 0U) &&
            (verify_id_errors == 0U) &&
            (verify_dlc_errors == 0U) &&
            (verify_flags_errors == 0U) &&
            (verify_payload_errors == 0U);

        verify_done = true;
    }

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
