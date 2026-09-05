#include "console.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CONSOLE_FORMAT_SIZE      1024U
#define CONSOLE_DMA_CHUNK_SIZE   64U
#define CONSOLE_QUEUE_CAPACITY   128U

_Static_assert((CONSOLE_QUEUE_CAPACITY & (CONSOLE_QUEUE_CAPACITY - 1U)) == 0U,
               "console queue capacity must be a power of two");
_Static_assert(CONSOLE_DMA_CHUNK_SIZE <= UINT16_MAX,
               "console DMA chunk length must fit uint16_t");
_Static_assert(CONSOLE_FORMAT_SIZE <=
                   (CONSOLE_DMA_CHUNK_SIZE * CONSOLE_QUEUE_CAPACITY),
               "formatted console message must fit an empty queue");

#if defined(__GNUC__)
#define CONSOLE_DMA_STORAGE \
    __attribute__((section(".console_ram"), aligned(32)))
#else
#define CONSOLE_DMA_STORAGE
#endif

static UART_HandleTypeDef *console_uart;
static uint8_t console_messages[CONSOLE_QUEUE_CAPACITY][CONSOLE_DMA_CHUNK_SIZE]
    CONSOLE_DMA_STORAGE;
static uint16_t console_lengths[CONSOLE_QUEUE_CAPACITY]
    CONSOLE_DMA_STORAGE;
static volatile uint32_t console_write_sequence;
static volatile uint32_t console_read_sequence;
static volatile uint32_t console_dropped_messages;
static volatile uint32_t console_uart_errors;
static volatile uint8_t console_tx_busy;

static uint32_t Console_EnterCritical(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    return primask;
}

static void Console_ExitCritical(uint32_t primask)
{
    if (primask == 0U)
    {
        __enable_irq();
    }
}

static void Console_StartNext(void)
{
    HAL_StatusTypeDef status;
    uint32_t index;
    uint32_t primask;
    uint16_t length;

    if ((console_uart == NULL) || (console_uart->hdmatx == NULL))
    {
        return;
    }

    primask = Console_EnterCritical();

    if ((console_tx_busy != 0U) ||
        (console_read_sequence == console_write_sequence))
    {
        Console_ExitCritical(primask);
        return;
    }

    index = console_read_sequence & (CONSOLE_QUEUE_CAPACITY - 1U);
    length = console_lengths[index];
    console_tx_busy = 1U;

    Console_ExitCritical(primask);

    status = HAL_UART_Transmit_DMA(console_uart,
                                   console_messages[index],
                                   length);
    if (status != HAL_OK)
    {
        primask = Console_EnterCritical();
        console_tx_busy = 0U;
        console_read_sequence++;
        console_dropped_messages++;
        console_uart_errors++;
        Console_ExitCritical(primask);

        Console_StartNext();
    }
}

static void Console_Queue(const uint8_t *message, uint32_t length)
{
    uint32_t chunk_count;
    uint32_t chunk_length;
    uint32_t offset;
    uint32_t chunk;
    uint32_t index;
    uint32_t primask;
    uint32_t write_sequence;

    if ((console_uart == NULL) ||
        (console_uart->hdmatx == NULL) ||
        (message == NULL) ||
        (length == 0U))
    {
        return;
    }

    chunk_count =
        (length + CONSOLE_DMA_CHUNK_SIZE - 1U) / CONSOLE_DMA_CHUNK_SIZE;

    if (chunk_count > CONSOLE_QUEUE_CAPACITY)
    {
        primask = Console_EnterCritical();
        console_dropped_messages++;
        Console_ExitCritical(primask);
        return;
    }

    /* Reserve, fill, and publish every chunk as one atomic operation.  A long
     * logical line therefore cannot be interleaved with interrupt logging,
     * while no individual UART DMA transaction exceeds one 64-byte packet. */
    primask = Console_EnterCritical();
    write_sequence = console_write_sequence;
    if (((write_sequence - console_read_sequence) + chunk_count) >
        CONSOLE_QUEUE_CAPACITY)
    {
        console_dropped_messages++;
        Console_ExitCritical(primask);
        return;
    }

    offset = 0U;
    for (chunk = 0U; chunk < chunk_count; chunk++)
    {
        chunk_length = length - offset;
        if (chunk_length > CONSOLE_DMA_CHUNK_SIZE)
        {
            chunk_length = CONSOLE_DMA_CHUNK_SIZE;
        }

        index = (write_sequence + chunk) & (CONSOLE_QUEUE_CAPACITY - 1U);
        (void)memcpy(console_messages[index], &message[offset], chunk_length);
        console_lengths[index] = (uint16_t)chunk_length;
        offset += chunk_length;
    }

    /* Publish only after the complete logical message is visible to the ISR. */
    __DMB();
    console_write_sequence = write_sequence + chunk_count;
    Console_ExitCritical(primask);

    Console_StartNext();
}

void Console_Init(UART_HandleTypeDef *huart)
{
    uint32_t primask = Console_EnterCritical();

    /* USB initialization binds the console before main() binds it again. */
    if ((console_uart != NULL) && (console_uart == huart))
    {
        Console_ExitCritical(primask);
        return;
    }

    console_uart = huart;
    console_write_sequence = 0U;
    console_read_sequence = 0U;
    console_dropped_messages = 0U;
    console_uart_errors = 0U;
    console_tx_busy = 0U;

    Console_ExitCritical(primask);

}

void Console_Write(const char *message)
{
    size_t length;

    if (message == NULL)
    {
        return;
    }

    length = strlen(message);
    Console_Queue((const uint8_t *)message, (uint32_t)length);
}

void Console_Printf(const char *format, ...)
{
    char buffer[CONSOLE_FORMAT_SIZE];
    va_list args;
    int length;

    if (format == NULL)
    {
        return;
    }

    va_start(args, format);
    length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (length <= 0)
    {
        return;
    }

    if ((uint32_t)length >= sizeof(buffer))
    {
        uint32_t primask = Console_EnterCritical();
        console_dropped_messages++;
        Console_ExitCritical(primask);
        return;
    }

    Console_Queue((const uint8_t *)buffer, (uint32_t)length);
}

static void Console_CompleteActive(UART_HandleTypeDef *huart, uint8_t failed)
{
    uint32_t primask;

    if ((huart == NULL) || (huart != console_uart))
    {
        return;
    }

    primask = Console_EnterCritical();
    if (console_tx_busy != 0U)
    {
        console_tx_busy = 0U;
        console_read_sequence++;

        if (failed != 0U)
        {
            console_dropped_messages++;
            console_uart_errors++;
        }
    }
    Console_ExitCritical(primask);

    Console_StartNext();
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    Console_CompleteActive(huart, 0U);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    Console_CompleteActive(huart, 1U);
}

int Console_IsIdle(void)
{
    return ((console_read_sequence == console_write_sequence) &&
            (console_tx_busy == 0U));
}

void Console_Flush(void)
{
    while (!Console_IsIdle())
    {
        Console_StartNext();
    }
}

uint32_t Console_GetDroppedCount(void)
{
    return console_dropped_messages;
}

uint32_t Console_GetUartErrorCount(void)
{
    return console_uart_errors;
}
