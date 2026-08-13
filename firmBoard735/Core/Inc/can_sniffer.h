#ifndef CAN_SNIFFER_H
#define CAN_SNIFFER_H

#include <stdint.h>

void CAN_Sniffer_Init(void);
void CAN_Sniffer_Process(void);

uint32_t CAN_Sniffer_GetRxCount(void);
uint32_t CAN_Sniffer_GetErrorCount(void);
uint32_t CAN_Sniffer_GetBufferedCount(void);
uint32_t CAN_Sniffer_GetDroppedCount(void);
void CAN_Sniffer_DumpBufferedFrames(uint32_t count);
uint32_t CAN_Sniffer_GetFifoLostEvents(void);
uint32_t CAN_Sniffer_GetMaxFifoFill(void);
uint32_t CAN_Sniffer_GetSequenceErrors(void);
void CAN_Sniffer_StressConsume(void);
uint32_t CAN_Sniffer_GetConsumedCount(void);
uint64_t CAN_Sniffer_GetCaptureCycles(void);
uint32_t CAN_Sniffer_GetMeasuredFrames(void);

#endif
