
#include "can_test.h"
#include "main.h"
#include "stm32h7xx_hal_fdcan.h"
#include "stm32h7xx_hal_uart.h"

#include <stdio.h>

extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan3;
extern UART_HandleTypeDef huart1;

FDCAN_FilterTypeDef can1_filter;
FDCAN_FilterTypeDef can3_filter;

FDCAN_TxHeaderTypeDef can_tx_header;
FDCAN_RxHeaderTypeDef can_rx_header;

uint8_t can_tx_data[8] = {0};
uint8_t can_rx_data[8] = {0};

uint32_t can_counter = 0;

/* 0 = nothing pending
 * 1 = FDCAN1 must transmit
 * 3 = FDCAN3 must transmit
 */
uint8_t can_pending_tx = 0;

uint32_t can_reply_due = 0;
uint32_t led_tick = 0;
FDCAN_ErrorCountersTypeDef fdcan1_errors;
FDCAN_ErrorCountersTypeDef fdcan3_errors;


void CAN_Test_Init(void)
{
	/* ----------------------------------------------------------
	   * FDCAN1 receives messages coming from FDCAN3
	   * ID = 0x321
	   * ---------------------------------------------------------- */

	  can1_filter.IdType       = FDCAN_STANDARD_ID;
	  can1_filter.FilterIndex  = 0;
	  can1_filter.FilterType   = FDCAN_FILTER_MASK;
	  can1_filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
	  can1_filter.FilterID1    = 0x321;
	  can1_filter.FilterID2    = 0x7FF;

	  if (HAL_FDCAN_ConfigFilter(&hfdcan1, &can1_filter) != HAL_OK)
	  {
	      Error_Handler();
	  }


	  /* ----------------------------------------------------------
	   * FDCAN3 receives messages coming from FDCAN1
	   * ID = 0x123
	   * ---------------------------------------------------------- */

	  can3_filter.IdType       = FDCAN_STANDARD_ID;
	  can3_filter.FilterIndex  = 0;
	  can3_filter.FilterType   = FDCAN_FILTER_MASK;
	  can3_filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
	  can3_filter.FilterID1    = 0x123;
	  can3_filter.FilterID2    = 0x7FF;

	  if (HAL_FDCAN_ConfigFilter(&hfdcan3, &can3_filter) != HAL_OK)
	  {
	      Error_Handler();
	  }


	  /* Reject everything except our filters */

	  if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1,
	                                   FDCAN_REJECT,
	                                   FDCAN_REJECT,
	                                   FDCAN_REJECT_REMOTE,
	                                   FDCAN_REJECT_REMOTE) != HAL_OK)
	  {
	      Error_Handler();
	  }

	  if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan3,
	                                   FDCAN_REJECT,
	                                   FDCAN_REJECT,
	                                   FDCAN_REJECT_REMOTE,
	                                   FDCAN_REJECT_REMOTE) != HAL_OK)
	  {
	      Error_Handler();
	  }


	  /* Common Classic CAN TX header */

	  can_tx_header.IdType              = FDCAN_STANDARD_ID;
	  can_tx_header.TxFrameType         = FDCAN_DATA_FRAME;
	  can_tx_header.DataLength          = FDCAN_DLC_BYTES_8;
	  can_tx_header.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
	  can_tx_header.BitRateSwitch       = FDCAN_BRS_OFF;
	  can_tx_header.FDFormat            = FDCAN_CLASSIC_CAN;
	  can_tx_header.TxEventFifoControl  = FDCAN_NO_TX_EVENTS;
	  can_tx_header.MessageMarker       = 0;


	  /* Start both CAN controllers */

	  if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK)
	  {
	      Error_Handler();
	  }

	  if (HAL_FDCAN_Start(&hfdcan3) != HAL_OK)
	  {
	      Error_Handler();
	  }


	  /* ----------------------------------------------------------
	   * Start ping-pong:
	   *
	   * FDCAN1 -> FDCAN3
	   * ID = 0x123
	   * counter = 0
	   * ---------------------------------------------------------- */

	  can_counter = 0;

	  can_tx_data[0] = (uint8_t)(can_counter);
	  can_tx_data[1] = (uint8_t)(can_counter >> 8);
	  can_tx_data[2] = (uint8_t)(can_counter >> 16);
	  can_tx_data[3] = (uint8_t)(can_counter >> 24);

	  can_tx_data[4] = 0;
	  can_tx_data[5] = 0;
	  can_tx_data[6] = 0;
	  can_tx_data[7] = 0;

	  can_tx_header.Identifier = 0x123;

	  if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1,
	                                    &can_tx_header,
	                                    can_tx_data) != HAL_OK)
	  {
	      Error_Handler();
	  }

  printf("CAN ping-pong started, period = 40 ms\r\n");
}
void CAN_Test_Process(void)
{
	uint32_t now = HAL_GetTick();


		  /* ==========================================================
		   * Did FDCAN3 receive the 0x123 frame from FDCAN1?
		   * ========================================================== */

		  if (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan3,
		                                   FDCAN_RX_FIFO0) > 0)
		  {
		      if (HAL_FDCAN_GetRxMessage(&hfdcan3,
		                                 FDCAN_RX_FIFO0,
		                                 &can_rx_header,
		                                 can_rx_data) != HAL_OK)
		      {
		          Error_Handler();
		      }

		      if (can_rx_header.Identifier == 0x123)
		      {
		          can_counter =
		                ((uint32_t)can_rx_data[0])
		              | ((uint32_t)can_rx_data[1] << 8)
		              | ((uint32_t)can_rx_data[2] << 16)
		              | ((uint32_t)can_rx_data[3] << 24);

		          can_counter++;

		          /* CAN2/FDCAN3 will reply after 40 ms */
		          can_reply_due = now + 40;
		          can_pending_tx = 3;
		      }
		  }


		  /* ==========================================================
		   * Did FDCAN1 receive the 0x321 frame from FDCAN3?
		   * ========================================================== */

		  if (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan1,
		                                   FDCAN_RX_FIFO0) > 0)
		  {
		      if (HAL_FDCAN_GetRxMessage(&hfdcan1,
		                                 FDCAN_RX_FIFO0,
		                                 &can_rx_header,
		                                 can_rx_data) != HAL_OK)
		      {
		          Error_Handler();
		      }

		      if (can_rx_header.Identifier == 0x321)
		      {
		          can_counter =
		                ((uint32_t)can_rx_data[0])
		              | ((uint32_t)can_rx_data[1] << 8)
		              | ((uint32_t)can_rx_data[2] << 16)
		              | ((uint32_t)can_rx_data[3] << 24);

		          can_counter++;

		          /* CAN1/FDCAN1 will reply after 40 ms */
		          can_reply_due = now + 40;
		          can_pending_tx = 1;
		      }
		  }


		  /* ==========================================================
		   * Time to send the scheduled reply?
		   * ========================================================== */

		  if ((can_pending_tx != 0) &&
		      ((int32_t)(now - can_reply_due) >= 0))
		  {
		      /* Put counter into first 4 data bytes */

		      can_tx_data[0] = (uint8_t)(can_counter);
		      can_tx_data[1] = (uint8_t)(can_counter >> 8);
		      can_tx_data[2] = (uint8_t)(can_counter >> 16);
		      can_tx_data[3] = (uint8_t)(can_counter >> 24);

		      can_tx_data[4] = 0;
		      can_tx_data[5] = 0;
		      can_tx_data[6] = 0;
		      can_tx_data[7] = 0;


		      if (can_pending_tx == 3)
		      {
		          /* FDCAN3 -> FDCAN1 */

		          can_tx_header.Identifier = 0x321;

		          if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan3,
		                                            &can_tx_header,
		                                            can_tx_data) != HAL_OK)
		          {
		              Error_Handler();
		          }
		      }
		      else
		      {
		          /* FDCAN1 -> FDCAN3 */

		          can_tx_header.Identifier = 0x123;

		          if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1,
		                                            &can_tx_header,
		                                            can_tx_data) != HAL_OK)
		          {
		              Error_Handler();
		          }
		      }

		      can_pending_tx = 0;


		      /* Print only once every 25 CAN messages.
		         25 x 40 ms ≈ 1 second */

		      if ((can_counter % 25) == 0)
		      {
		          printf("CAN=%lu | CAN1 TEC=%lu REC=%lu | CAN3 TEC=%lu REC=%lu\r\n",
		                 (unsigned long)can_counter,
		                 (unsigned long)fdcan1_errors.TxErrorCnt,
		                 (unsigned long)fdcan1_errors.RxErrorCnt,
		                 (unsigned long)fdcan3_errors.TxErrorCnt,
		                 (unsigned long)fdcan3_errors.RxErrorCnt);
		      }
		  }


		  /* LEDs remain alive, but without blocking CAN */

		  if ((now - led_tick) >= 500)
		  {
		      led_tick = now;

		      HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
		      HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
		  }

}
