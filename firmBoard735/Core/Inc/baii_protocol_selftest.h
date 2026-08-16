#ifndef BAII_PROTOCOL_SELFTEST_H
#define BAII_PROTOCOL_SELFTEST_H

/*
 * baii_protocol_selftest.c includes this header before main.h, while main.c
 * reaches it after main.h through hyperram_capture.h.  Use that include order
 * to keep the printf compatibility wrapper private to the self-test source.
 *
 * The project's embedded printf does not render %llu correctly.  The wrapper
 * handles that one format locally without changing the protocol or the normal
 * console path.
 */
#if !defined(BAII_PROTOCOL_SELFTEST_ON_BOOT)
#include <stdio.h>
int BAII_SelfTest_Printf(const char *format, ...);
#define printf BAII_SelfTest_Printf
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Temporary bring-up test for BusAnalyzerII host protocol v0.1.
 *
 * Tests GET_INFO, GET/SET_RTC_TIME and CAN bitrate reconfiguration before
 * USB transport is introduced. The hardware portions expect an external CAN
 * generator as described by the console prompts.
 */
void BAII_Protocol_SelfTest(void);

#ifdef __cplusplus
}
#endif

#endif /* BAII_PROTOCOL_SELFTEST_H */
