#include "can_capture_buffer.h"

#include <stddef.h>
#include <stdio.h>


_Static_assert(
    sizeof(CAN_SnifferFrame) == 16U,
    "CAN_SnifferFrame must be exactly 16 bytes");

_Static_assert(
    (CAN_CAPTURE_BUFFER_CAPACITY &
     (CAN_CAPTURE_BUFFER_CAPACITY - 1U)) == 0U,
    "CAN capture buffer capacity must be power of two");


/*
 * This array occupies exactly 64 KiB.
 *
 * Because it is uninitialized static storage, the current
 * linker puts it into .bss -> RAM_D1.
 */
static CAN_SnifferFrame frame_buffer[
    CAN_CAPTURE_BUFFER_CAPACITY
];


/*
 * Monotonically increasing sequence numbers.
 *
 * Actual array index:
 *
 *     sequence & (capacity - 1)
 *
 * Using sequence numbers makes full/empty handling simpler
 * than wrapping read/write indices manually.
 */
static uint32_t write_sequence = 0U;
static uint32_t read_sequence  = 0U;

static uint32_t dropped_count = 0U;


/*
 * Temporary diagnostic for the controlled CAN3 generator test.
 *
 * This monitor sees each decoded CAN1 frame immediately BEFORE it is copied
 * into the SRAM ring.  Comparing this report with the later pre-write and
 * HyperRAM reports tells us whether corruption/loss already exists at the
 * FDCAN -> software-frame boundary or is introduced by the SRAM ring/storage
 * path.
 *
 * It deliberately prints only once, after the first 100000 accepted frames,
 * so there is no printf activity in the per-frame hot path.
 */
#define CAN_INPUT_DIAG_FRAMES 100000U

static bool input_diag_have_counter = false;
static bool input_diag_printed = false;
static uint32_t input_diag_checked = 0U;
static uint32_t input_diag_expected_counter = 0U;
static uint32_t input_diag_first_counter = 0U;
static uint32_t input_diag_last_counter = 0U;
static uint32_t input_diag_sequence_errors = 0U;
static uint32_t input_diag_large_jumps = 0U;
static uint32_t input_diag_backward_events = 0U;
static uint32_t input_diag_id_errors = 0U;
static uint32_t input_diag_dlc_errors = 0U;
static uint32_t input_diag_flags_errors = 0U;
static uint32_t input_diag_payload_errors = 0U;


static uint32_t CAN_CaptureBuffer_DiagExpectedId(uint32_t counter)
{
    switch (counter & 3U)
    {
        case 0U: return 0x123U;
        case 1U: return 0x321U;
        case 2U: return 0x555U;
        default: return 0x7AAU;
    }
}


static void CAN_CaptureBuffer_PrintInputDiagnostic(void)
{
    printf("\r\n--- CAN1 INPUT BEFORE SRAM RING ---\r\n");
    printf("Frames checked  : %lu\r\n",
           (unsigned long)input_diag_checked);
    printf("First counter   : %lu\r\n",
           (unsigned long)input_diag_first_counter);
    printf("Last counter    : %lu\r\n",
           (unsigned long)input_diag_last_counter);
    printf("Sequence errors : %lu\r\n",
           (unsigned long)input_diag_sequence_errors);
    printf("Large jumps     : %lu\r\n",
           (unsigned long)input_diag_large_jumps);
    printf("Backward events : %lu\r\n",
           (unsigned long)input_diag_backward_events);
    printf("ID errors       : %lu\r\n",
           (unsigned long)input_diag_id_errors);
    printf("DLC errors      : %lu\r\n",
           (unsigned long)input_diag_dlc_errors);
    printf("Flags errors    : %lu\r\n",
           (unsigned long)input_diag_flags_errors);
    printf("Payload errors  : %lu\r\n",
           (unsigned long)input_diag_payload_errors);
    printf("--- END CAN1 INPUT BEFORE SRAM RING ---\r\n\r\n");
}


