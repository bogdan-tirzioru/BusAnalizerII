#include "baii_protocol_selftest.h"

#include "baii_protocol.h"
#include "can_sniffer.h"
#include "hyperram_capture.h"
#include "main.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BAII_SELFTEST_REQUEST_SIZE          64U
#define BAII_SELFTEST_RESPONSE_SIZE         192U
#define BAII_SELFTEST_CAN_WINDOW_MS         3000U
#define BAII_SELFTEST_CAN_SWITCH_DELAY_MS   8000U

#define BAII_SELFTEST_FDCAN_CLOCK_HZ        96000000UL
#define BAII_SELFTEST_HYPERRAM_BYTES        (8UL * 1024UL * 1024UL)
#define BAII_SELFTEST_REQUIRED_CAPABILITIES \
    (BAII_CAP_RTC | BAII_CAP_CAN_CONFIG | \
     BAII_CAP_CAPTURE_STATUS | BAII_CAP_HYPERRAM)

/* 2026-08-16 05:30:00 UTC. Whole-second value is intentional. */
#define BAII_SELFTEST_RTC_VALUE_US          1786858200000000ULL


typedef struct
{
    uint8_t bytes[BAII_SELFTEST_RESPONSE_SIZE];
    BAII_MessageHeader header;
    const uint8_t *payload;
    uint32_t length;
} BAII_SelfTestResponse;


static uint16_t BAII_SelfTest_GetU16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] |
                      ((uint16_t)p[1] << 8U));
}


static uint32_t BAII_SelfTest_GetU32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}


static uint64_t BAII_SelfTest_GetU64(const uint8_t *p)
{
    uint64_t value = 0U;

    for (uint32_t i = 0U; i < 8U; i++)
    {
        value |= ((uint64_t)p[i]) << (8U * i);
    }

    return value;
}


static void BAII_SelfTest_PutU16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8U) & 0xFFU);
}


static void BAII_SelfTest_PutU32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8U) & 0xFFU);
    p[2] = (uint8_t)((value >> 16U) & 0xFFU);
    p[3] = (uint8_t)((value >> 24U) & 0xFFU);
}


static void BAII_SelfTest_PutU64(uint8_t *p, uint64_t value)
{
    for (uint32_t i = 0U; i < 8U; i++)
    {
        p[i] = (uint8_t)((value >> (8U * i)) & 0xFFU);
    }
}


static uint8_t BAII_SelfTest_Exchange(
        uint32_t transaction_id,
        const uint8_t *command_payload,
        uint32_t command_payload_length,
        BAII_SelfTestResponse *response)
{
    uint8_t request[BAII_SELFTEST_REQUEST_SIZE];
    uint32_t request_length;
    uint32_t response_length = 0U;
    BAII_ProtocolResult result;

    if ((command_payload == NULL) ||
        (response == NULL) ||
        (command_payload_length >
            (BAII_SELFTEST_REQUEST_SIZE - BAII_PROTOCOL_HEADER_SIZE)))
    {
        return 0U;
    }

    memset(request, 0, sizeof(request));
    memset(response, 0, sizeof(*response));

    request_length = BAII_Protocol_BuildMessage(
            BAII_MSG_COMMAND,
            BAII_MSG_FLAG_NONE,
            transaction_id,
            0U,
            command_payload,
            command_payload_length,
            request,
            sizeof(request));

    if (request_length == 0U)
    {
        return 0U;
    }

    result = BAII_Protocol_HandleMessage(
            request,
            request_length,
            response->bytes,
            sizeof(response->bytes),
            &response_length);

    if (result != BAII_PROTOCOL_OK)
    {
        return 0U;
    }

    result = BAII_Protocol_DecodeHeader(
            response->bytes,
            response_length,
            &response->header);

    if (result != BAII_PROTOCOL_OK)
    {
        return 0U;
    }

    if ((response->header.message_type != BAII_MSG_RESPONSE) ||
        (response->header.transaction_id != transaction_id) ||
        (response->header.payload_length < 4U))
    {
        return 0U;
    }

    response->payload = &response->bytes[BAII_PROTOCOL_HEADER_SIZE];
    response->length = response_length;
    return 1U;
}


