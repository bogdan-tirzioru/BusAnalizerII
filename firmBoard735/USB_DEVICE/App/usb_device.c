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

/*
 * STM32 OTG ULPI PHY access viewport.
 *
 * The STM32H7 HAL/CMSIS headers do not expose this diagnostic register as a
 * named member. Keep the access local to this bring-up diagnostic and derive
 * the address from the active OTG HS instance rather than hard-coding the
 * peripheral base address.
 */
#define USB_ULPI_PHYCR_OFFSET       0x034U
#define USB_ULPI_PHYCR_DATA_MASK    0x000000FFU
#define USB_ULPI_PHYCR_ADDR_SHIFT   16U
#define USB_ULPI_PHYCR_RW           0x00400000U
#define USB_ULPI_PHYCR_NEW          0x02000000U
#define USB_ULPI_PHYCR_BUSY         0x04000000U
#define USB_ULPI_PHYCR_DONE         0x08000000U
#define USB_ULPI_PHYCR_TIMEOUT      1000000U

#define USB3300_REG_VENDOR_ID_LOW   0x00U
#define USB3300_REG_VENDOR_ID_HIGH  0x01U
#define USB3300_REG_PRODUCT_ID_LOW  0x02U
#define USB3300_REG_PRODUCT_ID_HIGH 0x03U
#define USB3300_REG_SCRATCH         0x16U

#define USB3300_EXPECTED_VENDOR_ID  0x0424U
#define USB3300_EXPECTED_PRODUCT_ID 0x0004U

/* USER CODE END PV */

/* USER CODE BEGIN PFP */
/* Private function prototypes -----------------------------------------------*/
static HAL_StatusTypeDef USB_ULPI_ReadReg(uint8_t address, uint8_t *data);
static HAL_StatusTypeDef USB_ULPI_WriteReg(uint8_t address, uint8_t data);
static void USB_ULPI_CommunicationTest(void);

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
  * @brief  Read one USB3300 ULPI register through the STM32 OTG viewport.
  * @param  address 6-bit ULPI register address.
  * @param  data destination byte.
  * @retval HAL_OK on completed transaction, HAL_TIMEOUT otherwise.
  */
static HAL_StatusTypeDef USB_ULPI_ReadReg(uint8_t address, uint8_t *data)
{
  __IO uint32_t *phycr;
  uint32_t value;
  uint32_t timeout = USB_ULPI_PHYCR_TIMEOUT;

  if (data == NULL)
  {
    return HAL_ERROR;
  }

  phycr = (__IO uint32_t *)((uint32_t)USB_OTG_HS + USB_ULPI_PHYCR_OFFSET);

  *phycr = USB_ULPI_PHYCR_NEW |
           (((uint32_t)address & 0x3FU) << USB_ULPI_PHYCR_ADDR_SHIFT);

  do
  {
    value = *phycr;
    if ((value & USB_ULPI_PHYCR_DONE) != 0U)
    {
      *data = (uint8_t)(value & USB_ULPI_PHYCR_DATA_MASK);
      return HAL_OK;
    }
    timeout--;
  } while (timeout != 0U);

  printf("USB: ULPI READ timeout reg=0x%02X PHYCR=0x%08lX\r\n",
         (unsigned int)address,
         (unsigned long)(*phycr));
  return HAL_TIMEOUT;
}

/**
  * @brief  Write one USB3300 ULPI register through the STM32 OTG viewport.
  * @param  address 6-bit ULPI register address.
  * @param  data byte to write.
  * @retval HAL_OK on completed transaction, HAL_TIMEOUT otherwise.
  */
static HAL_StatusTypeDef USB_ULPI_WriteReg(uint8_t address, uint8_t data)
{
  __IO uint32_t *phycr;
  uint32_t value;
  uint32_t timeout = USB_ULPI_PHYCR_TIMEOUT;

  phycr = (__IO uint32_t *)((uint32_t)USB_OTG_HS + USB_ULPI_PHYCR_OFFSET);

  *phycr = USB_ULPI_PHYCR_NEW |
           USB_ULPI_PHYCR_RW |
           (((uint32_t)address & 0x3FU) << USB_ULPI_PHYCR_ADDR_SHIFT) |
           ((uint32_t)data & USB_ULPI_PHYCR_DATA_MASK);

  do
  {
    value = *phycr;
    if ((value & USB_ULPI_PHYCR_DONE) != 0U)
    {
      return HAL_OK;
    }
    timeout--;
  } while (timeout != 0U);

  printf("USB: ULPI WRITE timeout reg=0x%02X data=0x%02X PHYCR=0x%08lX\r\n",
         (unsigned int)address,
         (unsigned int)data,
         (unsigned long)(*phycr));
  return HAL_TIMEOUT;
}

/**
  * @brief  Prove bidirectional communication with the USB3300 PHY.
  *
  * Reads the four read-only identification bytes, then exercises the USB3300
  * Scratch register with complementary 0x55 and 0xAA patterns. The original
  * Scratch value is restored before normal USB initialization continues.
  */
