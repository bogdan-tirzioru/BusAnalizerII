/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usb_device.c
  * @version        : v1.0_Cube
  * @brief          : This file implements the USB Device
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/

#include "usb_device.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "console.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* USER CODE END PV */

/* USER CODE BEGIN PFP */
/* Private function prototypes -----------------------------------------------*/

/* USER CODE END PFP */

/* USB Device Core handle declaration. */
USBD_HandleTypeDef hUsbDeviceHS;

/*
 * -- Insert your variables declaration here --
 */
/* USER CODE BEGIN 0 */
extern UART_HandleTypeDef huart1;
extern PCD_HandleTypeDef hpcd_USB_OTG_HS;
/* USER CODE END 0 */

/*
 * -- Insert your external function declaration here --
 */
/* USER CODE BEGIN 1 */
/*
 * Investigation-only register viewport, following ST's CubeF7 example:
 * Projects/STM32746G-Discovery/Examples/PWR/PWR_CurrentConsumption/Src/
 * stm32f7xx_lp_modes.c (USB_ULPI_Read).
 *
 * IMPORTANT: H725/H735 CMSIS reserves USB base + 0x034. This mechanism is
 * NOT documented/guaranteed on these parts. A timeout is inconclusive, not
 * evidence that the USB3300 or ULPI wiring is defective. Set the switch to
 * 0 for controller snapshots only. Never use an F4/F7 hard-coded USB base.
 */
#ifndef BA2_ULPI_EXPERIMENTAL_READS
#define BA2_ULPI_EXPERIMENTAL_READS 0
#endif

static void USB_DiagController(const char *stage)
{
  USB_OTG_GlobalTypeDef *USBx = USB_OTG_HS;
  uint32_t USBx_BASE = (uint32_t)USBx;
  uint32_t dctl = USBx_DEVICE->DCTL;
  uint32_t gotgctl = USBx->GOTGCTL;
  uint32_t pcgcctl = USBx_PCGCCTL;
  uint32_t pa = GPIOA->IDR;
  uint32_t pc = GPIOC->IDR;

  printf("USB DIAG: %s t=%lu ms\r\n", stage, (unsigned long)HAL_GetTick());
  printf("  GUSBCFG=%08lX GOTGCTL=%08lX GRSTCTL=%08lX\r\n",
         (unsigned long)USBx->GUSBCFG, (unsigned long)gotgctl,
         (unsigned long)USBx->GRSTCTL);
  printf("  DCTL=%08lX DCFG=%08lX DSTS=%08lX PCGCCTL=%08lX\r\n",
         (unsigned long)dctl, (unsigned long)USBx_DEVICE->DCFG,
         (unsigned long)USBx_DEVICE->DSTS, (unsigned long)pcgcctl);
  printf("  GINTSTS=%08lX GINTMSK=%08lX GCCFG=%08lX\r\n",
         (unsigned long)USBx->GINTSTS, (unsigned long)USBx->GINTMSK,
         (unsigned long)USBx->GCCFG);
  printf("  SDIS=%u BSVLD=%u BVALOEN=%u BVALOVAL=%u\r\n",
         (unsigned)((dctl & USB_OTG_DCTL_SDIS) != 0U),
         (unsigned)((gotgctl & USB_OTG_GOTGCTL_BSESVLD) != 0U),
         (unsigned)((gotgctl & USB_OTG_GOTGCTL_BVALOEN) != 0U),
         (unsigned)((gotgctl & USB_OTG_GOTGCTL_BVALOVAL) != 0U));
  printf("  GPIO instant: STP=%u CK=%u NXT=%u DIR=%u (not a trace)\r\n",
         (unsigned)((pc & GPIO_PIN_0) != 0U),
         (unsigned)((pa & GPIO_PIN_5) != 0U),
         (unsigned)((pc & GPIO_PIN_3) != 0U),
         (unsigned)((pc & GPIO_PIN_2) != 0U));
  printf("  USB device state=%u address=%u (BSVLD may be overridden)\r\n",
         (unsigned)hUsbDeviceHS.dev_state,
         (unsigned)hUsbDeviceHS.dev_address);
}