static uint8_t BAII_SelfTest_ResponseIsOk(
        const BAII_SelfTestResponse *response,
        uint16_t expected_command,
        uint32_t expected_payload_length)
{
    if ((response == NULL) || (response->payload == NULL))
    {
        return 0U;
    }

    if ((response->header.payload_length != expected_payload_length) ||
        (BAII_SelfTest_GetU16(&response->payload[0]) != expected_command) ||
        (BAII_SelfTest_GetU16(&response->payload[2]) != BAII_STATUS_OK))
    {
        return 0U;
    }

    return 1U;
}


static uint8_t BAII_SelfTest_GetRtc(
        uint32_t transaction_id,
        uint64_t *unix_time_us)
{
    uint8_t payload[4] = {0};
    BAII_SelfTestResponse response;

    if (unix_time_us == NULL)
    {
        return 0U;
    }

    BAII_SelfTest_PutU16(&payload[0], BAII_CMD_GET_RTC_TIME);

    if (BAII_SelfTest_Exchange(
            transaction_id,
            payload,
            sizeof(payload),
            &response) == 0U)
    {
        return 0U;
    }

    if (BAII_SelfTest_ResponseIsOk(
            &response,
            BAII_CMD_GET_RTC_TIME,
            12U) == 0U)
    {
        return 0U;
    }

    *unix_time_us = BAII_SelfTest_GetU64(&response.payload[4]);
    return 1U;
}


static uint8_t BAII_SelfTest_SetRtc(
        uint32_t transaction_id,
        uint64_t requested_time_us,
        uint64_t *applied_time_us)
{
    uint8_t payload[12] = {0};
    BAII_SelfTestResponse response;

    if (applied_time_us == NULL)
    {
        return 0U;
    }

    BAII_SelfTest_PutU16(&payload[0], BAII_CMD_SET_RTC_TIME);
    BAII_SelfTest_PutU64(&payload[4], requested_time_us);

    if (BAII_SelfTest_Exchange(
            transaction_id,
            payload,
            sizeof(payload),
            &response) == 0U)
    {
        return 0U;
    }

    if (BAII_SelfTest_ResponseIsOk(
            &response,
            BAII_CMD_SET_RTC_TIME,
            12U) == 0U)
    {
        return 0U;
    }

    *applied_time_us = BAII_SelfTest_GetU64(&response.payload[4]);
    return 1U;
}


static void BAII_SelfTest_DecodeCanConfig(
        const uint8_t *payload,
        BAII_CanConfig *config)
{
    memset(config, 0, sizeof(*config));

    config->channel = payload[4];
    config->mode = payload[5];
    config->frame_format = payload[6];
    config->fdcan_clock_hz = BAII_SelfTest_GetU32(&payload[8]);
    config->nominal_bitrate = BAII_SelfTest_GetU32(&payload[12]);
    config->data_bitrate = BAII_SelfTest_GetU32(&payload[16]);
    config->nominal_sample_point_permille =
        BAII_SelfTest_GetU16(&payload[20]);
    config->data_sample_point_permille =
        BAII_SelfTest_GetU16(&payload[22]);
    config->nominal_prescaler = BAII_SelfTest_GetU16(&payload[24]);
    config->nominal_time_seg1 = BAII_SelfTest_GetU16(&payload[26]);
    config->nominal_time_seg2 = BAII_SelfTest_GetU16(&payload[28]);
    config->nominal_sjw = BAII_SelfTest_GetU16(&payload[30]);
}


static uint8_t BAII_SelfTest_SetCan(
        uint32_t transaction_id,
        uint32_t bitrate,
        BAII_CanConfig *applied)
{
    uint8_t payload[20] = {0};
    BAII_SelfTestResponse response;

    if (applied == NULL)
    {
        return 0U;
    }

    BAII_SelfTest_PutU16(&payload[0], BAII_CMD_SET_CAN_CONFIG);
    payload[4] = BAII_CAN_CHANNEL_1;
    payload[5] = BAII_CAN_MODE_LISTEN_ONLY;
    payload[6] = BAII_CAN_FORMAT_CLASSIC;
    BAII_SelfTest_PutU32(&payload[8], bitrate);
    BAII_SelfTest_PutU32(&payload[12], 0U);
    BAII_SelfTest_PutU16(&payload[16], 875U);
    BAII_SelfTest_PutU16(&payload[18], 0U);

    if (BAII_SelfTest_Exchange(
            transaction_id,
            payload,
            sizeof(payload),
            &response) == 0U)
    {
        return 0U;
    }

    if (BAII_SelfTest_ResponseIsOk(
            &response,
            BAII_CMD_SET_CAN_CONFIG,
            32U) == 0U)
    {
        return 0U;
    }

    BAII_SelfTest_DecodeCanConfig(response.payload, applied);
    return 1U;
}


