#ifndef CONSOLE_H
#define CONSOLE_H

#include "main.h"

void Console_Init(UART_HandleTypeDef *huart);

/*
 * Wait until all queued console data has physically
 * finished transmitting.
 *
 * Call only from normal/main context, not from an ISR.
 */
void Console_Flush(void);

/*
 * Returns non-zero when no console transmission
 * is pending.
 */
int Console_IsIdle(void);

/* Number of messages dropped because the queue was full, a message exceeded
 * the maximum length, or UART/DMA transmission failed. */
uint32_t Console_GetDroppedCount(void);

/* Number of UART/DMA start or transmission errors. */
uint32_t Console_GetUartErrorCount(void);

#endif /* CONSOLE_H */
