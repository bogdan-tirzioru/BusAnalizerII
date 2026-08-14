#include "hyperram_capture.h"

#include "can_capture_buffer.h"
#include "main.h"

#include <stdio.h>

extern OSPI_HandleTypeDef hospi1;

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

static uint32_t write_index = 0U;
static uint32_t stored_count = 0U;
static uint32_t wrap_count = 0U;
static uint32_t write_errors = 0U;
static uint32_t write_lost_frames = 0U;

static CAN_SnifferFrame batch[HYPERRAM_CAPTURE_BATCH_FRAMES];

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

void HyperRAM_Capture_Init(void)
{
    write_index = 0U;
    stored_count = 0U;
    wrap_count = 0U;
    write_errors = 0U;
    write_lost_frames = 0U;

    printf("HyperRAM CAN : %lu frames / %lu bytes\r\n",
           (unsigned long)HYPERRAM_CAPTURE_CAPACITY,
           (unsigned long)HYPERRAM_CAPTURE_SIZE_BYTES);
    printf("HyperRAM CAN : base=0x%08lX, batch=%u frames\r\n",
           (unsigned long)HYPERRAM_CAPTURE_BASE_ADDRESS,
           HYPERRAM_CAPTURE_BATCH_FRAMES);
}

void HyperRAM_Capture_Process(void)
{
    uint32_t available = CAN_CaptureBuffer_GetCount();

    if (available == 0U)
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

    /* Never let a single HyperBus transaction cross the circular boundary. */
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
        return;
    }

    write_index += popped;

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

uint32_t HyperRAM_Capture_GetStoredCount(void)
{
    return stored_count;
}

uint32_t HyperRAM_Capture_GetWriteErrors(void)
{
    return write_errors;
}

uint32_t HyperRAM_Capture_GetWrapCount(void)
{
    return wrap_count;
}
