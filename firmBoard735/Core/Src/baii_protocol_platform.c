#include "baii_protocol_platform.h"

#include "main.h"
#include "can_sniffer.h"
#include "hyperram_capture.h"

#include <string.h>

/*
 * CubeMX currently clocks FDCAN from PLL2 at 96 MHz.
 * Keep this value synchronized with PeriphCommonClock_Config().
 */
#define BAII_FDCAN_KERNEL_CLOCK_HZ  96000000UL
#define BAII_HYPERRAM_SIZE_BYTES    (8UL * 1024UL * 1024UL)

#define BAII_FW_VERSION_MAJOR       0U
#define BAII_FW_VERSION_MINOR       1U
#define BAII_FW_VERSION_PATCH       0U

#define BAII_UNIX_2000_01_01        946684800ULL
#define BAII_UNIX_2100_01_01        4102444800ULL

extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan3;
extern RTC_HandleTypeDef hrtc;

typedef struct
{
    uint16_t prescaler;
    uint16_t time_seg1;
    uint16_t time_seg2;
    uint16_t sjw;
    uint16_t sample_point_permille;
    uint32_t bitrate;
} BAII_CanTiming;

static uint8_t BAII_IsLeapYear(uint32_t year)
{
    if ((year % 400U) == 0U)
    {
        return 1U;
    }

    if ((year % 100U) == 0U)
    {
        return 0U;
    }

    return ((year % 4U) == 0U) ? 1U : 0U;
}

static uint8_t BAII_DaysInMonth(uint32_t year, uint32_t month)
{
    static const uint8_t days[12] =
    {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };

    if ((month < 1U) || (month > 12U))
    {
        return 0U;
    }

    if ((month == 2U) && (BAII_IsLeapYear(year) != 0U))
    {
        return 29U;
    }

    return days[month - 1U];
}

static FDCAN_HandleTypeDef *BAII_SelectCan(uint8_t channel)
{
    if (channel == BAII_CAN_CHANNEL_1)
    {
        return &hfdcan1;
    }

    if (channel == BAII_CAN_CHANNEL_2)
    {
        return &hfdcan3;
    }

    return NULL;
}

static uint16_t BAII_AbsDiffU16(uint16_t a, uint16_t b)
{
    return (a >= b) ? (uint16_t)(a - b) : (uint16_t)(b - a);
}

static uint32_t BAII_AbsDiffU32(uint32_t a, uint32_t b)
{
    return (a >= b) ? (a - b) : (b - a);
}

/*
 * Find an exact bitrate and the closest requested sample point.
 *
 * STM32H7 nominal timing limits used here:
 *   prescaler : 1..512
 *   TSEG1     : 1..256
 *   TSEG2     : 1..128
 *
 * A secondary preference keeps the number of time quanta close to 16,
 * matching the current validated 500 kbit/s setup (12, 13, 2, SJW 2).
 */
