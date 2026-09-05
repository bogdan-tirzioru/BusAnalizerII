#include "can_sniffer.h"

#include "can_capture_buffer.h"
#include "console.h"
#include "main.h"

#include <stdbool.h>
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

/*
 * Preserve the beginning of an error episode.  The current failures produce
 * fewer than 128 protocol errors during a 100k-frame verification run.  If a
 * noisier run exceeds this depth, new events are counted but the first events
 * are retained because they are the most useful for finding the trigger.
 */
#define CAN_DIAGNOSTIC_EVENT_CAPACITY 128U

#define CAN_DIAGNOSTIC_REASON_BASELINE   (1U << 0)
#define CAN_DIAGNOSTIC_REASON_LEC        (1U << 1)
#define CAN_DIAGNOSTIC_REASON_DLEC       (1U << 2)
#define CAN_DIAGNOSTIC_REASON_LOGGING    (1U << 3)
#define CAN_DIAGNOSTIC_REASON_STATUS     (1U << 4)
#define CAN_DIAGNOSTIC_REASON_INTERRUPT  (1U << 5)

#define CAN_DIAGNOSTIC_PSR_STATUS_MASK \
    (FDCAN_PSR_EP | FDCAN_PSR_EW | FDCAN_PSR_BO | FDCAN_PSR_PXE)

#define CAN_DIAGNOSTIC_IR_MASK \
    (FDCAN_IR_RF0L | FDCAN_IR_RF0F | FDCAN_IR_MRAF | FDCAN_IR_ELO | \
     FDCAN_IR_EP | FDCAN_IR_EW | FDCAN_IR_BO | FDCAN_IR_WDI | \
     FDCAN_IR_PEA | FDCAN_IR_PED | FDCAN_IR_ARA)

#if defined(__GNUC__)
#define CAN_DIAGNOSTIC_STORAGE \
    __attribute__((section(".dtcm_scratch"), aligned(32)))
#else
#define CAN_DIAGNOSTIC_STORAGE
#endif

extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan3;

typedef struct
{
  FDCAN_HandleTypeDef *hfdcan;
  uint8_t frame_channel_flag;
  uint32_t rx_count;
  uint32_t error_count;
  uint32_t fifo_lost_events;
  uint32_t max_fifo_fill;
  uint32_t stress_expected_counter;
  uint8_t stress_have_counter;
  uint8_t diagnostic_initialized;
  uint32_t diagnostic_last_logging;
  uint32_t diagnostic_last_status;
  uint32_t diagnostic_last_ir;
  uint32_t diagnostic_event_count;
  uint32_t diagnostic_dropped_count;
} CAN_SnifferChannel;

typedef struct
{
  uint32_t sequence;
  uint32_t tick_ms;
  uint32_t cycle_counter;
  uint32_t channel_rx_count;
  uint32_t peer_rx_count;
  uint32_t sram_count;
  uint32_t psr;
  uint32_t ecr;
  uint32_t rxf0s;
  uint32_t ir;
  uint32_t reason;
  uint8_t channel;
  uint8_t reserved[3];
} CAN_DiagnosticEvent;

_Static_assert(sizeof(CAN_DiagnosticEvent) == 48U,
               "Unexpected FDCAN diagnostic event size");

static CAN_SnifferChannel channels[CAN_SNIFFER_CHANNEL_COUNT];
static CAN_DiagnosticEvent diagnostic_events[CAN_DIAGNOSTIC_EVENT_CAPACITY]
    CAN_DIAGNOSTIC_STORAGE;
static uint32_t diagnostic_stored_count;
static uint32_t diagnostic_sequence;
static uint8_t next_channel;
static uint32_t stress_consumed;
static uint32_t stress_sequence_errors;
static uint64_t capture_cycles;
static uint32_t capture_measured_frames;

static CAN_SnifferChannel *CAN_Sniffer_GetChannel(uint8_t channel)
{
  if ((channel < CAN_SNIFFER_CHANNEL_1) ||
      (channel > CAN_SNIFFER_CHANNEL_2))
  {
    return NULL;
  }

  return &channels[channel - CAN_SNIFFER_CHANNEL_1];
}

static uint8_t CAN_DlcToLength(uint8_t dlc)
{
  static const uint8_t lengths[16] =
  {
    0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U,
    8U, 12U, 16U, 20U, 24U, 32U, 48U, 64U
  };

  return lengths[dlc & 0x0FU];
}