static uint8_t BAII_SelfTest_GetCan(
        uint32_t transaction_id,
        BAII_CanConfig *config)
{
    uint8_t payload[8] = {0};
    BAII_SelfTestResponse response;

    if (config == NULL)
    {
        return 0U;
    }

    BAII_SelfTest_PutU16(&payload[0], BAII_CMD_GET_CAN_CONFIG);
    payload[4] = BAII_CAN_CHANNEL_1;

    if (BAII_SelfTest_Exchange(
            transaction_id,
            payload,
            sizeof(payload),
            &response) == 0U)
    {
        return 0U;
    }

    if (BAII_SelfTest_ResponseIsOk(
            &response,
            BAII_CMD_GET_CAN_CONFIG,
            32U) == 0U)
    {
        return 0U;
    }

    BAII_SelfTest_DecodeCanConfig(response.payload, config);
    return 1U;
}


static uint8_t BAII_SelfTest_CheckCanTiming(
        const BAII_CanConfig *config,
        uint32_t expected_bitrate,
        uint16_t expected_prescaler)
{
    if (config == NULL)
    {
        return 0U;
    }

    return ((config->channel == BAII_CAN_CHANNEL_1) &&
            (config->mode == BAII_CAN_MODE_LISTEN_ONLY) &&
            (config->frame_format == BAII_CAN_FORMAT_CLASSIC) &&
            (config->fdcan_clock_hz == BAII_SELFTEST_FDCAN_CLOCK_HZ) &&
            (config->nominal_bitrate == expected_bitrate) &&
            (config->data_bitrate == 0U) &&
            (config->nominal_sample_point_permille == 875U) &&
            (config->nominal_prescaler == expected_prescaler) &&
            (config->nominal_time_seg1 == 13U) &&
            (config->nominal_time_seg2 == 2U) &&
            (config->nominal_sjw == 2U)) ? 1U : 0U;
}


static void BAII_SelfTest_PrintCanConfig(const BAII_CanConfig *config)
{
    BAII_SelfTest_Printf("Channel       : %u\r\n", (unsigned int)config->channel);
    BAII_SelfTest_Printf("Mode          : %s\r\n",
           (config->mode == BAII_CAN_MODE_LISTEN_ONLY) ?
           "LISTEN_ONLY" : "NORMAL");
    BAII_SelfTest_Printf("Format        : %s\r\n",
           (config->frame_format == BAII_CAN_FORMAT_CLASSIC) ?
           "CLASSIC" : "OTHER");
    BAII_SelfTest_Printf("FDCAN clock   : %lu Hz\r\n",
           (unsigned long)config->fdcan_clock_hz);
    BAII_SelfTest_Printf("Bitrate       : %lu bit/s\r\n",
           (unsigned long)config->nominal_bitrate);
    BAII_SelfTest_Printf("Sample point  : %u.%u %%\r\n",
           (unsigned int)(config->nominal_sample_point_permille / 10U),
           (unsigned int)(config->nominal_sample_point_permille % 10U));
    BAII_SelfTest_Printf("Prescaler     : %u\r\n",
           (unsigned int)config->nominal_prescaler);
    BAII_SelfTest_Printf("TSEG1         : %u\r\n",
           (unsigned int)config->nominal_time_seg1);
    BAII_SelfTest_Printf("TSEG2         : %u\r\n",
           (unsigned int)config->nominal_time_seg2);
    BAII_SelfTest_Printf("SJW           : %u\r\n",
           (unsigned int)config->nominal_sjw);
}