static uint8_t BAII_SolveNominalTiming(
        uint32_t bitrate,
        uint16_t requested_sample_point_permille,
        BAII_CanTiming *timing)
{
    uint32_t best_score = UINT32_MAX;
    uint8_t found = 0U;
    uint16_t requested_sample_point = requested_sample_point_permille;

    if ((timing == NULL) ||
        (bitrate < 10000UL) ||
        (bitrate > 1000000UL))
    {
        return 0U;
    }

    if (requested_sample_point == 0U)
    {
        requested_sample_point = 875U;
    }

    if ((requested_sample_point < 500U) ||
        (requested_sample_point > 950U))
    {
        return 0U;
    }

    for (uint32_t total_tq = 4U; total_tq <= 385U; total_tq++)
    {
        uint64_t denominator = (uint64_t)bitrate * (uint64_t)total_tq;
        uint32_t prescaler;
        uint32_t sample_tq;
        uint32_t time_seg1;
        uint32_t time_seg2;
        uint16_t actual_sample_point;
        uint32_t score;

        if ((denominator == 0U) ||
            (((uint64_t)BAII_FDCAN_KERNEL_CLOCK_HZ % denominator) != 0U))
        {
            continue;
        }

        prescaler =
            (uint32_t)((uint64_t)BAII_FDCAN_KERNEL_CLOCK_HZ / denominator);

        if ((prescaler < 1U) || (prescaler > 512U))
        {
            continue;
        }

        sample_tq =
            ((uint32_t)requested_sample_point * total_tq + 500U) / 1000U;

        if (sample_tq < 2U)
        {
            sample_tq = 2U;
        }
        else if (sample_tq >= total_tq)
        {
            sample_tq = total_tq - 1U;
        }

        time_seg1 = sample_tq - 1U;
        time_seg2 = total_tq - sample_tq;

        if ((time_seg1 < 1U) || (time_seg1 > 256U) ||
            (time_seg2 < 1U) || (time_seg2 > 128U))
        {
            continue;
        }

        actual_sample_point =
            (uint16_t)(((sample_tq * 1000U) + (total_tq / 2U)) / total_tq);

        score =
            ((uint32_t)BAII_AbsDiffU16(
                    actual_sample_point,
                    requested_sample_point) * 1000UL) +
            BAII_AbsDiffU32(total_tq, 16U);

        if (score < best_score)
        {
            uint32_t sjw = (time_seg2 < 4U) ? time_seg2 : 4U;

            best_score = score;
            timing->prescaler = (uint16_t)prescaler;
            timing->time_seg1 = (uint16_t)time_seg1;
            timing->time_seg2 = (uint16_t)time_seg2;
            timing->sjw = (uint16_t)sjw;
            timing->sample_point_permille = actual_sample_point;
            timing->bitrate = bitrate;
            found = 1U;
        }
    }

    return found;
}

static BAII_StatusCode BAII_ReconfigureCan(
        FDCAN_HandleTypeDef *hfdcan,
        uint8_t channel,
        uint8_t mode,
        const BAII_CanTiming *timing)
{
    if ((hfdcan == NULL) || (timing == NULL))
    {
        return BAII_STATUS_INVALID_PARAM;
    }

    /* Stop may legitimately fail when the selected channel was not started. */
    (void)HAL_FDCAN_Stop(hfdcan);

    if (HAL_FDCAN_DeInit(hfdcan) != HAL_OK)
    {
        return BAII_STATUS_HAL_ERROR;
    }

    hfdcan->Init.FrameFormat = FDCAN_FRAME_CLASSIC;
    hfdcan->Init.Mode =
        (mode == BAII_CAN_MODE_LISTEN_ONLY) ?
        FDCAN_MODE_BUS_MONITORING : FDCAN_MODE_NORMAL;

    hfdcan->Init.NominalPrescaler = timing->prescaler;
    hfdcan->Init.NominalSyncJumpWidth = timing->sjw;
    hfdcan->Init.NominalTimeSeg1 = timing->time_seg1;
    hfdcan->Init.NominalTimeSeg2 = timing->time_seg2;

    if (HAL_FDCAN_Init(hfdcan) != HAL_OK)
    {
        return BAII_STATUS_HAL_ERROR;
    }

    if (channel == BAII_CAN_CHANNEL_1)
    {
        if (HAL_FDCAN_ConfigGlobalFilter(
                hfdcan,
                FDCAN_ACCEPT_IN_RX_FIFO0,
                FDCAN_ACCEPT_IN_RX_FIFO0,
                FDCAN_FILTER_REMOTE,
                FDCAN_FILTER_REMOTE) != HAL_OK)
        {
            return BAII_STATUS_HAL_ERROR;
        }

        if (HAL_FDCAN_ConfigTimestampCounter(
                hfdcan,
                FDCAN_TIMESTAMP_PRESC_1) != HAL_OK)
        {
            return BAII_STATUS_HAL_ERROR;
        }

        if (HAL_FDCAN_EnableTimestampCounter(
                hfdcan,
                FDCAN_TIMESTAMP_INTERNAL) != HAL_OK)
        {
            return BAII_STATUS_HAL_ERROR;
        }
    }

    if (HAL_FDCAN_Start(hfdcan) != HAL_OK)
    {
        return BAII_STATUS_HAL_ERROR;
    }

    return BAII_STATUS_OK;
}

