#include "baii_protocol.h"
#include "baii_protocol_platform.h"

#include <string.h>

#define BAII_MAGIC_0 'B'
#define BAII_MAGIC_1 'A'
#define BAII_MAGIC_2 'I'
#define BAII_MAGIC_3 'I'

static uint32_t baii_tx_sequence = 0U;

static uint16_t BAII_GetU16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] |
                      ((uint16_t)p[1] << 8U));
}

static uint32_t BAII_GetU32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}

static uint64_t BAII_GetU64(const uint8_t *p)
{
    uint64_t value = 0U;

    for (uint32_t i = 0U; i < 8U; i++)
    {
        value |= ((uint64_t)p[i]) << (8U * i);
    }

    return value;
}

static void BAII_PutU16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void BAII_PutU32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8U) & 0xFFU);
    p[2] = (uint8_t)((value >> 16U) & 0xFFU);
    p[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static void BAII_PutU64(uint8_t *p, uint64_t value)
{
    for (uint32_t i = 0U; i < 8U; i++)
    {
        p[i] = (uint8_t)((value >> (8U * i)) & 0xFFU);
    }
}

static uint32_t BAII_WriteStatusOnly(
        uint16_t command,
        BAII_StatusCode status,
        uint8_t *payload,
        uint32_t capacity)
{
    if (capacity < 4U)
    {
        return 0U;
    }

    BAII_PutU16(&payload[0], command);
    BAII_PutU16(&payload[2], (uint16_t)status);
    return 4U;
}

static uint32_t BAII_WriteCanConfig(
        uint16_t command,
        BAII_StatusCode status,
        const BAII_CanConfig *config,
        uint8_t *payload,
        uint32_t capacity)
{
    if ((config == NULL) || (capacity < 32U))
    {
        return 0U;
    }

    BAII_PutU16(&payload[0], command);
    BAII_PutU16(&payload[2], (uint16_t)status);

    payload[4] = config->channel;
    payload[5] = config->mode;
    payload[6] = config->frame_format;
    payload[7] = 0U;

    BAII_PutU32(&payload[8],  config->fdcan_clock_hz);
    BAII_PutU32(&payload[12], config->nominal_bitrate);
    BAII_PutU32(&payload[16], config->data_bitrate);
    BAII_PutU16(&payload[20], config->nominal_sample_point_permille);
    BAII_PutU16(&payload[22], config->data_sample_point_permille);
    BAII_PutU16(&payload[24], config->nominal_prescaler);
    BAII_PutU16(&payload[26], config->nominal_time_seg1);
    BAII_PutU16(&payload[28], config->nominal_time_seg2);
    BAII_PutU16(&payload[30], config->nominal_sjw);

    return 32U;
}

static uint32_t BAII_DispatchCommand(
        const uint8_t *request_payload,
        uint32_t request_payload_length,
        uint8_t *response_payload,
        uint32_t response_capacity)
{
    uint16_t command;
    BAII_StatusCode status;

    if ((request_payload == NULL) ||
        (request_payload_length < BAII_PROTOCOL_COMMAND_HEADER_SIZE))
    {
        return 0U;
    }

    command = BAII_GetU16(&request_payload[0]);

    switch (command)
    {
        case BAII_CMD_GET_INFO:
        {
            BAII_DeviceInfo info;

            if (request_payload_length != 4U)
            {
                return BAII_WriteStatusOnly(
                        command,
                        BAII_STATUS_BAD_LENGTH,
                        response_payload,
                        response_capacity);
            }

            memset(&info, 0, sizeof(info));
            status = BAII_Platform_GetInfo(&info);

            if (status != BAII_STATUS_OK)
            {
                return BAII_WriteStatusOnly(
                        command,
                        status,
                        response_payload,
                        response_capacity);
            }

            if (response_capacity < 28U)
            {
                return 0U;
            }

            BAII_PutU16(&response_payload[0], command);
            BAII_PutU16(&response_payload[2], (uint16_t)status);
            response_payload[4] = info.firmware_major;
            response_payload[5] = info.firmware_minor;
            BAII_PutU16(&response_payload[6], info.firmware_patch);
            BAII_PutU32(&response_payload[8], info.capabilities);
            BAII_PutU32(&response_payload[12], info.fdcan_clock_hz);
            BAII_PutU32(&response_payload[16], info.hyperram_size_bytes);
            BAII_PutU32(&response_payload[20], info.device_id);
            response_payload[24] = info.can_channel_count;
            response_payload[25] = info.rtc_valid;
            response_payload[26] = 0U;
            response_payload[27] = 0U;
            return 28U;
        }

        case BAII_CMD_GET_STATUS:
        {
            BAII_DeviceStatus device_status;

            if (request_payload_length != 4U)
            {
                return BAII_WriteStatusOnly(
                        command,
                        BAII_STATUS_BAD_LENGTH,
                        response_payload,
                        response_capacity);
            }

            memset(&device_status, 0, sizeof(device_status));
            status = BAII_Platform_GetStatus(&device_status);

            if (status != BAII_STATUS_OK)
            {
                return BAII_WriteStatusOnly(
                        command,
                        status,
                        response_payload,
                        response_capacity);
            }

            if (response_capacity < 40U)
            {
                return 0U;
            }

            BAII_PutU16(&response_payload[0], command);
            BAII_PutU16(&response_payload[2], (uint16_t)status);
            BAII_PutU32(&response_payload[4],  device_status.uptime_ms);
            BAII_PutU32(&response_payload[8],  device_status.can_rx_frames);
            BAII_PutU32(&response_payload[12], device_status.sram_buffered_frames);
            BAII_PutU32(&response_payload[16], device_status.sram_dropped_frames);
            BAII_PutU32(&response_payload[20], device_status.fdcan_fifo_lost_events);
            BAII_PutU32(&response_payload[24], device_status.hyperram_stored_frames);
            BAII_PutU32(&response_payload[28], device_status.hyperram_write_errors);
            BAII_PutU32(&response_payload[32], device_status.hyperram_lost_frames);
            BAII_PutU32(&response_payload[36], device_status.hyperram_wrap_count);
            return 40U;
        }

        case BAII_CMD_GET_RTC_TIME:
        {
            uint64_t unix_time_us = 0U;

            if (request_payload_length != 4U)
            {
                return BAII_WriteStatusOnly(
                        command,
                        BAII_STATUS_BAD_LENGTH,
                        response_payload,
                        response_capacity);
            }

            status = BAII_Platform_GetRtcTime(&unix_time_us);

            if (status != BAII_STATUS_OK)
            {
                return BAII_WriteStatusOnly(
                        command,
                        status,
                        response_payload,
                        response_capacity);
            }

            if (response_capacity < 12U)
            {
                return 0U;
            }

            BAII_PutU16(&response_payload[0], command);
            BAII_PutU16(&response_payload[2], (uint16_t)status);
            BAII_PutU64(&response_payload[4], unix_time_us);
            return 12U;
        }

        case BAII_CMD_SET_RTC_TIME:
        {
            uint64_t requested_time_us;
            uint64_t applied_time_us = 0U;

            if (request_payload_length != 12U)
            {
                return BAII_WriteStatusOnly(
                        command,
                        BAII_STATUS_BAD_LENGTH,
                        response_payload,
                        response_capacity);
            }

            requested_time_us = BAII_GetU64(&request_payload[4]);
            status = BAII_Platform_SetRtcTime(requested_time_us);

            if (status == BAII_STATUS_OK)
            {
                status = BAII_Platform_GetRtcTime(&applied_time_us);
            }

            if (status != BAII_STATUS_OK)
            {
                return BAII_WriteStatusOnly(
                        command,
                        status,
                        response_payload,
                        response_capacity);
            }

            if (response_capacity < 12U)
            {
                return 0U;
            }

            BAII_PutU16(&response_payload[0], command);
            BAII_PutU16(&response_payload[2], (uint16_t)status);
            BAII_PutU64(&response_payload[4], applied_time_us);
            return 12U;
        }

        case BAII_CMD_GET_CAN_CONFIG:
        {
            BAII_CanConfig config;
            uint8_t channel;

            if (request_payload_length != 8U)
            {
                return BAII_WriteStatusOnly(
                        command,
                        BAII_STATUS_BAD_LENGTH,
                        response_payload,
                        response_capacity);
            }

            channel = request_payload[4];
            memset(&config, 0, sizeof(config));
            status = BAII_Platform_GetCanConfig(channel, &config);

            if (status != BAII_STATUS_OK)
            {
                return BAII_WriteStatusOnly(
                        command,
                        status,
                        response_payload,
                        response_capacity);
            }

            return BAII_WriteCanConfig(
                    command,
                    status,
                    &config,
                    response_payload,
                    response_capacity);
        }

        case BAII_CMD_SET_CAN_CONFIG:
        {
            BAII_CanConfig requested;
            BAII_CanConfig applied;

            if (request_payload_length != 20U)
            {
                return BAII_WriteStatusOnly(
                        command,
                        BAII_STATUS_BAD_LENGTH,
                        response_payload,
                        response_capacity);
            }

            memset(&requested, 0, sizeof(requested));
            memset(&applied, 0, sizeof(applied));

            requested.channel = request_payload[4];
            requested.mode = request_payload[5];
            requested.frame_format = request_payload[6];
            requested.nominal_bitrate = BAII_GetU32(&request_payload[8]);
            requested.data_bitrate = BAII_GetU32(&request_payload[12]);
            requested.nominal_sample_point_permille =
                    BAII_GetU16(&request_payload[16]);
            requested.data_sample_point_permille =
                    BAII_GetU16(&request_payload[18]);

            status = BAII_Platform_SetCanConfig(&requested, &applied);

            if (status != BAII_STATUS_OK)
            {
                return BAII_WriteStatusOnly(
                        command,
                        status,
                        response_payload,
                        response_capacity);
            }

            return BAII_WriteCanConfig(
                    command,
                    status,
                    &applied,
                    response_payload,
                    response_capacity);
        }

        case BAII_CMD_CAPTURE_START:
        case BAII_CMD_CAPTURE_STOP:
        case BAII_CMD_CAPTURE_CLEAR:
        case BAII_CMD_GET_CAPTURE_STATUS:
            return BAII_WriteStatusOnly(
                    command,
                    BAII_STATUS_NOT_SUPPORTED,
                    response_payload,
                    response_capacity);

        default:
            return BAII_WriteStatusOnly(
                    command,
                    BAII_STATUS_UNKNOWN_COMMAND,
                    response_payload,
                    response_capacity);
    }
}

void BAII_Protocol_Init(void)
{
    baii_tx_sequence = 0U;
}

BAII_ProtocolResult BAII_Protocol_DecodeHeader(
        const uint8_t *message,
        uint32_t message_length,
        BAII_MessageHeader *header)
{
    if ((message == NULL) || (header == NULL))
    {
        return BAII_PROTOCOL_BAD_ARGUMENT;
    }

    if (message_length < BAII_PROTOCOL_HEADER_SIZE)
    {
        return BAII_PROTOCOL_BAD_LENGTH;
    }

    if ((message[0] != (uint8_t)BAII_MAGIC_0) ||
        (message[1] != (uint8_t)BAII_MAGIC_1) ||
        (message[2] != (uint8_t)BAII_MAGIC_2) ||
        (message[3] != (uint8_t)BAII_MAGIC_3))
    {
        return BAII_PROTOCOL_BAD_MAGIC;
    }

    header->version_major = message[4];
    header->version_minor = message[5];
    header->message_type = message[6];
    header->flags = message[7];
    header->transaction_id = BAII_GetU32(&message[8]);
    header->sequence = BAII_GetU32(&message[12]);
    header->payload_length = BAII_GetU32(&message[16]);

    if ((header->version_major != BAII_PROTOCOL_VERSION_MAJOR) ||
        (header->version_minor != BAII_PROTOCOL_VERSION_MINOR))
    {
        return BAII_PROTOCOL_UNSUPPORTED_VERSION;
    }

    if (header->payload_length !=
        (message_length - BAII_PROTOCOL_HEADER_SIZE))
    {
        return BAII_PROTOCOL_BAD_LENGTH;
    }

    return BAII_PROTOCOL_OK;
}

uint32_t BAII_Protocol_BuildMessage(
        uint8_t message_type,
        uint8_t flags,
        uint32_t transaction_id,
        uint32_t sequence,
        const uint8_t *payload,
        uint32_t payload_length,
        uint8_t *output,
        uint32_t output_capacity)
{
    uint32_t total_length = BAII_PROTOCOL_HEADER_SIZE + payload_length;

    if ((output == NULL) ||
        ((payload_length > 0U) && (payload == NULL)) ||
        (output_capacity < total_length))
    {
        return 0U;
    }

    output[0] = (uint8_t)BAII_MAGIC_0;
    output[1] = (uint8_t)BAII_MAGIC_1;
    output[2] = (uint8_t)BAII_MAGIC_2;
    output[3] = (uint8_t)BAII_MAGIC_3;
    output[4] = BAII_PROTOCOL_VERSION_MAJOR;
    output[5] = BAII_PROTOCOL_VERSION_MINOR;
    output[6] = message_type;
    output[7] = flags;
    BAII_PutU32(&output[8], transaction_id);
    BAII_PutU32(&output[12], sequence);
    BAII_PutU32(&output[16], payload_length);

    if (payload_length > 0U)
    {
        memcpy(&output[BAII_PROTOCOL_HEADER_SIZE], payload, payload_length);
    }

    return total_length;
}

BAII_ProtocolResult BAII_Protocol_HandleMessage(
        const uint8_t *request,
        uint32_t request_length,
        uint8_t *response,
        uint32_t response_capacity,
        uint32_t *response_length)
{
    BAII_MessageHeader header;
    BAII_ProtocolResult result;
    uint8_t response_payload[BAII_PROTOCOL_MAX_CONTROL_PAYLOAD];
    uint32_t payload_length;
    uint32_t built_length;

    if ((request == NULL) ||
        (response == NULL) ||
        (response_length == NULL))
    {
        return BAII_PROTOCOL_BAD_ARGUMENT;
    }

    *response_length = 0U;

    result = BAII_Protocol_DecodeHeader(request, request_length, &header);
    if (result != BAII_PROTOCOL_OK)
    {
        return result;
    }

    if (header.message_type != BAII_MSG_COMMAND)
    {
        return BAII_PROTOCOL_BAD_MESSAGE_TYPE;
    }

    if ((header.payload_length < BAII_PROTOCOL_COMMAND_HEADER_SIZE) ||
        (header.payload_length > BAII_PROTOCOL_MAX_CONTROL_PAYLOAD))
    {
        return BAII_PROTOCOL_BAD_LENGTH;
    }

    payload_length = BAII_DispatchCommand(
            &request[BAII_PROTOCOL_HEADER_SIZE],
            header.payload_length,
            response_payload,
            sizeof(response_payload));

    if (payload_length == 0U)
    {
        return BAII_PROTOCOL_OUTPUT_TOO_SMALL;
    }

    built_length = BAII_Protocol_BuildMessage(
            BAII_MSG_RESPONSE,
            BAII_MSG_FLAG_NONE,
            header.transaction_id,
            baii_tx_sequence++,
            response_payload,
            payload_length,
            response,
            response_capacity);

    if (built_length == 0U)
    {
        return BAII_PROTOCOL_OUTPUT_TOO_SMALL;
    }

    *response_length = built_length;
    return BAII_PROTOCOL_OK;
}

uint32_t BAII_Protocol_EncodeCanRecord(
        const BAII_CanRecord *record,
        uint8_t *output,
        uint32_t output_capacity)
{
    uint32_t record_length;

    if ((record == NULL) || (output == NULL))
    {
        return 0U;
    }

    if ((record->channel != BAII_CAN_CHANNEL_1) &&
        (record->channel != BAII_CAN_CHANNEL_2))
    {
        return 0U;
    }

    if ((record->dlc > 15U) ||
        (record->data_length > BAII_PROTOCOL_CAN_DATA_MAX) ||
        (record->can_id > 0x1FFFFFFFUL))
    {
        return 0U;
    }

    if (((record->flags & BAII_CAN_FLAG_EXT) == 0U) &&
        (record->can_id > 0x7FFU))
    {
        return 0U;
    }

    record_length = BAII_PROTOCOL_CAN_RECORD_BASE + record->data_length;

    if (output_capacity < record_length)
    {
        return 0U;
    }

    BAII_PutU64(&output[0], record->timestamp_us);
    BAII_PutU32(&output[8], record->can_id);
    BAII_PutU16(&output[12], record->flags);
    output[14] = record->channel;
    output[15] = record->dlc;
    output[16] = record->data_length;
    output[17] = 0U;

    if (record->data_length > 0U)
    {
        memcpy(&output[18], record->data, record->data_length);
    }

    return record_length;
}
