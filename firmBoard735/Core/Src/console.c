#include "console.h"

#include <stdio.h>
#include <errno.h>

static UART_HandleTypeDef *console_uart = NULL;


void Console_Init(UART_HandleTypeDef *huart)
{
    console_uart = huart;

    /*
     * Useful for bring-up/debug logging:
     * don't let libc buffer stdout/stderr.
     */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}


/*
 * Retarget newlib stdout/stderr to UART.
 *
 * syscalls.c contains a weak _write(), so this implementation
 * overrides it.
 */
int _write(int file, char *ptr, int len)
{
    (void)file;

    if ((console_uart == NULL) || (ptr == NULL) || (len <= 0))
    {
        errno = EIO;
        return -1;
    }

    if (HAL_UART_Transmit(console_uart,
                          (uint8_t *)ptr,
                          (uint16_t)len,
                          HAL_MAX_DELAY) != HAL_OK)
    {
        errno = EIO;
        return -1;
    }

    return len;
}