static void USB_ULPI_CommunicationTest(void)
{
  uint8_t vid_low = 0U;
  uint8_t vid_high = 0U;
  uint8_t pid_low = 0U;
  uint8_t pid_high = 0U;
  uint8_t scratch_original = 0U;
  uint8_t scratch_55 = 0U;
  uint8_t scratch_aa = 0U;
  uint8_t scratch_restored = 0U;
  uint16_t vendor_id;
  uint16_t product_id;
  uint32_t failures = 0U;
  HAL_StatusTypeDef status;

  printf("\r\n--- USB3300 ULPI COMMUNICATION TEST ---\r\n");

  status = USB_ULPI_ReadReg(USB3300_REG_VENDOR_ID_LOW, &vid_low);
  if (status != HAL_OK) { failures++; }
  status = USB_ULPI_ReadReg(USB3300_REG_VENDOR_ID_HIGH, &vid_high);
  if (status != HAL_OK) { failures++; }
  status = USB_ULPI_ReadReg(USB3300_REG_PRODUCT_ID_LOW, &pid_low);
  if (status != HAL_OK) { failures++; }
  status = USB_ULPI_ReadReg(USB3300_REG_PRODUCT_ID_HIGH, &pid_high);
  if (status != HAL_OK) { failures++; }

  vendor_id = ((uint16_t)vid_high << 8) | vid_low;
  product_id = ((uint16_t)pid_high << 8) | pid_low;

  printf("ULPI VID bytes : %02X %02X -> 0x%04X (expected 0x%04X)\r\n",
         (unsigned int)vid_high,
         (unsigned int)vid_low,
         (unsigned int)vendor_id,
         (unsigned int)USB3300_EXPECTED_VENDOR_ID);
  printf("ULPI PID bytes : %02X %02X -> 0x%04X (expected 0x%04X)\r\n",
         (unsigned int)pid_high,
         (unsigned int)pid_low,
         (unsigned int)product_id,
         (unsigned int)USB3300_EXPECTED_PRODUCT_ID);

  if (vendor_id != USB3300_EXPECTED_VENDOR_ID) { failures++; }
  if (product_id != USB3300_EXPECTED_PRODUCT_ID) { failures++; }

  status = USB_ULPI_ReadReg(USB3300_REG_SCRATCH, &scratch_original);
  if (status != HAL_OK)
  {
    failures++;
  }
  else
  {
    status = USB_ULPI_WriteReg(USB3300_REG_SCRATCH, 0x55U);
    if (status != HAL_OK) { failures++; }
    status = USB_ULPI_ReadReg(USB3300_REG_SCRATCH, &scratch_55);
    if (status != HAL_OK) { failures++; }

    status = USB_ULPI_WriteReg(USB3300_REG_SCRATCH, 0xAAU);
    if (status != HAL_OK) { failures++; }
    status = USB_ULPI_ReadReg(USB3300_REG_SCRATCH, &scratch_aa);
    if (status != HAL_OK) { failures++; }

    /* Restore the PHY state even when one of the pattern comparisons fails. */
    status = USB_ULPI_WriteReg(USB3300_REG_SCRATCH, scratch_original);
    if (status != HAL_OK) { failures++; }
    status = USB_ULPI_ReadReg(USB3300_REG_SCRATCH, &scratch_restored);
    if (status != HAL_OK) { failures++; }

    printf("ULPI scratch   : original=0x%02X 55->0x%02X AA->0x%02X restore=0x%02X\r\n",
           (unsigned int)scratch_original,
           (unsigned int)scratch_55,
           (unsigned int)scratch_aa,
           (unsigned int)scratch_restored);

    if (scratch_55 != 0x55U) { failures++; }
    if (scratch_aa != 0xAAU) { failures++; }
    if (scratch_restored != scratch_original) { failures++; }
  }

  if (failures == 0U)
  {
    printf("USB3300 ULPI COMMUNICATION: PASS\r\n");
  }
  else
  {
    printf("USB3300 ULPI COMMUNICATION: FAIL (%lu checks)\r\n",
           (unsigned long)failures);
  }
  printf("--- END USB3300 ULPI COMMUNICATION TEST ---\r\n\r\n");
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

  /* USER CODE END USB_DEVICE_Init_PreTreatment */

  /* Init Device Library, add supported class and start the library. */
  if (USBD_Init(&hUsbDeviceHS, &HS_Desc, DEVICE_HS) != USBD_OK)
  {
    Error_Handler();
  }
  printf("USB: USBD_Init OK\r\n");

  /* USER CODE BEGIN USB_DEVICE_Init_ULPI_Diagnostic */
  /*
   * HAL_PCD_Init() has now configured the ULPI pins/clocks and initialized the
   * OTG core, while the device is still soft-disconnected. This is the clean
   * point to prove register communication with the external USB3300 PHY.
   */
  USB_ULPI_CommunicationTest();
  /* USER CODE END USB_DEVICE_Init_ULPI_Diagnostic */

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