static uint8_t BAII_SelfTest_MeasureCanTraffic(
        uint32_t window_ms,
        uint32_t *frames_received,
        uint32_t *fifo_lost_delta,
        uint32_t *read_error_delta)
{
    uint32_t rx_before;
    uint32_t fifo_before;
    uint32_t errors_before;
    uint32_t start;

    if ((frames_received == NULL) ||
        (fifo_lost_delta == NULL) ||
        (read_error_delta == NULL))
    {
        return 0U;
    }

    rx_before = CAN_Sniffer_GetRxCount();
    fifo_before = CAN_Sniffer_GetFifoLostEvents();
    errors_before = CAN_Sniffer_GetErrorCount();
    start = HAL_GetTick();

    while ((HAL_GetTick() - start) < window_ms)
    {
        CAN_Sniffer_Process();
        HyperRAM_Capture_Process();
    }

    *frames_received = CAN_Sniffer_GetRxCount() - rx_before;
    *fifo_lost_delta = CAN_Sniffer_GetFifoLostEvents() - fifo_before;
    *read_error_delta = CAN_Sniffer_GetErrorCount() - errors_before;

    return ((*frames_received > 0U) &&
            (*fifo_lost_delta == 0U) &&
            (*read_error_delta == 0U)) ? 1U : 0U;
}


static void BAII_SelfTest_PumpCan(uint32_t duration_ms)
{
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < duration_ms)
    {
        CAN_Sniffer_Process();
        HyperRAM_Capture_Process();
    }
}