static void CAN_Sniffer_RecordDiagnosticEvent(
    CAN_SnifferChannel *ctx,
    uint8_t channel,
    uint32_t reason,
    uint32_t psr,
    uint32_t ecr,
    uint32_t rxf0s,
    uint32_t ir)
{
  CAN_DiagnosticEvent *event;
  uint32_t peer_index = (channel == CAN_SNIFFER_CHANNEL_1) ? 1U : 0U;

  ctx->diagnostic_event_count++;
  diagnostic_sequence++;

  if (diagnostic_stored_count >= CAN_DIAGNOSTIC_EVENT_CAPACITY)
  {
    ctx->diagnostic_dropped_count++;
    return;
  }

  event = &diagnostic_events[diagnostic_stored_count++];
  event->sequence = diagnostic_sequence;
  event->tick_ms = HAL_GetTick();
  event->cycle_counter = DWT->CYCCNT;
  event->channel_rx_count = ctx->rx_count;
  event->peer_rx_count = channels[peer_index].rx_count;
  event->sram_count = CAN_CaptureBuffer_GetCount();
  event->psr = psr;
  event->ecr = ecr;
  event->rxf0s = rxf0s;
  event->ir = ir;
  event->reason = reason;
  event->channel = channel;
}

static void CAN_Sniffer_MonitorChannelDiagnostics(
    CAN_SnifferChannel *ctx,
    uint8_t channel)
{
  uint32_t psr = ctx->hfdcan->Instance->PSR;
  uint32_t ecr = ctx->hfdcan->Instance->ECR;
  uint32_t rxf0s = ctx->hfdcan->Instance->RXF0S;
  uint32_t ir = ctx->hfdcan->Instance->IR;
  uint32_t lec = (psr & FDCAN_PSR_LEC) >> FDCAN_PSR_LEC_Pos;
  uint32_t dlec = (psr & FDCAN_PSR_DLEC) >> FDCAN_PSR_DLEC_Pos;
  uint32_t logging = (ecr & FDCAN_ECR_CEL) >> FDCAN_ECR_CEL_Pos;
  uint32_t status = psr & CAN_DIAGNOSTIC_PSR_STATUS_MASK;
  uint32_t diagnostic_ir = ir & CAN_DIAGNOSTIC_IR_MASK;
  uint32_t reason = 0U;

  if (ctx->diagnostic_initialized == 0U)
  {
    reason |= CAN_DIAGNOSTIC_REASON_BASELINE;
    ctx->diagnostic_initialized = 1U;
  }
  else
  {
    /* Reading PSR changes LEC/DLEC to NO_CHANGE. Code NONE is also a normal
     * successful bus event, so retain only real error codes 1 through 6. */
    if ((lec != FDCAN_PROTOCOL_ERROR_NONE) &&
        (lec != FDCAN_PROTOCOL_ERROR_NO_CHANGE))
    {
      reason |= CAN_DIAGNOSTIC_REASON_LEC;
    }
    if ((dlec != FDCAN_PROTOCOL_ERROR_NONE) &&
        (dlec != FDCAN_PROTOCOL_ERROR_NO_CHANGE))
    {
      reason |= CAN_DIAGNOSTIC_REASON_DLEC;
    }
    if (logging != ctx->diagnostic_last_logging)
    {
      reason |= CAN_DIAGNOSTIC_REASON_LOGGING;
    }
    if (status != ctx->diagnostic_last_status)
    {
      reason |= CAN_DIAGNOSTIC_REASON_STATUS;
    }
    if ((diagnostic_ir & ~ctx->diagnostic_last_ir) != 0U)
    {
      reason |= CAN_DIAGNOSTIC_REASON_INTERRUPT;
    }
  }

  ctx->diagnostic_last_logging = logging;
  ctx->diagnostic_last_status = status;
  ctx->diagnostic_last_ir = diagnostic_ir;

  if (reason != 0U)
  {
    CAN_Sniffer_RecordDiagnosticEvent(
        ctx, channel, reason, psr, ecr, rxf0s, ir);
  }
}