static uint64_t BAII_CalendarToUnixSeconds(
        uint32_t year,
        uint32_t month,
        uint32_t day,
        uint32_t hour,
        uint32_t minute,
        uint32_t second)
{
    uint64_t days = 0U;

    for (uint32_t y = 1970U; y < year; y++)
    {
        days += (BAII_IsLeapYear(y) != 0U) ? 366U : 365U;
    }

    for (uint32_t m = 1U; m < month; m++)
    {
        days += BAII_DaysInMonth(year, m);
    }

    days += (uint64_t)(day - 1U);

    return (days * 86400ULL) +
           ((uint64_t)hour * 3600ULL) +
           ((uint64_t)minute * 60ULL) +
           (uint64_t)second;
}

BAII_StatusCode BAII_Platform_GetInfo(BAII_DeviceInfo *info)
{
    uint64_t rtc_time_us = 0U;

    if (info == NULL)
    {
        return BAII_STATUS_INVALID_PARAM;
    }

    memset(info, 0, sizeof(*info));

    info->firmware_major = BAII_FW_VERSION_MAJOR;
    info->firmware_minor = BAII_FW_VERSION_MINOR;
    info->firmware_patch = BAII_FW_VERSION_PATCH;
    info->capabilities =
        BAII_CAP_RTC |
        BAII_CAP_CAN_CONFIG |
        BAII_CAP_CAPTURE_STATUS |
        BAII_CAP_HYPERRAM;
    info->fdcan_clock_hz = BAII_FDCAN_KERNEL_CLOCK_HZ;
    info->hyperram_size_bytes = BAII_HYPERRAM_SIZE_BYTES;
    info->device_id = (uint32_t)(DBGMCU->IDCODE & 0x0FFFU);
    info->can_channel_count = 2U;
    info->rtc_valid =
        (BAII_Platform_GetRtcTime(&rtc_time_us) == BAII_STATUS_OK) ? 1U : 0U;

    return BAII_STATUS_OK;
}

BAII_StatusCode BAII_Platform_GetStatus(BAII_DeviceStatus *status)
{
    if (status == NULL)
    {
        return BAII_STATUS_INVALID_PARAM;
    }

    memset(status, 0, sizeof(*status));

    status->uptime_ms = HAL_GetTick();
    status->can_rx_frames = CAN_Sniffer_GetRxCount();
    status->sram_buffered_frames = CAN_Sniffer_GetBufferedCount();
    status->sram_dropped_frames = CAN_Sniffer_GetDroppedCount();
    status->fdcan_fifo_lost_events = CAN_Sniffer_GetFifoLostEvents();
    status->hyperram_stored_frames = HyperRAM_Capture_GetStoredCount();
    status->hyperram_write_errors = HyperRAM_Capture_GetWriteErrors();
    status->hyperram_lost_frames = HyperRAM_Capture_GetWriteLostFrames();
    status->hyperram_wrap_count = HyperRAM_Capture_GetWrapCount();

    return BAII_STATUS_OK;
}

BAII_StatusCode BAII_Platform_GetRtcTime(uint64_t *unix_time_us)
{
    RTC_TimeTypeDef time = {0};
    RTC_DateTypeDef date = {0};
    uint32_t year;
    uint64_t unix_seconds;
    uint64_t fractional_us = 0U;

    if (unix_time_us == NULL)
    {
        return BAII_STATUS_INVALID_PARAM;
    }

    if (HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN) != HAL_OK)
    {
        return BAII_STATUS_HAL_ERROR;
    }

    /* Reading the date unlocks the RTC shadow registers after GetTime(). */
    if (HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN) != HAL_OK)
    {
        return BAII_STATUS_HAL_ERROR;
    }

    year = 2000U + (uint32_t)date.Year;

    if ((year < 2000U) || (year > 2099U) ||
        (date.Month < 1U) || (date.Month > 12U) ||
        (date.Date < 1U) ||
        (date.Date > BAII_DaysInMonth(year, date.Month)) ||
        (time.Hours > 23U) ||
        (time.Minutes > 59U) ||
        (time.Seconds > 59U))
    {
        return BAII_STATUS_INTERNAL_ERROR;
    }

    unix_seconds = BAII_CalendarToUnixSeconds(
            year,
            date.Month,
            date.Date,
            time.Hours,
            time.Minutes,
            time.Seconds);

    if ((time.SecondFraction > 0U) &&
        (time.SubSeconds <= time.SecondFraction))
    {
        uint64_t elapsed_ticks =
            (uint64_t)time.SecondFraction - (uint64_t)time.SubSeconds;

        fractional_us =
            (elapsed_ticks * 1000000ULL) /
            ((uint64_t)time.SecondFraction + 1ULL);
    }

    *unix_time_us = (unix_seconds * 1000000ULL) + fractional_us;
    return BAII_STATUS_OK;
}

