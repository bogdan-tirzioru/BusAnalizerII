#include "can_sniffer.h"

#include "can_capture_buffer.h"
#include "console.h"
#include "main.h"

#include <stddef.h>
#include <string.h>

#define RX_ELEMENT_STDID_MASK 0x1FFC0000U
#define RX_ELEMENT_EXTID_MASK 0x1FFFFFFFU
#define RX_ELEMENT_RTR_MASK   0x20000000U
#define RX_ELEMENT_XTD_MASK   0x40000000U
#define RX_ELEMENT_ESI_MASK   0x80000000U
#define RX_ELEMENT_TS_MASK    0x0000FFFFU
#define RX_ELEMENT_DLC_MASK   0x000F0000U
#define RX_ELEMENT_BRS_MASK   0x00100000U
#define RX_ELEMENT_FDF_MASK   0x00200000U

extern FDCAN_HandleTypeDef hfdcan1;

static uint32_t rx_count;
static uint32_t error_count;
static uint32_t fifo_lost_events;
static uint32_t max_fifo_fill;
static uint32_t stress_consumed;
static uint32_t stress_sequence_errors;
static uint32_t stress_expected_counter;
static uint64_t capture_cycles;
static uint32_t capture_measured_frames;

static uint8_t CAN_DlcToLength(uint8_t dlc)
{
  static const uint8_t lengths[16] =
  {
    0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U,
    8U, 12U, 16U, 20U, 24U, 32U, 48U, 64U
  };

  return lengths[dlc & 0x0FU];
}

static bool CAN_Sniffer_ReadFifo0Direct(CAN_SnifferFrame *frame)
{
  uint32_t fifo_status;
  uint32_t get_index;
  volatile const uint32_t *element;
  uint32_t word0;
  uint32_t word1;
  uint8_t length;
  uint8_t stored_length;

  if (frame == NULL)
  {
    return false;
  }

  fifo_status = hfdcan1.Instance->RXF0S;
  if ((fifo_status & FDCAN_RXF0S_F0FL) == 0U)
  {
    return false;
  }

  get_index =
      (fifo_status & FDCAN_RXF0S_F0GI) >> FDCAN_RXF0S_F0GI_Pos;

  element = (volatile const uint32_t *)(uintptr_t)
      (hfdcan1.msgRam.RxFIFO0SA +
       (get_index * hfdcan1.Init.RxFifo0ElmtSize * 4U));

  word0 = element[0];
  word1 = element[1];

  frame->flags = 0U;

  if ((word0 & RX_ELEMENT_XTD_MASK) != 0U)
  {
    frame->id = word0 & RX_ELEMENT_EXTID_MASK;
    frame->flags |= CAN_FRAME_FLAG_EXTENDED;
  }
  else
  {
    frame->id = (word0 & RX_ELEMENT_STDID_MASK) >> 18U;
  }

  if ((word0 & RX_ELEMENT_RTR_MASK) != 0U)
  {
    frame->flags |= CAN_FRAME_FLAG_RTR;
  }
  if ((word0 & RX_ELEMENT_ESI_MASK) != 0U)
  {
    frame->flags |= CAN_FRAME_FLAG_ESI;
  }
  if ((word1 & RX_ELEMENT_FDF_MASK) != 0U)
  {
    frame->flags |= CAN_FRAME_FLAG_FD;
  }
  if ((word1 & RX_ELEMENT_BRS_MASK) != 0U)
  {
    frame->flags |= CAN_FRAME_FLAG_BRS;
  }

  frame->timestamp = (uint16_t)(word1 & RX_ELEMENT_TS_MASK);
  frame->dlc = (uint8_t)((word1 & RX_ELEMENT_DLC_MASK) >> 16U);

  if ((frame->flags & CAN_FRAME_FLAG_RTR) != 0U)
  {
    length = 0U;
    stored_length = 0U;
  }
  else if ((frame->flags & CAN_FRAME_FLAG_FD) != 0U)
  {
    length = CAN_DlcToLength(frame->dlc);
    stored_length = sizeof(frame->data);
  }
  else
  {
    length = (frame->dlc <= 8U) ? frame->dlc : 8U;
    stored_length = 8U;
  }

  if (length != 0U)
  {
    (void)memcpy(frame->data, (const void *)&element[2], length);
  }
  if (length < stored_length)
  {
    (void)memset(&frame->data[length], 0, stored_length - length);
  }

  hfdcan1.Instance->RXF0A = get_index;
  return true;
}