/* One-shot attach experiment; no PHY reset, viewport access or stack restart. */
static void USB_DiagReconnect(void)
{
  HAL_StatusTypeDef status;
  uint32_t at_ms = HAL_GetTick();

  status = HAL_PCD_DevDisconnect(&hpcd_USB_OTG_HS);
  printf("USB RECONNECT: disconnect t=%lu ms status=%u\r\n",
         (unsigned long)at_ms, (unsigned)status);
  USB_DiagController("after forced disconnect (expect SDIS=1)");
  if (status != HAL_OK)
  {
    printf("USB RECONNECT: aborted - disconnect failed\r\n");
    return;
  }

  HAL_Delay(100U);
  at_ms = HAL_GetTick();
  status = HAL_PCD_DevConnect(&hpcd_USB_OTG_HS);
  printf("USB RECONNECT: connect t=%lu ms status=%u\r\n",
         (unsigned long)at_ms, (unsigned)status);
  USB_DiagController("after forced reconnect (expect SDIS=0)");
  if (status != HAL_OK)
  {
    printf("USB RECONNECT: connect failed\r\n");
    return;
  }

  HAL_Delay(100U);
  USB_DiagController("100 ms after forced reconnect");
}

#if BA2_ULPI_EXPERIMENTAL_READS
#define BA2_ULPI_NEW       (1UL << 25)
#define BA2_ULPI_BUSY      (1UL << 26)
#define BA2_ULPI_DONE      (1UL << 27)
#define BA2_ULPI_TIMEOUT_MS 10U
#define BA2_ULPI_POLL_LIMIT 100000U

static HAL_StatusTypeDef USB_DiagReadPhy(uint8_t address, uint8_t *value,
                                       uint32_t *raw)
{
  volatile uint32_t *viewport =
      (volatile uint32_t *)((uintptr_t)USB_OTG_HS + 0x034U);
  uint32_t started;

  *raw = *viewport;
  if ((*raw & BA2_ULPI_BUSY) != 0U)
  {
    return HAL_BUSY; /* Do not overwrite an outstanding access. */
  }

  started = HAL_GetTick();
  *viewport = BA2_ULPI_NEW | ((uint32_t)address << 16);
  /* No PHY register writes; the write above requests a register READ. */
  for (uint32_t polls = 0U; polls < BA2_ULPI_POLL_LIMIT; ++polls)
  {
    *raw = *viewport;
    if (((*raw & BA2_ULPI_DONE) != 0U) &&
        ((*raw & BA2_ULPI_BUSY) == 0U))
    {
      *value = (uint8_t)(*raw & 0xFFU);
      return HAL_OK;
    }
    if ((uint32_t)(HAL_GetTick() - started) >= BA2_ULPI_TIMEOUT_MS)
    {
      return HAL_TIMEOUT;
    }
  }
  return HAL_TIMEOUT; /* Also bounded if the HAL tick stops. */
}

static void USB_DiagPhy(void)
{
  static const uint8_t addresses[] =
      {0x00U, 0x01U, 0x02U, 0x03U, 0x00U, 0x01U, 0x02U, 0x03U,
       0x04U, 0x07U, 0x0AU, 0x13U, 0x15U};
  uint8_t values[sizeof(addresses)] = {0};
  uint32_t raw = 0U;

  printf("USB PHY: EXPERIMENTAL reads, undocumented H725/H735 viewport\r\n");
  printf("USB PHY: start t=%lu ms; timeout does NOT prove PHY failure\r\n",
         (unsigned long)HAL_GetTick());
  for (uint32_t i = 0U; i < sizeof(addresses); ++i)
  {
    HAL_StatusTypeDef status = USB_DiagReadPhy(addresses[i], &values[i], &raw);
    if (status != HAL_OK)
    {
      printf("USB PHY: addr=%02X %s raw=%08lX; remaining reads skipped\r\n",
             (unsigned)addresses[i], status == HAL_BUSY ? "BUSY" : "TIMEOUT",
             (unsigned long)raw);
      printf("USB PHY: INCONCLUSIVE - viewport support or bus access\r\n");
      return;
    }
    printf("USB PHY: addr=%02X value=%02X raw=%08lX\r\n",
           (unsigned)addresses[i], (unsigned)values[i], (unsigned long)raw);

    /* Reject unsupported/stale/unexpected readback before interpreting controls. */
    if (i == 7U)
    {
      uint16_t vid = (uint16_t)((uint16_t)values[1] << 8) | values[0];
      uint16_t pid = (uint16_t)((uint16_t)values[3] << 8) | values[2];
      if ((vid != 0x0424U) || (pid != 0x0004U) ||
          (values[0] != values[4]) || (values[1] != values[5]) ||
          (values[2] != values[6]) || (values[3] != values[7]))
      {
        printf("USB PHY: ID UNVERIFIED VID=%04X PID=%04X; controls skipped\r\n",
               (unsigned)vid, (unsigned)pid);
        return;
      }
      printf("USB PHY: USB3300 ID matched twice (0424:0004)\r\n");
    }
  }
  printf("USB PHY: FUNC=%02X IFACE=%02X OTG=%02X STATUS=%02X DEBUG=%02X\r\n",
         (unsigned)values[8], (unsigned)values[9], (unsigned)values[10],
         (unsigned)values[11], (unsigned)values[12]);
  printf("  XcvrSelect=%u TermSelect=%u OpMode=%u Reset=%u SuspendM=%u\r\n",
         (unsigned)(values[8] & 3U), (unsigned)((values[8] >> 2) & 1U),
         (unsigned)((values[8] >> 3) & 3U), (unsigned)((values[8] >> 5) & 1U),
         (unsigned)((values[8] >> 6) & 1U));
  printf("  DPpulldown=%u DMpulldown=%u UseExternalVbus=%u\r\n",
         (unsigned)((values[10] >> 1) & 1U),
         (unsigned)((values[10] >> 2) & 1U),
         (unsigned)((values[10] >> 7) & 1U));
}
#endif

