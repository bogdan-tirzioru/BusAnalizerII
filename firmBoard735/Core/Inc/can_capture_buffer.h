#ifndef CAN_CAPTURE_BUFFER_H
#define CAN_CAPTURE_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 4096 frames x 16 bytes = 65536 bytes = 64 KiB
 */
#define CAN_CAPTURE_BUFFER_CAPACITY 4096U

#define CAN_FRAME_FLAG_EXTENDED  (1U << 0)
#define CAN_FRAME_FLAG_RTR       (1U << 1)
#define CAN_FRAME_FLAG_ESI       (1U << 2)

typedef struct
{
    uint32_t id;

    /*
     * Raw 16-bit FDCAN hardware timestamp.
     * We will extend this later.
     */
    uint16_t timestamp;

    /*
     * Raw CAN DLC: 0..15.
     */
    uint8_t dlc;

    uint8_t flags;

    uint8_t data[8];

} CAN_SnifferFrame;


void CAN_CaptureBuffer_Init(void);

bool CAN_CaptureBuffer_Push(
        const CAN_SnifferFrame *frame);

bool CAN_CaptureBuffer_Pop(
        CAN_SnifferFrame *frame);

uint32_t CAN_CaptureBuffer_GetCount(void);
uint32_t CAN_CaptureBuffer_GetFree(void);
uint32_t CAN_CaptureBuffer_GetDroppedCount(void);

#endif
