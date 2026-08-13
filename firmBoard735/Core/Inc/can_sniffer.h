#ifndef CAN_SNIFFER_H
#define CAN_SNIFFER_H

#include <stdint.h>

void CAN_Sniffer_Init(void);
void CAN_Sniffer_Process(void);

uint32_t CAN_Sniffer_GetRxCount(void);
uint32_t CAN_Sniffer_GetErrorCount(void);

#endif