/* USER CODE END 1 */

/**
  * Init USB device Library, add supported class and start the library
  * @retval None
  */
void MX_USB_DEVICE_Init(void)
{
  /* USER CODE BEGIN USB_DEVICE_Init_PreTreatment */
  /*
   * USB bring-up diagnostics need printf() before the USB stack starts.
   * USART1 and its DMA are already initialized by main() at this point.
   */
  Console_Init(&huart1);
  printf("USB: starting HS/ULPI initialization\r\n");

  /*
   * STM32H735 PC2_C and PC3_C reach the digital PC2/PC3 functions through
   * internal analog switches. ULPI DIR and NXT use these two _C pads on this
   * package, so explicitly close both switches for deterministic bring-up.
   * CLOSE is encoded as 0 in the H7 HAL. This is harmless if the package
   * reset state already has the switches closed.
   */
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  HAL_SYSCFG_AnalogSwitchConfig(SYSCFG_SWITCH_PC2, SYSCFG_SWITCH_PC2_CLOSE);
  HAL_SYSCFG_AnalogSwitchConfig(SYSCFG_SWITCH_PC3, SYSCFG_SWITCH_PC3_CLOSE);
  printf("USB: PC2_C/PC3_C analog switches forced closed, PMCR=0x%08lX\r\n",
         (unsigned long)SYSCFG->PMCR);

  /* USER CODE END USB_DEVICE_Init_PreTreatment */

  /* Init Device Library, add supported class and start the library. */
  if (USBD_Init(&hUsbDeviceHS, &HS_Desc, DEVICE_HS) != USBD_OK)
  {
    Error_Handler();
  }
  printf("USB: USBD_Init OK\r\n");

  if (USBD_RegisterClass(&hUsbDeviceHS, &USBD_CDC) != USBD_OK)
  {
    Error_Handler();
  }
  printf("USB: USBD_RegisterClass OK\r\n");

  if (USBD_CDC_RegisterInterface(&hUsbDeviceHS, &USBD_Interface_fops_HS) != USBD_OK)
  {
    Error_Handler();
  }
  printf("USB: USBD_CDC_RegisterInterface OK\r\n");

  if (USBD_Start(&hUsbDeviceHS) != USBD_OK)
  {
    Error_Handler();
  }
  printf("USB: USBD_Start OK\r\n");

  /* USER CODE BEGIN USB_DEVICE_Init_PostTreatment */
  HAL_PWREx_EnableUSBVoltageDetector();
  USB_DiagController("after USBD_Start");
#if BA2_ULPI_EXPERIMENTAL_READS
  USB_DiagPhy();
#else
  printf("USB PHY: experimental reads disabled; controller snapshots only\r\n");
#endif
  HAL_Delay(100U); /* Investigation-only window for host attach/reset. */
  USB_DiagController("100 ms after diagnostics");
  USB_DiagReconnect();
  printf("USB: HS/ULPI initialization complete\r\n");

  /* USER CODE END USB_DEVICE_Init_PostTreatment */
}

/**
  * @}
  */

/**
  * @}
  */