static bool CAN_Sniffer_ReadFifo0Direct(
    CAN_SnifferChannel *ctx,
    CAN_SnifferFrame *frame)
{
  uint32_t fifo_status;
  uint32_t get_index;
  volatile const uint32_t *element;
  uint32_t word0;
  uint32_t word1;
  uint8_t length;
  uint8_t stored_length;

  if ((ctx == NULL) || (frame == NULL))
  {
    return false;
  }

  fifo_status = ctx->hfdcan->Instance->RXF0S;
  if ((fifo_status & FDCAN_RXF0S_F0FL) == 0U)
  {
    return false;
  }

  get_index =
      (fifo_status & FDCAN_RXF0S_F0GI) >> FDCAN_RXF0S_F0GI_Pos;

  element = (volatile const uint32_t *)(uintptr_t)
      (ctx->hfdcan->msgRam.RxFIFO0SA +
       (get_index * ctx->hfdcan->Init.RxFifo0ElmtSize * 4U));

  word0 = element[0];
  word1 = element[1];
  frame->flags = ctx->frame_channel_flag;

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

  ctx->hfdcan->Instance->RXF0A = get_index;
  return true;
}

static void CAN_Sniffer_CycleCounter_Init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static bool CAN_Sniffer_StartChannel(CAN_SnifferChannel *ctx)
{
  ctx->hfdcan->Instance->IR = FDCAN_IR_RF0L;

  return
      (HAL_FDCAN_ConfigGlobalFilter(
          ctx->hfdcan,
          FDCAN_ACCEPT_IN_RX_FIFO0,
          FDCAN_ACCEPT_IN_RX_FIFO0,
          FDCAN_FILTER_REMOTE,
          FDCAN_FILTER_REMOTE) == HAL_OK) &&
      (HAL_FDCAN_ConfigTimestampCounter(
          ctx->hfdcan, FDCAN_TIMESTAMP_PRESC_1) == HAL_OK) &&
      (HAL_FDCAN_EnableTimestampCounter(
          ctx->hfdcan, FDCAN_TIMESTAMP_INTERNAL) == HAL_OK) &&
      (HAL_FDCAN_Start(ctx->hfdcan) == HAL_OK);
}

void CAN_Sniffer_Init(void)
{
  CAN_CaptureBuffer_Init();
  (void)memset(channels, 0, sizeof(channels));

  channels[0].hfdcan = &hfdcan1;
  channels[0].frame_channel_flag = 0U;
  channels[1].hfdcan = &hfdcan3;
  channels[1].frame_channel_flag = CAN_FRAME_FLAG_CHANNEL_2;

  next_channel = 0U;
  stress_consumed = 0U;
  stress_sequence_errors = 0U;
  capture_cycles = 0U;
  capture_measured_frames = 0U;
  diagnostic_stored_count = 0U;
  diagnostic_sequence = 0U;
  CAN_Sniffer_CycleCounter_Init();

  Console_Printf("RAM buffer   : %lu frames / %lu bytes / shared CAN FD records\r\n",
                 (unsigned long)CAN_CAPTURE_CAPACITY,
                 (unsigned long)(CAN_CAPTURE_CAPACITY *
                                 sizeof(CAN_SnifferFrame)));
  Console_Printf("\r\n--- DUAL CAN FD SNIFFER ---\r\n");
  Console_Printf("CAN1/CAN2   : FDCAN1 / FDCAN3\r\n");
  Console_Printf("Mode        : BUS MONITORING\r\n");
  Console_Printf("Nominal/data: 1 Mbit/s / 5 Mbit/s\r\n");
  Console_Printf("STD/EXT/RTR : ACCEPT ALL\r\n");
  Console_Printf("RX FIFO0    : 64 x 64-byte elements per channel\r\n");
  Console_Printf("RX read path: DIRECT MESSAGE RAM -> ZERO-COPY SRAM\r\n");
  Console_Printf("CAN TX      : DISABLED ON BOTH CHANNELS\r\n");
  Console_Printf("--------------------\r\n");

  /* Do not let startup diagnostics fill the hardware FIFOs before polling. */
  Console_Flush();

  if (!CAN_Sniffer_StartChannel(&channels[0]) ||
      !CAN_Sniffer_StartChannel(&channels[1]))
  {
    Error_Handler();
  }

  /* Establish a post-start baseline and reset PSR's LEC/DLEC change latches. */
  CAN_Sniffer_MonitorChannelDiagnostics(
      &channels[0], CAN_SNIFFER_CHANNEL_1);
  CAN_Sniffer_MonitorChannelDiagnostics(
      &channels[1], CAN_SNIFFER_CHANNEL_2);
}