static void CAN_CaptureBuffer_MonitorInput(
        const CAN_SnifferFrame *frame)
{
    if (input_diag_checked >= CAN_INPUT_DIAG_FRAMES)
    {
        return;
    }

    uint32_t counter =
          ((uint32_t)frame->data[0])
        | ((uint32_t)frame->data[1] << 8)
        | ((uint32_t)frame->data[2] << 16)
        | ((uint32_t)frame->data[3] << 24);

    if (!input_diag_have_counter)
    {
        input_diag_have_counter = true;
        input_diag_first_counter = counter;
        input_diag_expected_counter = counter;
    }

    uint32_t expected_before = input_diag_expected_counter;

    if (counter != expected_before)
    {
        input_diag_sequence_errors++;

        if (counter > expected_before)
        {
            uint32_t jump = counter - expected_before;

            if (jump > CAN_INPUT_DIAG_FRAMES)
            {
                input_diag_large_jumps++;
            }
        }
        else
        {
            input_diag_backward_events++;
        }

        /* Resynchronise after each discontinuity. */
        input_diag_expected_counter = counter;
    }

    input_diag_expected_counter++;
    input_diag_last_counter = counter;

    if (frame->id != CAN_CaptureBuffer_DiagExpectedId(counter))
    {
        input_diag_id_errors++;
    }

    if (frame->dlc != 8U)
    {
        input_diag_dlc_errors++;
    }

    if (frame->flags != 0U)
    {
        input_diag_flags_errors++;
    }

    if ((frame->data[4] != 0x11U) ||
        (frame->data[5] != 0x22U) ||
        (frame->data[6] != 0x33U) ||
        (frame->data[7] != 0x44U))
    {
        input_diag_payload_errors++;
    }

    input_diag_checked++;

    if ((input_diag_checked == CAN_INPUT_DIAG_FRAMES) &&
        !input_diag_printed)
    {
        input_diag_printed = true;
        CAN_CaptureBuffer_PrintInputDiagnostic();
    }
}


void CAN_CaptureBuffer_Init(void)
{
    write_sequence = 0U;
    read_sequence = 0U;
    dropped_count = 0U;

    input_diag_have_counter = false;
    input_diag_printed = false;
    input_diag_checked = 0U;
    input_diag_expected_counter = 0U;
    input_diag_first_counter = 0U;
    input_diag_last_counter = 0U;
    input_diag_sequence_errors = 0U;
    input_diag_large_jumps = 0U;
    input_diag_backward_events = 0U;
    input_diag_id_errors = 0U;
    input_diag_dlc_errors = 0U;
    input_diag_flags_errors = 0U;
    input_diag_payload_errors = 0U;
}


bool CAN_CaptureBuffer_Push(
        const CAN_SnifferFrame *frame)
{
    if (frame == NULL)
    {
        return false;
    }

    /*
     * Buffer full?
     *
     * For an analyzer we preserve frames already captured
     * and DROP THE NEW FRAME.
     *
     * Most importantly, we record that data was lost.
     */
    if ((write_sequence - read_sequence) >=
        CAN_CAPTURE_BUFFER_CAPACITY)
    {
        dropped_count++;

        return false;
    }

    /* Observe the decoded frame before the SRAM ring can modify it. */
    CAN_CaptureBuffer_MonitorInput(frame);

    uint32_t index =
        write_sequence &
        (CAN_CAPTURE_BUFFER_CAPACITY - 1U);

    frame_buffer[index] = *frame;

    write_sequence++;

    return true;
}


bool CAN_CaptureBuffer_Pop(
        CAN_SnifferFrame *frame)
{
    if (frame == NULL)
    {
        return false;
    }

    if (write_sequence == read_sequence)
    {
        return false;
    }

    uint32_t index =
        read_sequence &
        (CAN_CAPTURE_BUFFER_CAPACITY - 1U);

    *frame = frame_buffer[index];

    read_sequence++;

    return true;
}


uint32_t CAN_CaptureBuffer_GetCount(void)
{
    return write_sequence - read_sequence;
}


uint32_t CAN_CaptureBuffer_GetFree(void)
{
    return CAN_CAPTURE_BUFFER_CAPACITY -
           CAN_CaptureBuffer_GetCount();
}


uint32_t CAN_CaptureBuffer_GetDroppedCount(void)
{
    return dropped_count;
}
