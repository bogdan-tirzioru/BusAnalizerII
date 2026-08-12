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

#endif /* CONSOLE_H */