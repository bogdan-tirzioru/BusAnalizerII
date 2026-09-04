#ifndef BAII_PROTOCOL_SELFTEST_H
#define BAII_PROTOCOL_SELFTEST_H

/*
 * The project's embedded printf does not render %llu correctly.  The wrapper
 * console path.
 */
int BAII_SelfTest_Printf(const char *format, ...);

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
