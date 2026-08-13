#include "can_capture_buffer.h"

#include <stddef.h>


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


void CAN_CaptureBuffer_Init(void)
{
    write_sequence = 0U;
    read_sequence = 0U;
    dropped_count = 0U;
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
