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
static uint32_t USB_ULPI_CLK_MeasureHz(void);
static void USB_ULPI_WakeTest(uint32_t *dir_before,
                              uint32_t *clk_before_hz,
                              uint32_t *dir_after,
                              uint32_t *clk_after_hz);

/* USER CODE END PFP */

/* USB Device Core handle declaration. */
USBD_HandleTypeDef hUsbDeviceHS;

/*
 * -- Insert your variables declaration here --
 */
/* USER CODE BEGIN 0 */
extern UART_HandleTypeDef huart1;
/* USER CODE END 0 */

/*
 * -- Insert your external function declaration here --
 */
/* USER CODE BEGIN 1 */

/**
  * @brief  Measure USB3300 ULPI CLKOUT on PA5 without using an oscilloscope.
  * @note   PA5 is temporarily switched from ULPI_CLK (AF10) to TIM2_ETR (AF1).
  *         TIM2 counts external rising edges for 10 ms, then PA5 and TIM2 are
  *         released so HAL_PCD_MspInit() can configure the normal ULPI AF.
  * @retval Measured CLKOUT frequency in Hz.
  */
static uint32_t USB_ULPI_CLK_MeasureHz(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  uint32_t tick;
  uint32_t edges;

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_TIM2_CLK_ENABLE();
  __HAL_RCC_TIM2_FORCE_RESET();
  __HAL_RCC_TIM2_RELEASE_RESET();

  /* PA5 = TIM2_CH1/TIM2_ETR, AF1. */
  GPIO_InitStruct.Pin = GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF1_TIM2;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* TIM2 external clock mode 2: counter clocked directly from ETR rising edges. */
  TIM2->CR1 = 0U;
  TIM2->CR2 = 0U;
  TIM2->SMCR = 0U;
  TIM2->DIER = 0U;
  TIM2->CCMR1 = 0U;
  TIM2->CCMR2 = 0U;
  TIM2->CCER = 0U;
  TIM2->PSC = 0U;
  TIM2->ARR = 0xFFFFFFFFU;
  TIM2->CNT = 0U;
  TIM2->SMCR = TIM_SMCR_ECE;
  TIM2->EGR = TIM_EGR_UG;
  TIM2->CNT = 0U;
  TIM2->CR1 = TIM_CR1_CEN;

  /* Align the 10 ms measurement window to a SysTick edge. */
  tick = HAL_GetTick();
  while (HAL_GetTick() == tick)
  {
  }

  TIM2->CNT = 0U;
  tick = HAL_GetTick();
  while ((HAL_GetTick() - tick) < 10U)
  {
  }

  edges = TIM2->CNT;

  TIM2->CR1 = 0U;
  HAL_GPIO_DeInit(GPIOA, GPIO_PIN_5);
  __HAL_RCC_TIM2_FORCE_RESET();
  __HAL_RCC_TIM2_RELEASE_RESET();
  __HAL_RCC_TIM2_CLK_DISABLE();

  /* 10 ms window: Hz = edges * 100. */
  return edges * 100U;
}

/**
  * @brief  Exercise the USB3300 ULPI warm-wakeup path and observe DIR/CLKOUT.
  * @note   PC0 (STP) and PC2_C (DIR) are used only for this diagnostic and are
  *         released before the normal USB PCD MSP initialization starts.
  */
static void USB_ULPI_WakeTest(uint32_t *dir_before,
                              uint32_t *clk_before_hz,
                              uint32_t *dir_after,
                              uint32_t *clk_after_hz)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  /* PC2_C reaches the PC2 digital input through the H735 analog switch. */
  HAL_SYSCFG_AnalogSwitchConfig(SYSCFG_SWITCH_PC2, SYSCFG_SWITCH_PC2_CLOSE);

  /* Observe DIR before disturbing STP. */
  GPIO_InitStruct.Pin = GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  *dir_before = (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_2) == GPIO_PIN_SET) ? 1U : 0U;
  *clk_before_hz = USB_ULPI_CLK_MeasureHz();

  /*
   * Warm wake request: prepare the output latch HIGH before changing PC0 to
   * output mode, then hold STP HIGH long enough for PHY suspend recovery.
   */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_SET);
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  HAL_Delay(5U);

  *dir_after = (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_2) == GPIO_PIN_SET) ? 1U : 0U;
  *clk_after_hz = USB_ULPI_CLK_MeasureHz();

  /* Return STP inactive and release temporary GPIO ownership. */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_RESET);
  HAL_Delay(1U);
  HAL_GPIO_DeInit(GPIOC, GPIO_PIN_0 | GPIO_PIN_2);
}

/* USER CODE END 1 */

/**
  * Init USB device Library, add supported class and start the library
  * @retval None
  */
void MX_USB_DEVICE_Init(void)
{
  /* USER CODE BEGIN USB_DEVICE_Init_PreTreatment */
  uint32_t dir_before;
  uint32_t dir_after;
  uint32_t ulpi_clk_before_hz;
  uint32_t ulpi_clk_after_hz;

  /*
   * USB bring-up diagnostics need printf() before the USB stack starts.
   * USART1 and its DMA are already initialized by main() at this point.
   */
  Console_Init(&huart1);
  printf("USB: starting HS/ULPI initialization\r\n");

  /*
   * Check whether the USB3300 is sleeping with CLKOUT stopped, request a warm
   * wake through STP, then verify both DIR and CLKOUT again using the MCU.
   */
  USB_ULPI_WakeTest(&dir_before,
                    &ulpi_clk_before_hz,
                    &dir_after,
                    &ulpi_clk_after_hz);

  printf("USB: ULPI pre-wake  DIR=%lu CLKOUT=%lu Hz\r\n",
         (unsigned long)dir_before,
         (unsigned long)ulpi_clk_before_hz);
  printf("USB: ULPI wake      STP=HIGH for 5 ms\r\n");
  printf("USB: ULPI post-wake DIR=%lu CLKOUT=%lu Hz\r\n",
         (unsigned long)dir_after,
         (unsigned long)ulpi_clk_after_hz);

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
  printf("USB: HS/ULPI initialization complete\r\n");

  /* USER CODE END USB_DEVICE_Init_PostTreatment */
}

/**
  * @}
  */

/**
  * @}
  */