static void CAN_Sniffer_ProcessChannel(CAN_SnifferChannel *ctx)
{
  CAN_SnifferFrame discarded_frame;
  CAN_SnifferFrame *frame;
  uint32_t fifo_status;
  uint32_t fill;
  uint32_t frames_before;
  uint32_t cycle_start;
  uint32_t cycle_end;
  uint32_t frames_to_drain;
  bool buffer_full;

  CAN_Sniffer_MonitorChannelDiagnostics(
      ctx,
      (ctx == &channels[0]) ?
          CAN_SNIFFER_CHANNEL_1 : CAN_SNIFFER_CHANNEL_2);

  fifo_status = ctx->hfdcan->Instance->RXF0S;
  fill = fifo_status & FDCAN_RXF0S_F0FL;
  if (fill > ctx->max_fifo_fill)
  {
    ctx->max_fifo_fill = fill;
  }

  if ((fifo_status & FDCAN_RXF0S_RF0L) != 0U)
  {
    ctx->fifo_lost_events++;
    ctx->hfdcan->Instance->IR = FDCAN_IR_RF0L;
  }

  if (fill == 0U)
  {
    return;
  }

  /* Drain only the entry snapshot so a saturated bus cannot starve its peer. */
  frames_to_drain = fill;
  frames_before = ctx->rx_count;
  cycle_start = DWT->CYCCNT;

  while ((frames_to_drain != 0U) &&
         ((ctx->hfdcan->Instance->RXF0S & FDCAN_RXF0S_F0FL) != 0U))
  {
    frame = CAN_CaptureBuffer_BeginPush();
    buffer_full = (frame == NULL);
    if (buffer_full)
    {
      frame = &discarded_frame;
    }

    if (!CAN_Sniffer_ReadFifo0Direct(ctx, frame))
    {
      ctx->error_count++;
      break;
    }

    ctx->rx_count++;
    if (buffer_full)
    {
      CAN_CaptureBuffer_RecordDrop();
    }
    else
    {
      CAN_CaptureBuffer_CommitPush();
    }

    frames_to_drain--;
  }

  cycle_end = DWT->CYCCNT;
  if (ctx->rx_count != frames_before)
  {
    capture_cycles += (uint32_t)(cycle_end - cycle_start);
    capture_measured_frames += ctx->rx_count - frames_before;
  }
}

void CAN_Sniffer_Process(void)
{
  CAN_Sniffer_ProcessChannel(&channels[next_channel]);
  CAN_Sniffer_ProcessChannel(&channels[next_channel ^ 1U]);
  next_channel ^= 1U;
}

uint32_t CAN_Sniffer_GetRxCount(void)
{
  return channels[0].rx_count + channels[1].rx_count;
}

uint32_t CAN_Sniffer_GetErrorCount(void)
{
  return channels[0].error_count + channels[1].error_count;
}

uint32_t CAN_Sniffer_GetChannelRxCount(uint8_t channel)
{
  CAN_SnifferChannel *ctx = CAN_Sniffer_GetChannel(channel);
  return (ctx != NULL) ? ctx->rx_count : 0U;
}

uint32_t CAN_Sniffer_GetChannelErrorCount(uint8_t channel)
{
  CAN_SnifferChannel *ctx = CAN_Sniffer_GetChannel(channel);
  return (ctx != NULL) ? ctx->error_count : 0U;
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
        "RAM[%lu] CAN%u ID=%08lX DLC=%u FLAGS=%02X TS=%u "
        "DATA=%02X %02X %02X %02X %02X %02X %02X %02X\r\n",
        (unsigned long)n,
        ((frame.flags & CAN_FRAME_FLAG_CHANNEL_2) != 0U) ? 2U : 1U,
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
  return channels[0].fifo_lost_events + channels[1].fifo_lost_events;
}

uint32_t CAN_Sniffer_GetMaxFifoFill(void)
{
  return (channels[0].max_fifo_fill >= channels[1].max_fifo_fill) ?
      channels[0].max_fifo_fill : channels[1].max_fifo_fill;
}

uint32_t CAN_Sniffer_GetChannelFifoLostEvents(uint8_t channel)
{
  CAN_SnifferChannel *ctx = CAN_Sniffer_GetChannel(channel);
  return (ctx != NULL) ? ctx->fifo_lost_events : 0U;
}

uint32_t CAN_Sniffer_GetChannelMaxFifoFill(uint8_t channel)
{
  CAN_SnifferChannel *ctx = CAN_Sniffer_GetChannel(channel);
  return (ctx != NULL) ? ctx->max_fifo_fill : 0U;
}

uint32_t CAN_Sniffer_GetChannelDiagnosticEventCount(uint8_t channel)
{
  CAN_SnifferChannel *ctx = CAN_Sniffer_GetChannel(channel);
  return (ctx != NULL) ? ctx->diagnostic_event_count : 0U;
}

