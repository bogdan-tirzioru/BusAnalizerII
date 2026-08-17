#ifndef USB_DIAG_H
#define USB_DIAG_H

#include <stdint.h>
#include "usbd_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Print one snapshot of the STM32 USB OTG HS core, PCD configuration,
 * and USB Device stack state.
 *
 * This intentionally uses only documented STM32 USB core registers.
 * The STM32H735 does not expose a supported ULPI PHY register viewport,
 * so this diagnostic does not attempt direct USB3300 register reads.
 */
void USB_Diag_Dump(const char *reason, const USBD_HandleTypeDef *pdev);

/*
 * Observe the first seconds after USBD_Start(). USB interrupts remain active
 * while this function waits, so enumeration can proceed normally. A snapshot
 * is printed when meaningful state changes are detected, plus a final dump.
 */
void USB_Diag_RunStartupWindow(const USBD_HandleTypeDef *pdev,
                               uint32_t duration_ms,
                               uint32_t sample_interval_ms);

#ifdef __cplusplus
}
#endif

#endif /* USB_DIAG_H */
