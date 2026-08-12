#include "console.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>


#define CONSOLE_TX_BUFFER_SIZE    4096U


/*
 * Console UART.
 */
static UART_HandleTypeDef *console_uart = NULL;


/*
 * DMA source buffer.
 *
 * 32-byte alignment is useful for STM32H7 and will also
 * help later if D-cache is enabled.
 */
static uint8_t console_tx_buffer[CONSOLE_TX_BUFFER_SIZE]
    __attribute__((aligned(32)));


/*
 * Ring buffer state.
 *
 * head:
 *   next position where CPU writes data
 *
 * tail:
 *   first byte waiting to be transmitted
 */
static volatile uint32_t console_tx_head = 0U;
static volatile uint32_t console_tx_tail = 0U;


/*
 * Number of bytes currently owned by DMA.
 */
static volatile uint32_t console_dma_length = 0U;


/*
 * 0 = DMA idle
 * 1 = DMA transmission active
 */
static volatile uint8_t console_dma_active = 0U;


/*
 * ------------------------------------------------------------
 * Critical section helpers
 * ------------------------------------------------------------
 */

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


/*
 * ------------------------------------------------------------
 * Calculate unused ring-buffer space.
 *
 * One byte is intentionally left unused so:
 *
 * head == tail
 *
 * always means EMPTY.
 * ------------------------------------------------------------
 */

static uint32_t Console_FreeSpace(void)
{
    uint32_t head = console_tx_head;
    uint32_t tail = console_tx_tail;

    if (head >= tail)
    {
        return CONSOLE_TX_BUFFER_SIZE -
               (head - tail) -
               1U;
    }

    return tail - head - 1U;
}


/*
 * ------------------------------------------------------------
 * Start one contiguous DMA transaction.
 *
 * A ring buffer can wrap around, but DMA wants one contiguous
 * memory block. Therefore each DMA transaction runs either:
 *
 * tail -> head
 *
 * or
 *
 * tail -> end of buffer
 *
 * The completion callback starts another DMA transaction if
 * more data remains.
 * ------------------------------------------------------------
 */

static void Console_StartTx(void)
{
    uint32_t primask;
    uint32_t length;
    uint8_t *data;


    primask = Console_EnterCritical();


    if ((console_uart == NULL) ||
        (console_uart->hdmatx == NULL) ||
        (console_dma_active != 0U) ||
        (console_tx_head == console_tx_tail))
    {
        Console_ExitCritical(primask);
        return;
    }


    /*
     * Determine contiguous amount available from tail.
     */
    if (console_tx_head > console_tx_tail)
    {
        length = console_tx_head - console_tx_tail;
    }
    else
    {
        length = CONSOLE_TX_BUFFER_SIZE -
                 console_tx_tail;
    }


    data = &console_tx_buffer[console_tx_tail];

    console_dma_length = length;
    console_dma_active = 1U;


    Console_ExitCritical(primask);


    /*
     * DMA1 Stream0 is linked to USART1_TX by CubeMX.
     */
    if (HAL_UART_Transmit_DMA(console_uart,
                              data,
                              (uint16_t)length) != HAL_OK)
    {
        /*
         * Do NOT remove data from the ring.
         * Leave it queued so a later call can retry.
         */
        primask = Console_EnterCritical();

        console_dma_length = 0U;
        console_dma_active = 0U;

        Console_ExitCritical(primask);
    }
}


/*
 * ------------------------------------------------------------
 * Console initialization
 * ------------------------------------------------------------
 */

void Console_Init(UART_HandleTypeDef *huart)
{
    uint32_t primask;

    primask = Console_EnterCritical();

    console_uart = huart;

    console_tx_head = 0U;
    console_tx_tail = 0U;

    console_dma_length = 0U;
    console_dma_active = 0U;

    Console_ExitCritical(primask);


    /*
     * Let _write() receive printf output immediately.
     */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}


