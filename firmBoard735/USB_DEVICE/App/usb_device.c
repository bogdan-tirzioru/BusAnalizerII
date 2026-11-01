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
#include "main.h"
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
 * External Waveshare USB3300: RESET is active HIGH, not NRST polarity.
 * PC7 is asserted by MX_GPIO_Init(). Keep STP high while the PHY starts;
 * HAL_PCD_MspInit() hands STP back to the ULPI alternate function later.
 *
 * USB3300 datasheet sections 6.1.9.2/3 and 6.1.11 require an active CLKOUT
 * before RESET release, and stable PHY inputs for two clocks afterwards.
 * The 10 ms guard allows startup/wake-up with valid power and a working
 * module crystal. Verify the 60 MHz clock on hardware; this is not a
 * clock/power detector and does not restore the ULPI registers to POR.
 */
static void USB_ExternalPhyReset(void)
{
  GPIO_InitTypeDef gpio = {0};

  HAL_GPIO_WritePin(USB_PHY_RESET_GPIO_Port, USB_PHY_RESET_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_SET);
  gpio.Pin = GPIO_PIN_0; /* ULPI STP: startup protection / wake request */
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &gpio);

  printf("USB PHY: PC7 RESET asserted, STP high; startup guard 10 ms\r\n");
  HAL_Delay(10U);
  HAL_GPIO_WritePin(USB_PHY_RESET_GPIO_Port, USB_PHY_RESET_Pin, GPIO_PIN_RESET);
  HAL_Delay(1U); /* Keep STP and data inputs stable after RESET release. */
  printf("USB PHY: PC7 RESET released low; recovery guard 1 ms complete\r\n");
}

/* Keep the current attach sequence until its role in enumeration is verified. */
static void USB_DiagReconnect(void)
{
  if (HAL_PCD_DevDisconnect(&hpcd_USB_OTG_HS) != HAL_OK)
  {
    printf("USB: disconnect failed\r\n");
    return;
  }
  HAL_Delay(100U);
  if (HAL_PCD_DevConnect(&hpcd_USB_OTG_HS) != HAL_OK)
  {
    printf("USB: reconnect failed\r\n");
  }
}

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

  USB_ExternalPhyReset();

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
  HAL_Delay(100U); /* Preserve pre-reconnect timing pending A/B tests. */
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