BAII_StatusCode BAII_Platform_SetRtcTime(uint64_t unix_time_us)
{
    RTC_TimeTypeDef time = {0};
    RTC_DateTypeDef date = {0};
    uint64_t unix_seconds = unix_time_us / 1000000ULL;
    uint64_t days;
    uint32_t seconds_of_day;
    uint32_t year = 1970U;
    uint32_t month = 1U;

    if ((unix_seconds < BAII_UNIX_2000_01_01) ||
        (unix_seconds >= BAII_UNIX_2100_01_01))
    {
        return BAII_STATUS_INVALID_PARAM;
    }

    days = unix_seconds / 86400ULL;
    seconds_of_day = (uint32_t)(unix_seconds % 86400ULL);

    while (year <= 2099U)
    {
        uint32_t days_in_year =
            (BAII_IsLeapYear(year) != 0U) ? 366U : 365U;

        if (days < days_in_year)
        {
            break;
        }

        days -= days_in_year;
        year++;
    }

    if ((year < 2000U) || (year > 2099U))
    {
        return BAII_STATUS_INVALID_PARAM;
    }

    while (month <= 12U)
    {
        uint32_t days_in_month = BAII_DaysInMonth(year, month);

        if (days < days_in_month)
        {
            break;
        }

        days -= days_in_month;
        month++;
    }

    if (month > 12U)
    {
        return BAII_STATUS_INTERNAL_ERROR;
    }

    time.Hours = (uint8_t)(seconds_of_day / 3600U);
    seconds_of_day %= 3600U;
    time.Minutes = (uint8_t)(seconds_of_day / 60U);
    time.Seconds = (uint8_t)(seconds_of_day % 60U);
    time.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    time.StoreOperation = RTC_STOREOPERATION_RESET;

    date.Year = (uint8_t)(year - 2000U);
    date.Month = (uint8_t)month;
    date.Date = (uint8_t)(days + 1U);

    /* RTC weekday encoding is Monday=1 ... Sunday=7. */
    date.WeekDay = (uint8_t)((((unix_seconds / 86400ULL) + 3ULL) % 7ULL) + 1ULL);

    if (HAL_RTC_SetTime(&hrtc, &time, RTC_FORMAT_BIN) != HAL_OK)
    {
        return BAII_STATUS_HAL_ERROR;
    }

    if (HAL_RTC_SetDate(&hrtc, &date, RTC_FORMAT_BIN) != HAL_OK)
    {
        return BAII_STATUS_HAL_ERROR;
    }

    return BAII_STATUS_OK;
}

