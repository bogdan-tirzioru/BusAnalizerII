#ifndef BOARD_ID_H
#define BOARD_ID_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32h7xx_hal.h"

typedef enum
{
    BOARD_ID_UNKNOWN = 0,

    BOARD_ID_BUSANALYZER_H725_01,
    BOARD_ID_BUSANALYZER_H735_01

} BoardID_t;

void BoardID_Init(void);

BoardID_t BoardID_Get(void);
const char *BoardID_GetName(void);

void BoardID_Print(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_ID_H */