static void CAN_Sniffer_CycleCounter_Init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void CAN_Sniffer_Init(void)
{
  CAN_CaptureBuffer_Init();

  rx_count = 0U;
  error_count = 0U;
  fifo_lost_events = 0U;
  max_fifo_fill = 0U;
  stress_consumed = 0U;
  stress_sequence_errors = 0U;
  stress_expected_counter = 0U;
  capture_cycles = 0U;
  capture_measured_frames = 0U;
  CAN_Sniffer_CycleCounter_Init();

  if ((HAL_FDCAN_ConfigGlobalFilter(
          &hfdcan1,
          FDCAN_ACCEPT_IN_RX_FIFO0,
          FDCAN_ACCEPT_IN_RX_FIFO0,
          FDCAN_FILTER_REMOTE,
          FDCAN_FILTER_REMOTE) != HAL_OK) ||
      (HAL_FDCAN_ConfigTimestampCounter(
          &hfdcan1, FDCAN_TIMESTAMP_PRESC_1) != HAL_OK) ||
      (HAL_FDCAN_EnableTimestampCounter(
          &hfdcan1, FDCAN_TIMESTAMP_INTERNAL) != HAL_OK) ||
      (HAL_FDCAN_Start(&hfdcan1) != HAL_OK))
  {
    Error_Handler();
  }

  Console_Printf("RAM buffer   : %lu frames / %lu bytes / CAN FD records\r\n",
                 (unsigned long)CAN_CAPTURE_CAPACITY,
                 (unsigned long)(CAN_CAPTURE_CAPACITY *
                                 sizeof(CAN_SnifferFrame)));
  Console_Printf("\r\n--- CAN1 SNIFFER ---\r\n");
  Console_Printf("Mode        : BUS MONITORING\r\n");
  Console_Printf("Bitrate     : 500 kbit/s\r\n");
  Console_Printf("STD/EXT/RTR : ACCEPT ALL\r\n");
  Console_Printf("RX FIFO0    : 64 frames\r\n");
  Console_Printf("RX read path: DIRECT MESSAGE RAM -> ZERO-COPY SRAM\r\n");
  Console_Printf("CAN1 TX     : DISABLED\r\n");
  Console_Printf("--------------------\r\n");
}

void CAN_Sniffer_Process(void)
{
  CAN_SnifferFrame discarded_frame;
  CAN_SnifferFrame *frame;
  uint32_t fifo_status;
  uint32_t fill;
  uint32_t frames_before;
  uint32_t cycle_start;
  uint32_t cycle_end;
  bool buffer_full;

  fifo_status = hfdcan1.Instance->RXF0S;
  fill = fifo_status & FDCAN_RXF0S_F0FL;
  if (fill > max_fifo_fill)
  {
    max_fifo_fill = fill;
  }

  if ((fifo_status & FDCAN_RXF0S_RF0L) != 0U)
  {
    fifo_lost_events++;
    hfdcan1.Instance->IR = FDCAN_IR_RF0L;
  }

  if (fill == 0U)
  {
    return;
  }

  frames_before = rx_count;
  cycle_start = DWT->CYCCNT;

  while ((hfdcan1.Instance->RXF0S & FDCAN_RXF0S_F0FL) != 0U)
  {
    frame = CAN_CaptureBuffer_BeginPush();
    buffer_full = (frame == NULL);
    if (buffer_full)
    {
      frame = &discarded_frame;
    }

    if (!CAN_Sniffer_ReadFifo0Direct(frame))
    {
      error_count++;
      break;
    }

    rx_count++;
    if (buffer_full)
    {
      CAN_CaptureBuffer_RecordDrop();
    }
    else
    {
      CAN_CaptureBuffer_CommitPush();
    }
  }

  cycle_end = DWT->CYCCNT;
  if (rx_count != frames_before)
  {
    capture_cycles += (uint32_t)(cycle_end - cycle_start);
    capture_measured_frames += rx_count - frames_before;
  }
}

uint32_t CAN_Sniffer_GetRxCount(void)
{
  return rx_count;
}

uint32_t CAN_Sniffer_GetErrorCount(void)
{
  return error_count;
}

uint32_t CAN_Sniffer_GetBufferedCount(void)
{
  return CAN_CaptureBuffer_GetCount();
}

uint32_t CAN_Sniffer_GetDroppedCount(void)
{
  return CAN_CaptureBuffer_GetDropped();
}

void CAN_Sniffer_DumpBufferedFrames(uint32_t count)
{
  CAN_SnifferFrame frame;

  for (uint32_t n = 0U; n < count; n++)
  {
    if (!CAN_CaptureBuffer_Pop(&frame))
    {
      break;
    }

    Console_Printf(
        "RAM[%lu] ID=%08lX DLC=%u FLAGS=%02X TS=%u "
        "DATA=%02X %02X %02X %02X %02X %02X %02X %02X\r\n",
        (unsigned long)n,
        (unsigned long)frame.id,
        frame.dlc,
        frame.flags,
        frame.timestamp,
        frame.data[0], frame.data[1], frame.data[2], frame.data[3],
        frame.data[4], frame.data[5], frame.data[6], frame.data[7]);
  }
}

uint32_t CAN_Sniffer_GetFifoLostEvents(void)
{
  return fifo_lost_events;
}

uint32_t CAN_Sniffer_GetMaxFifoFill(void)
{
  return max_fifo_fill;
}

void CAN_Sniffer_StressConsume(void)
{
  CAN_SnifferFrame frame;

  while (CAN_CaptureBuffer_Pop(&frame))
  {
    uint32_t counter =
        ((uint32_t)frame.data[0]) |
        ((uint32_t)frame.data[1] << 8) |
        ((uint32_t)frame.data[2] << 16) |
        ((uint32_t)frame.data[3] << 24);

    if (counter != stress_expected_counter)
    {
      stress_sequence_errors++;
      stress_expected_counter = counter;
    }

    stress_expected_counter++;
    stress_consumed++;
  }
}

uint32_t CAN_Sniffer_GetSequenceErrors(void)
{
  return stress_sequence_errors;
}

uint32_t CAN_Sniffer_GetConsumedCount(void)
{
  return stress_consumed;
}

uint64_t CAN_Sniffer_GetCaptureCycles(void)
{
  return capture_cycles;
}

uint32_t CAN_Sniffer_GetMeasuredFrames(void)
{
  return capture_measured_frames;
}
