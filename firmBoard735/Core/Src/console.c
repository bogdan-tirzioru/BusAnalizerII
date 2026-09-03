#include "console.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CONSOLE_MESSAGE_SIZE     320U
#define CONSOLE_QUEUE_CAPACITY   16U

_Static_assert((CONSOLE_QUEUE_CAPACITY & (CONSOLE_QUEUE_CAPACITY - 1U)) == 0U,
               "console queue capacity must be a power of two");

#if defined(__GNUC__)
#define CONSOLE_DMA_ALIGNED __attribute__((aligned(32)))
#else
#define CONSOLE_DMA_ALIGNED
#endif

static UART_HandleTypeDef *console_uart;
static uint8_t console_messages[CONSOLE_QUEUE_CAPACITY][CONSOLE_MESSAGE_SIZE]
    CONSOLE_DMA_ALIGNED;
static uint16_t console_lengths[CONSOLE_QUEUE_CAPACITY];
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

static void Console_RecordDrop(void)
{
    uint32_t primask = Console_EnterCritical();

    console_dropped_messages++;
    Console_ExitCritical(primask);
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

static int Console_Queue(const uint8_t *message, uint32_t length)
{
    uint32_t index;
    uint32_t write_sequence;

    if ((console_uart == NULL) ||
        (console_uart->hdmatx == NULL) ||
        (message == NULL) ||
        (length == 0U))
    {
        return 0;
    }

    if (length > CONSOLE_MESSAGE_SIZE)
    {
        Console_RecordDrop();
        return 0;
    }

    write_sequence = console_write_sequence;
    if ((write_sequence - console_read_sequence) >= CONSOLE_QUEUE_CAPACITY)
    {
        Console_RecordDrop();
        return 0;
    }

    index = write_sequence & (CONSOLE_QUEUE_CAPACITY - 1U);
    (void)memcpy(console_messages[index], message, length);
    console_lengths[index] = (uint16_t)length;

    /* Publish only after the message and its length are visible to the ISR. */
    __DMB();
    console_write_sequence = write_sequence + 1U;
    Console_StartNext();

    return 1;
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

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

/* newlib stdout/stderr backend. Log overload must never block capture. */
int _write(int file, char *ptr, int len)
{
    (void)file;

    if ((ptr == NULL) || (len <= 0) ||
        (console_uart == NULL) || (console_uart->hdmatx == NULL))
    {
        errno = EIO;
        return -1;
    }

    /* Report success to stdio even when the diagnostic message was dropped.
     * The loss is observable through Console_GetDroppedCount(). */
    (void)Console_Queue((const uint8_t *)ptr, (uint32_t)len);
    return len;
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