void BAII_Protocol_SelfTest(void)
{
    uint8_t test1_pass = 0U;
    uint8_t test2_pass = 0U;
    uint8_t test3_pass = 0U;
    uint8_t test4_pass = 0U;
    uint8_t test5_pass = 0U;
    uint8_t test6_pass = 0U;
    uint32_t transaction_id = 1U;

    BAII_SelfTest_Printf("\r\n");
    BAII_SelfTest_Printf("========================================\r\n");
    BAII_SelfTest_Printf(" BAII BINARY PROTOCOL v0.1 SELF TEST\r\n");
    BAII_SelfTest_Printf("========================================\r\n");
    BAII_SelfTest_Printf("NOTE: Test 3 sets RTC to 2026-08-16 05:30:00 UTC.\r\n");
    BAII_SelfTest_Printf("NOTE: For test 5, external CAN generator must be 250 kbit/s.\r\n");
    BAII_SelfTest_Printf("NOTE: Test 6 gives 8 seconds to switch generator to 500 kbit/s.\r\n\r\n");

    BAII_Protocol_Init();

    /* ------------------------------------------------------------ */
    /* Test 1: GET_INFO                                             */
    /* ------------------------------------------------------------ */
    {
        uint8_t payload[4] = {0};
        BAII_SelfTestResponse response;

        BAII_SelfTest_Printf("[1] GET_INFO\r\n");
        BAII_SelfTest_PutU16(&payload[0], BAII_CMD_GET_INFO);

        if ((BAII_SelfTest_Exchange(
                transaction_id++,
                payload,
                sizeof(payload),
                &response) != 0U) &&
            (BAII_SelfTest_ResponseIsOk(
                &response,
                BAII_CMD_GET_INFO,
                28U) != 0U))
        {
            uint8_t fw_major = response.payload[4];
            uint8_t fw_minor = response.payload[5];
            uint16_t fw_patch = BAII_SelfTest_GetU16(&response.payload[6]);
            uint32_t capabilities = BAII_SelfTest_GetU32(&response.payload[8]);
            uint32_t fdcan_clock = BAII_SelfTest_GetU32(&response.payload[12]);
            uint32_t hyperram_bytes = BAII_SelfTest_GetU32(&response.payload[16]);
            uint32_t device_id = BAII_SelfTest_GetU32(&response.payload[20]);
            uint8_t can_channels = response.payload[24];
            uint8_t rtc_valid = response.payload[25];

            BAII_SelfTest_Printf("Protocol       : %u.%u\r\n",
                   (unsigned int)response.header.version_major,
                   (unsigned int)response.header.version_minor);
            BAII_SelfTest_Printf("Firmware       : %u.%u.%u\r\n",
                   (unsigned int)fw_major,
                   (unsigned int)fw_minor,
                   (unsigned int)fw_patch);
            BAII_SelfTest_Printf("Capabilities   : 0x%08lX\r\n",
                   (unsigned long)capabilities);
            BAII_SelfTest_Printf("FDCAN clock    : %lu Hz\r\n",
                   (unsigned long)fdcan_clock);
            BAII_SelfTest_Printf("HyperRAM       : %lu bytes\r\n",
                   (unsigned long)hyperram_bytes);
            BAII_SelfTest_Printf("Device ID      : 0x%03lX\r\n",
                   (unsigned long)device_id);
            BAII_SelfTest_Printf("CAN channels   : %u\r\n",
                   (unsigned int)can_channels);
            BAII_SelfTest_Printf("RTC valid      : %s\r\n",
                   (rtc_valid != 0U) ? "YES" : "NO");

            test1_pass =
                ((fdcan_clock == BAII_SELFTEST_FDCAN_CLOCK_HZ) &&
                 (hyperram_bytes == BAII_SELFTEST_HYPERRAM_BYTES) &&
                 ((capabilities & BAII_SELFTEST_REQUIRED_CAPABILITIES) ==
                    BAII_SELFTEST_REQUIRED_CAPABILITIES) &&
                 (can_channels == 2U) &&
                 (rtc_valid != 0U)) ? 1U : 0U;
        }

        BAII_SelfTest_Printf("RESULT         : %s\r\n\r\n",
               (test1_pass != 0U) ? "PASS" : "FAIL");
    }

    /* ------------------------------------------------------------ */
    /* Test 2: GET_RTC_TIME twice, approximately one second apart.  */
    /* ------------------------------------------------------------ */
    {
        uint64_t rtc1 = 0U;
        uint64_t rtc2 = 0U;
        uint64_t delta = 0U;

        BAII_SelfTest_Printf("[2] GET_RTC_TIME\r\n");

        if (BAII_SelfTest_GetRtc(transaction_id++, &rtc1) != 0U)
        {
            HAL_Delay(1000U);

            if (BAII_SelfTest_GetRtc(transaction_id++, &rtc2) != 0U)
            {
                if (rtc2 >= rtc1)
                {
                    delta = rtc2 - rtc1;
                }

                test2_pass =
                    ((delta >= 900000ULL) &&
                     (delta <= 1100000ULL)) ? 1U : 0U;
            }
        }

        BAII_SelfTest_Printf("Time #1        : %llu us\r\n",
               (unsigned long long)rtc1);
        BAII_SelfTest_Printf("Time #2        : %llu us\r\n",
               (unsigned long long)rtc2);
        BAII_SelfTest_Printf("Delta          : %llu us\r\n",
               (unsigned long long)delta);
        BAII_SelfTest_Printf("RESULT         : %s\r\n\r\n",
               (test2_pass != 0U) ? "PASS" : "FAIL");
    }

    /* ------------------------------------------------------------ */
    /* Test 3: SET_RTC_TIME and read it back through the protocol.  */
    /* ------------------------------------------------------------ */
    {
        uint64_t applied = 0U;
        uint64_t readback = 0U;
        uint64_t applied_error = UINT64_MAX;
        uint64_t readback_error = UINT64_MAX;

        BAII_SelfTest_Printf("[3] SET_RTC_TIME\r\n");

        if (BAII_SelfTest_SetRtc(
                transaction_id++,
                BAII_SELFTEST_RTC_VALUE_US,
                &applied) != 0U)
        {
            if (applied >= BAII_SELFTEST_RTC_VALUE_US)
            {
                applied_error = applied - BAII_SELFTEST_RTC_VALUE_US;
            }

            if (BAII_SelfTest_GetRtc(transaction_id++, &readback) != 0U)
            {
                if (readback >= BAII_SELFTEST_RTC_VALUE_US)
                {
                    readback_error = readback - BAII_SELFTEST_RTC_VALUE_US;
                }

                test3_pass =
                    ((applied_error <= 250000ULL) &&
                     (readback_error <= 500000ULL)) ? 1U : 0U;
            }
        }

        BAII_SelfTest_Printf("Requested      : %llu us\r\n",
               (unsigned long long)BAII_SELFTEST_RTC_VALUE_US);
        BAII_SelfTest_Printf("Applied        : %llu us\r\n",
               (unsigned long long)applied);
        BAII_SelfTest_Printf("Readback       : %llu us\r\n",
               (unsigned long long)readback);
        BAII_SelfTest_Printf("RESULT         : %s\r\n\r\n",
               (test3_pass != 0U) ? "PASS" : "FAIL");
    }

    /* ------------------------------------------------------------ */
    /* Test 4: SET_CAN_CONFIG to 250 kbit/s + GET_CAN_CONFIG.       */
    /* ------------------------------------------------------------ */
    {
        BAII_CanConfig applied;
        BAII_CanConfig readback;
        uint8_t set_ok;
        uint8_t get_ok;

        memset(&applied, 0, sizeof(applied));
        memset(&readback, 0, sizeof(readback));

        BAII_SelfTest_Printf("[4] SET_CAN_CONFIG 250000\r\n");

        set_ok = BAII_SelfTest_SetCan(
                transaction_id++,
                250000UL,
                &applied);

        get_ok = BAII_SelfTest_GetCan(
                transaction_id++,
                &readback);

        if (set_ok != 0U)
        {
            BAII_SelfTest_PrintCanConfig(&applied);
        }

        test4_pass =
            ((set_ok != 0U) &&
             (get_ok != 0U) &&
             (BAII_SelfTest_CheckCanTiming(&applied, 250000UL, 24U) != 0U) &&
             (BAII_SelfTest_CheckCanTiming(&readback, 250000UL, 24U) != 0U)) ?
            1U : 0U;

        BAII_SelfTest_Printf("GET readback   : %s\r\n",
               (get_ok != 0U) ? "OK" : "FAIL");
        BAII_SelfTest_Printf("RESULT         : %s\r\n\r\n",
               (test4_pass != 0U) ? "PASS" : "FAIL");
    }

    /* ------------------------------------------------------------ */
    /* Test 5: prove the 250 kbit/s setting receives real traffic.  */
    /* ------------------------------------------------------------ */
    {
        uint32_t frames = 0U;
        uint32_t fifo_lost = 0U;
        uint32_t read_errors = 0U;
        uint32_t frames_per_second = 0U;

        BAII_SelfTest_Printf("[5] CAN1 RECEIVE @ 250000\r\n");
        BAII_SelfTest_Printf("External generator must be transmitting at 250 kbit/s now.\r\n");
        BAII_SelfTest_Printf("Measuring for %lu ms...\r\n",
               (unsigned long)BAII_SELFTEST_CAN_WINDOW_MS);

        test5_pass = BAII_SelfTest_MeasureCanTraffic(
                BAII_SELFTEST_CAN_WINDOW_MS,
                &frames,
                &fifo_lost,
                &read_errors);

        if (BAII_SELFTEST_CAN_WINDOW_MS > 0U)
        {
            frames_per_second =
                (frames * 1000UL) / BAII_SELFTEST_CAN_WINDOW_MS;
        }

        BAII_SelfTest_Printf("Frames         : %lu\r\n", (unsigned long)frames);
        BAII_SelfTest_Printf("Approx rate    : %lu frame/s\r\n",
               (unsigned long)frames_per_second);
        BAII_SelfTest_Printf("FIFO lost      : %lu\r\n", (unsigned long)fifo_lost);
        BAII_SelfTest_Printf("Read errors    : %lu\r\n", (unsigned long)read_errors);
        BAII_SelfTest_Printf("RESULT         : %s\r\n\r\n",
               (test5_pass != 0U) ? "PASS" : "FAIL / NO 250k TRAFFIC");
    }

    /* ------------------------------------------------------------ */
    /* Test 6: switch generator, set 500 kbit/s, prove reception.   */
    /* ------------------------------------------------------------ */
    {
        BAII_CanConfig applied;
        BAII_CanConfig readback;
        uint32_t frames = 0U;
        uint32_t fifo_lost = 0U;
        uint32_t read_errors = 0U;
        uint32_t frames_per_second = 0U;
        uint8_t set_ok;
        uint8_t get_ok;
        uint8_t timing_ok;
        uint8_t traffic_ok;

        memset(&applied, 0, sizeof(applied));
        memset(&readback, 0, sizeof(readback));

        BAII_SelfTest_Printf("[6] SET_CAN_CONFIG 500000 + RECEIVE\r\n");
        BAII_SelfTest_Printf("Switch the external generator to 500 kbit/s now.\r\n");
        BAII_SelfTest_Printf("Keeping CAN service alive for %lu ms before reconfiguration...\r\n",
               (unsigned long)BAII_SELFTEST_CAN_SWITCH_DELAY_MS);

        BAII_SelfTest_PumpCan(BAII_SELFTEST_CAN_SWITCH_DELAY_MS);

        set_ok = BAII_SelfTest_SetCan(
                transaction_id++,
                500000UL,
                &applied);

        get_ok = BAII_SelfTest_GetCan(
                transaction_id++,
                &readback);

        timing_ok =
            ((set_ok != 0U) &&
             (get_ok != 0U) &&
             (BAII_SelfTest_CheckCanTiming(&applied, 500000UL, 12U) != 0U) &&
             (BAII_SelfTest_CheckCanTiming(&readback, 500000UL, 12U) != 0U)) ?
            1U : 0U;

        if (set_ok != 0U)
        {
            BAII_SelfTest_PrintCanConfig(&applied);
        }

        BAII_SelfTest_Printf("Measuring for %lu ms...\r\n",
               (unsigned long)BAII_SELFTEST_CAN_WINDOW_MS);

        traffic_ok = BAII_SelfTest_MeasureCanTraffic(
                BAII_SELFTEST_CAN_WINDOW_MS,
                &frames,
                &fifo_lost,
                &read_errors);

        if (BAII_SELFTEST_CAN_WINDOW_MS > 0U)
        {
            frames_per_second =
                (frames * 1000UL) / BAII_SELFTEST_CAN_WINDOW_MS;
        }

        BAII_SelfTest_Printf("Frames         : %lu\r\n", (unsigned long)frames);
        BAII_SelfTest_Printf("Approx rate    : %lu frame/s\r\n",
               (unsigned long)frames_per_second);
        BAII_SelfTest_Printf("FIFO lost      : %lu\r\n", (unsigned long)fifo_lost);
        BAII_SelfTest_Printf("Read errors    : %lu\r\n", (unsigned long)read_errors);

        test6_pass =
            ((timing_ok != 0U) && (traffic_ok != 0U)) ? 1U : 0U;

        BAII_SelfTest_Printf("Timing         : %s\r\n",
               (timing_ok != 0U) ? "PASS" : "FAIL");
        BAII_SelfTest_Printf("Traffic        : %s\r\n",
               (traffic_ok != 0U) ? "PASS" : "FAIL / NO 500k TRAFFIC");
        BAII_SelfTest_Printf("RESULT         : %s\r\n\r\n",
               (test6_pass != 0U) ? "PASS" : "FAIL");
    }

    BAII_SelfTest_Printf("----------------------------------------\r\n");
    BAII_SelfTest_Printf(" TEST 1 GET_INFO             : %s\r\n", test1_pass ? "PASS" : "FAIL");
    BAII_SelfTest_Printf(" TEST 2 GET_RTC_TIME         : %s\r\n", test2_pass ? "PASS" : "FAIL");
    BAII_SelfTest_Printf(" TEST 3 SET_RTC_TIME         : %s\r\n", test3_pass ? "PASS" : "FAIL");
    BAII_SelfTest_Printf(" TEST 4 SET_CAN_CONFIG 250k  : %s\r\n", test4_pass ? "PASS" : "FAIL");
    BAII_SelfTest_Printf(" TEST 5 RX @ 250k            : %s\r\n", test5_pass ? "PASS" : "FAIL");
    BAII_SelfTest_Printf(" TEST 6 SET/RX @ 500k        : %s\r\n", test6_pass ? "PASS" : "FAIL");
    BAII_SelfTest_Printf("----------------------------------------\r\n");

    if ((test1_pass != 0U) &&
        (test2_pass != 0U) &&
        (test3_pass != 0U) &&
        (test4_pass != 0U) &&
        (test5_pass != 0U) &&
        (test6_pass != 0U))
    {
        BAII_SelfTest_Printf(" PROTOCOL + HARDWARE SELF TEST: PASS\r\n");
    }
    else
    {
        BAII_SelfTest_Printf(" PROTOCOL + HARDWARE SELF TEST: FAIL\r\n");
    }

    BAII_SelfTest_Printf("----------------------------------------\r\n\r\n");
}