/*
 * ------------------------------------------------------------
 * newlib stdout/stderr backend
 *
 * printf()
 *    |
 *    +--> _write()
 *            |
 *            +--> ring buffer
 *                    |
 *                    +--> UART DMA
 * ------------------------------------------------------------
 */

int _write(int file, char *ptr, int len)
{
    int written = 0;

    (void)file;


    if ((console_uart == NULL) ||
        (console_uart->hdmatx == NULL) ||
        (ptr == NULL) ||
        (len <= 0))
    {
        errno = EIO;
        return -1;
    }


    while (written < len)
    {
        uint32_t free_space;
        uint32_t copy_length;
        uint32_t first_length;
        uint32_t second_length;
        uint32_t head;
        uint32_t new_head;
        uint32_t primask;


        free_space = Console_FreeSpace();


        /*
         * Ring buffer full.
         *
         * DMA is already draining it. Wait until some space
         * becomes available.
         */
        if (free_space == 0U)
        {
            Console_StartTx();

            /*
             * printf() from ISR context is intentionally
             * unsupported. Waiting here from an ISR would
             * prevent the DMA completion interrupt.
             */
            if ((__get_IPSR() != 0U) ||
                (__get_PRIMASK() != 0U))
            {
                errno = EAGAIN;

                if (written > 0)
                {
                    return written;
                }

                return -1;
            }

            continue;
        }


        copy_length = (uint32_t)(len - written);

        if (copy_length > free_space)
        {
            copy_length = free_space;
        }


        /*
         * Snapshot current producer position.
         */
        head = console_tx_head;


        /*
         * First part: head -> end of ring buffer.
         */
        first_length = CONSOLE_TX_BUFFER_SIZE - head;

        if (first_length > copy_length)
        {
            first_length = copy_length;
        }


        memcpy(&console_tx_buffer[head],
               &ptr[written],
               first_length);


        /*
         * Second part if the write wraps around.
         */
        second_length = copy_length - first_length;

        if (second_length > 0U)
        {
            memcpy(&console_tx_buffer[0],
                   &ptr[written + first_length],
                   second_length);
        }


        new_head = head + copy_length;

        if (new_head >= CONSOLE_TX_BUFFER_SIZE)
        {
            new_head -= CONSOLE_TX_BUFFER_SIZE;
        }


        /*
         * Publish newly written data only after memcpy()
         * completed.
         */
        primask = Console_EnterCritical();

        console_tx_head = new_head;

        Console_ExitCritical(primask);


        written += (int)copy_length;


        /*
         * Start DMA if it isn't already running.
         */
        Console_StartTx();
    }


    return written;
}


/*
 * ------------------------------------------------------------
 * UART DMA transmission complete
 *
 * HAL calls this after the DMA transfer AND UART transmission
 * completion sequence.
 * ------------------------------------------------------------
 */

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    uint32_t primask;


    if (huart != console_uart)
    {
        return;
    }


    primask = Console_EnterCritical();


    console_tx_tail += console_dma_length;

    if (console_tx_tail >= CONSOLE_TX_BUFFER_SIZE)
    {
        console_tx_tail -= CONSOLE_TX_BUFFER_SIZE;
    }


    console_dma_length = 0U;
    console_dma_active = 0U;


    Console_ExitCritical(primask);


    /*
     * There may already be more printf() data waiting.
     */
    Console_StartTx();
}


/*
 * ------------------------------------------------------------
 * Console status
 * ------------------------------------------------------------
 */

int Console_IsIdle(void)
{
    return ((console_tx_head == console_tx_tail) &&
            (console_dma_active == 0U));
}


/*
 * ------------------------------------------------------------
 * Wait for everything to leave USART.
 * ------------------------------------------------------------
 */

void Console_Flush(void)
{
    /*
     * This function must not be called from interrupt context.
     */
    while (!Console_IsIdle())
    {
        Console_StartTx();
    }
}
