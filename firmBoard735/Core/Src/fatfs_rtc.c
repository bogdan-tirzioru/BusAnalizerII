#include "stm32h7xx_hal.h"
#include "stm32h7xx_hal_rtc.h"
#include "ff.h"


extern RTC_HandleTypeDef hrtc;
DWORD get_fattime(void)
{
    RTC_TimeTypeDef time = {0};
    RTC_DateTypeDef date = {0};

    if (HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN) != HAL_OK)
    {
        /* Obvious fallback: 1980-01-01 00:00:00 */
        return ((DWORD)0U << 25) |
               ((DWORD)1U << 21) |
               ((DWORD)1U << 16);
    }

    /*
     * HAL_RTC_GetDate() must follow HAL_RTC_GetTime()
     * when reading the calendar.
     */
    if (HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN) != HAL_OK)
    {
        return ((DWORD)0U << 25) |
               ((DWORD)1U << 21) |
               ((DWORD)1U << 16);
    }

    uint32_t year = 2000U + date.Year;

    /*
     * FAT timestamp format supports years 1980..2107.
     */
    if ((year < 1980U) || (year > 2107U) ||
        (date.Month < 1U) || (date.Month > 12U) ||
        (date.Date < 1U) || (date.Date > 31U))
    {
        return ((DWORD)0U << 25) |
               ((DWORD)1U << 21) |
               ((DWORD)1U << 16);
    }

    return ((DWORD)(year - 1980U) << 25) |
           ((DWORD)date.Month        << 21) |
           ((DWORD)date.Date         << 16) |
           ((DWORD)time.Hours        << 11) |
           ((DWORD)time.Minutes      << 5)  |
           ((DWORD)time.Seconds      >> 1);
}
