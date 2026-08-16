#ifndef HYPERRAM_CAPTURE_H
#define HYPERRAM_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

void HyperRAM_Capture_Init(void);
void HyperRAM_Capture_Process(void);

uint32_t HyperRAM_Capture_GetStoredCount(void);
uint32_t HyperRAM_Capture_GetWriteErrors(void);
uint32_t HyperRAM_Capture_GetWriteLostFrames(void);
uint32_t HyperRAM_Capture_GetWrapCount(void);

bool HyperRAM_Capture_IsVerifyDone(void);
bool HyperRAM_Capture_IsVerifyPassed(void);
uint32_t HyperRAM_Capture_GetVerifiedCount(void);

/*
 * Temporary protocol bring-up hook.
 *
 * main.c includes main.h before this header, so the self-test switch is
 * visible there. hyperram_capture.c includes this header before main.h, so its
 * real HyperRAM_Capture_Init() definition remains untouched.
 *
 * This keeps CubeMX-generated main.c unchanged while making the temporary
 * self-test easy to disable from the USER CODE section in main.h.
 */
#if defined(BAII_PROTOCOL_SELFTEST_ON_BOOT)
#include "baii_protocol_selftest.h"

static inline void HyperRAM_Capture_Init_WithProtocolSelfTest(void)
{
    HyperRAM_Capture_Init();
    BAII_Protocol_SelfTest();
}

#define HyperRAM_Capture_Init() \
    HyperRAM_Capture_Init_WithProtocolSelfTest()
#endif

#endif
