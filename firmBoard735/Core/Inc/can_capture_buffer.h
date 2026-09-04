#ifndef CAN_CAPTURE_BUFFER_H
#define CAN_CAPTURE_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * BA1-compatible internal capture queue.
 *
 * BA2 uses 2048 x 72-byte CAN FD records = 144 KiB in AXI SRAM.
 * This preserves headroom in its 320 KiB AXI SRAM for the console, FatFs,
 * HyperRAM batches, USB state and the remaining firmware.
 * The power-of-two depth permits a single mask operation per access.
 */
#define CAN_CAPTURE_CAPACITY        2048U
#define CAN_CAPTURE_BUFFER_CAPACITY CAN_CAPTURE_CAPACITY

typedef struct
{
  uint32_t id;
  uint16_t timestamp;
  uint8_t dlc;
  uint8_t flags;
  uint8_t data[64];
} CAN_SnifferFrame;

enum
{
  CAN_FRAME_FLAG_EXTENDED  = (1U << 0),
  CAN_FRAME_FLAG_RTR       = (1U << 1),
  CAN_FRAME_FLAG_FD        = (1U << 2),
  CAN_FRAME_FLAG_BRS       = (1U << 3),
  CAN_FRAME_FLAG_ESI       = (1U << 4),
  CAN_FRAME_FLAG_CHANNEL_1 = (1U << 5)
};

void CAN_CaptureBuffer_Init(void);

/*
 * Zero-copy producer API. Fill the slot returned by BeginPush(), then publish
 * it with CommitPush(). If BeginPush() returns NULL, consume/acknowledge the
 * hardware frame into a scratch record and call RecordDrop().
 */
CAN_SnifferFrame *CAN_CaptureBuffer_BeginPush(void);
void CAN_CaptureBuffer_CommitPush(void);
void CAN_CaptureBuffer_RecordDrop(void);

/*
 * Zero-copy consumer API. Peek() remains valid until Release() advances the
 * queue.
 */
const CAN_SnifferFrame *CAN_CaptureBuffer_Peek(void);
void CAN_CaptureBuffer_Release(void);

/* Compatibility helpers for the existing HyperRAM and diagnostic consumers. */
bool CAN_CaptureBuffer_Push(const CAN_SnifferFrame *frame);
bool CAN_CaptureBuffer_Pop(CAN_SnifferFrame *frame);

void CAN_CaptureBuffer_Clear(void);
uint32_t CAN_CaptureBuffer_GetCount(void);
uint32_t CAN_CaptureBuffer_GetFree(void);
uint32_t CAN_CaptureBuffer_GetDropped(void);
uint32_t CAN_CaptureBuffer_GetDroppedCount(void);
uint32_t CAN_CaptureBuffer_GetAndResetHighWater(void);

#endif