uint32_t CAN_Sniffer_GetDiagnosticDroppedCount(void)
{
  return channels[0].diagnostic_dropped_count +
         channels[1].diagnostic_dropped_count;
}

void CAN_Sniffer_DumpDiagnosticEvents(void)
{
  uint32_t detected =
      channels[0].diagnostic_event_count +
      channels[1].diagnostic_event_count;
  uint32_t dropped = CAN_Sniffer_GetDiagnosticDroppedCount();

  /* Capture is frozen before this is called. Empty the DMA queue so the
   * diagnostic dump itself cannot overwrite older verification messages. */
  Console_Flush();
  Console_Printf("\r\n--- FDCAN ERROR EVENT TIMELINE ---\r\n");
  Console_Printf(
      "Events detected/stored/dropped: %lu / %lu / %lu\r\n",
      (unsigned long)detected,
      (unsigned long)diagnostic_stored_count,
      (unsigned long)dropped);
  Console_Printf(
      "Reason bits: BASE=01 LEC=02 DLEC=04 LOG=08 STATUS=10 IR=20\r\n");
  Console_Printf(
      "Note: timeline polling consumes PSR LEC/DLEC; final state may be 7/7\r\n");

  for (uint32_t index = 0U; index < diagnostic_stored_count; index++)
  {
    const CAN_DiagnosticEvent *event = &diagnostic_events[index];
    uint32_t lec =
        (event->psr & FDCAN_PSR_LEC) >> FDCAN_PSR_LEC_Pos;
    uint32_t dlec =
        (event->psr & FDCAN_PSR_DLEC) >> FDCAN_PSR_DLEC_Pos;
    uint32_t rec =
        (event->ecr & FDCAN_ECR_REC) >> FDCAN_ECR_REC_Pos;
    uint32_t tec =
        (event->ecr & FDCAN_ECR_TEC) >> FDCAN_ECR_TEC_Pos;
    uint32_t logging =
        (event->ecr & FDCAN_ECR_CEL) >> FDCAN_ECR_CEL_Pos;
    uint32_t fill =
        (event->rxf0s & FDCAN_RXF0S_F0FL) >> FDCAN_RXF0S_F0FL_Pos;

    Console_Printf(
        "FDEVT #%lu t=%lums cy=%lu CAN%u why=%02lX "
        "rx=%lu peer=%lu sram=%lu LEC=%lu DLEC=%lu "
        "REC=%lu TEC=%lu LOG=%lu fill=%lu "
        "PSR=%08lX ECR=%08lX RXF0S=%08lX IR=%08lX\r\n",
        (unsigned long)event->sequence,
        (unsigned long)event->tick_ms,
        (unsigned long)event->cycle_counter,
        (unsigned int)event->channel,
        (unsigned long)event->reason,
        (unsigned long)event->channel_rx_count,
        (unsigned long)event->peer_rx_count,
        (unsigned long)event->sram_count,
        (unsigned long)lec,
        (unsigned long)dlec,
        (unsigned long)rec,
        (unsigned long)tec,
        (unsigned long)logging,
        (unsigned long)fill,
        (unsigned long)event->psr,
        (unsigned long)event->ecr,
        (unsigned long)event->rxf0s,
        (unsigned long)event->ir);

    if (((index + 1U) & 0x0FU) == 0U)
    {
      Console_Flush();
    }
  }

  Console_Printf("--- END FDCAN ERROR EVENT TIMELINE ---\r\n");
  Console_Flush();
}

void CAN_Sniffer_StressConsume(void)
{
  CAN_SnifferFrame frame;

  while (CAN_CaptureBuffer_Pop(&frame))
  {
    uint8_t channel =
        ((frame.flags & CAN_FRAME_FLAG_CHANNEL_2) != 0U) ?
        CAN_SNIFFER_CHANNEL_2 : CAN_SNIFFER_CHANNEL_1;
    CAN_SnifferChannel *ctx = CAN_Sniffer_GetChannel(channel);
    uint32_t counter =
        ((uint32_t)frame.data[0]) |
        ((uint32_t)frame.data[1] << 8) |
        ((uint32_t)frame.data[2] << 16) |
        ((uint32_t)frame.data[3] << 24);

    if (ctx->stress_have_counter == 0U)
    {
      ctx->stress_expected_counter = counter;
      ctx->stress_have_counter = 1U;
    }

    if (counter != ctx->stress_expected_counter)
    {
      stress_sequence_errors++;
      ctx->stress_expected_counter = counter;
    }

    ctx->stress_expected_counter++;
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