BAII_StatusCode BAII_Platform_GetCanConfig(
        uint8_t channel,
        BAII_CanConfig *config)
{
    FDCAN_HandleTypeDef *hfdcan = BAII_SelectCan(channel);
    uint32_t total_tq;

    if ((hfdcan == NULL) || (config == NULL))
    {
        return BAII_STATUS_INVALID_PARAM;
    }

    memset(config, 0, sizeof(*config));

    total_tq = 1U +
               hfdcan->Init.NominalTimeSeg1 +
               hfdcan->Init.NominalTimeSeg2;

    if ((hfdcan->Init.NominalPrescaler == 0U) || (total_tq == 0U))
    {
        return BAII_STATUS_INTERNAL_ERROR;
    }

    config->channel = channel;
    config->mode =
        (hfdcan->Init.Mode == FDCAN_MODE_BUS_MONITORING) ?
        BAII_CAN_MODE_LISTEN_ONLY : BAII_CAN_MODE_NORMAL;

    config->frame_format =
        (hfdcan->Init.FrameFormat == FDCAN_FRAME_CLASSIC) ?
        BAII_CAN_FORMAT_CLASSIC : BAII_CAN_FORMAT_FD_BRS;

    config->fdcan_clock_hz = BAII_FDCAN_KERNEL_CLOCK_HZ;
    config->nominal_bitrate =
        BAII_FDCAN_KERNEL_CLOCK_HZ /
        (hfdcan->Init.NominalPrescaler * total_tq);

    config->nominal_sample_point_permille =
        (uint16_t)((((1U + hfdcan->Init.NominalTimeSeg1) * 1000U) +
                    (total_tq / 2U)) /
                   total_tq);

    config->nominal_prescaler = (uint16_t)hfdcan->Init.NominalPrescaler;
    config->nominal_time_seg1 = (uint16_t)hfdcan->Init.NominalTimeSeg1;
    config->nominal_time_seg2 = (uint16_t)hfdcan->Init.NominalTimeSeg2;
    config->nominal_sjw = (uint16_t)hfdcan->Init.NominalSyncJumpWidth;

    if (hfdcan->Init.FrameFormat != FDCAN_FRAME_CLASSIC)
    {
        uint32_t data_total_tq = 1U +
                                 hfdcan->Init.DataTimeSeg1 +
                                 hfdcan->Init.DataTimeSeg2;

        if ((hfdcan->Init.DataPrescaler > 0U) && (data_total_tq > 0U))
        {
            config->data_bitrate =
                BAII_FDCAN_KERNEL_CLOCK_HZ /
                (hfdcan->Init.DataPrescaler * data_total_tq);

            config->data_sample_point_permille =
                (uint16_t)((((1U + hfdcan->Init.DataTimeSeg1) * 1000U) +
                            (data_total_tq / 2U)) /
                           data_total_tq);
        }
    }

    return BAII_STATUS_OK;
}

BAII_StatusCode BAII_Platform_SetCanConfig(
        const BAII_CanConfig *requested,
        BAII_CanConfig *applied)
{
    FDCAN_HandleTypeDef *hfdcan;
    BAII_CanTiming timing;
    BAII_StatusCode status;

    if ((requested == NULL) || (applied == NULL))
    {
        return BAII_STATUS_INVALID_PARAM;
    }

    hfdcan = BAII_SelectCan(requested->channel);
    if (hfdcan == NULL)
    {
        return BAII_STATUS_INVALID_PARAM;
    }

    if ((requested->mode != BAII_CAN_MODE_NORMAL) &&
        (requested->mode != BAII_CAN_MODE_LISTEN_ONLY))
    {
        return BAII_STATUS_INVALID_PARAM;
    }

    /*
     * v0.1 changes timing for classic CAN only.  The wire format is already
     * CAN-FD capable, but the current acquisition FIFO/frame is 8 bytes.
     */
    if ((requested->frame_format != BAII_CAN_FORMAT_CLASSIC) ||
        (requested->data_bitrate != 0U) ||
        (requested->data_sample_point_permille != 0U))
    {
        return BAII_STATUS_NOT_SUPPORTED;
    }

    memset(&timing, 0, sizeof(timing));

    if (BAII_SolveNominalTiming(
            requested->nominal_bitrate,
            requested->nominal_sample_point_permille,
            &timing) == 0U)
    {
        return BAII_STATUS_INVALID_PARAM;
    }

    status = BAII_ReconfigureCan(
            hfdcan,
            requested->channel,
            requested->mode,
            &timing);

    if (status != BAII_STATUS_OK)
    {
        return status;
    }

    return BAII_Platform_GetCanConfig(requested->channel, applied);
}
