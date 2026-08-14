#ifndef HYPERRAM_CAPTURE_H
#define HYPERRAM_CAPTURE_H

#include <stdint.h>

void HyperRAM_Capture_Init(void);
void HyperRAM_Capture_Process(void);

uint32_t HyperRAM_Capture_GetStoredCount(void);
uint32_t HyperRAM_Capture_GetWriteErrors(void);
uint32_t HyperRAM_Capture_GetWrapCount(void);

#endif
