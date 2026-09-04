#include "rtc_test.h"
#include "console.h"

#include <stdio.h>
#include <string.h>


static uint32_t rtc_seconds_of_day(const RTC_TimeTypeDef *time)
{
    return ((uint32_t)time->Hours * 3600U) +
           ((uint32_t)time->Minutes * 60U) +
           (uint32_t)time->Seconds;
}


void RTC_Test(RTC_HandleTypeDef *hrtc,
              UART_HandleTypeDef *huart)
{
    (void)huart;

    char buffer[256];

    RTC_TimeTypeDef time1 = {0};
    RTC_DateTypeDef date1 = {0};

    RTC_TimeTypeDef time2 = {0};
    RTC_DateTypeDef date2 = {0};

    Console_Printf("\r\n--- RTC / LSE TEST ---\r\n");


    /* ---------------------------------------------------------
     * Check LSE oscillator
     * --------------------------------------------------------- */

    if (__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY) != RESET)
    {
        Console_Printf("LSE status    : READY\r\n");
    }
    else
    {
        Console_Printf("LSE status    : NOT READY\r\n");
        Console_Printf("RTC TEST      : FAIL\r\n");
        Console_Printf("--- RTC / LSE TEST FAILED ---\r\n");
        return;
    }


    /* ---------------------------------------------------------
     * Check RTC clock source
     * --------------------------------------------------------- */

    uint32_t rtc_source = __HAL_RCC_GET_RTC_SOURCE();

    if (rtc_source == RCC_RTCCLKSOURCE_LSE)
    {
        Console_Printf("RTC source    : LSE 32768 Hz\r\n");
    }
    else
    {
        snprintf(buffer,
                 sizeof(buffer),
                 "RTC source    : NOT LSE (0x%08lX)\r\n",
                 (unsigned long)rtc_source);

        Console_Write(buffer);
        Console_Printf("RTC TEST      : FAIL\r\n--- RTC / LSE TEST FAILED ---\r\n");

        return;
    }


    snprintf(buffer,
             sizeof(buffer),
             "Async prediv   : %lu\r\n"
             "Sync prediv    : %lu\r\n",
             (unsigned long)hrtc->Init.AsynchPrediv,
             (unsigned long)hrtc->Init.SynchPrediv);

    Console_Write(buffer);


    /* ---------------------------------------------------------
     * First RTC reading
     * --------------------------------------------------------- */

    if (HAL_RTC_GetTime(hrtc,
                        &time1,
                        RTC_FORMAT_BIN) != HAL_OK)
    {
        Console_Printf("RTC read #1   : FAIL\r\nRTC TEST      : FAIL\r\n");
        return;
    }

    /*
     * MUST read date after time.
     */
    if (HAL_RTC_GetDate(hrtc,
                        &date1,
                        RTC_FORMAT_BIN) != HAL_OK)
    {
        Console_Printf("RTC date #1   : FAIL\r\nRTC TEST      : FAIL\r\n");
        return;
    }


    snprintf(buffer,
             sizeof(buffer),
             "Time #1        : %02u:%02u:%02u\r\n",
             time1.Hours,
             time1.Minutes,
             time1.Seconds);

    Console_Write(buffer);


    /* Wait long enough to cross at least one RTC second */

    HAL_Delay(1200);


    /* ---------------------------------------------------------
     * Second RTC reading
     * --------------------------------------------------------- */

    if (HAL_RTC_GetTime(hrtc,
                        &time2,
                        RTC_FORMAT_BIN) != HAL_OK)
    {
        Console_Printf("RTC read #2   : FAIL\r\nRTC TEST      : FAIL\r\n");
        return;
    }

    if (HAL_RTC_GetDate(hrtc,
                        &date2,
                        RTC_FORMAT_BIN) != HAL_OK)
    {
        Console_Printf("RTC date #2   : FAIL\r\nRTC TEST      : FAIL\r\n");
        return;
    }


    snprintf(buffer,
             sizeof(buffer),
             "Time #2        : %02u:%02u:%02u\r\n",
             time2.Hours,
             time2.Minutes,
             time2.Seconds);

    Console_Write(buffer);


    /* ---------------------------------------------------------
     * Verify RTC advanced
     * --------------------------------------------------------- */

    uint32_t sec1 = rtc_seconds_of_day(&time1);
    uint32_t sec2 = rtc_seconds_of_day(&time2);

    uint32_t delta =
        (sec2 + 86400U - sec1) % 86400U;


    snprintf(buffer,
             sizeof(buffer),
             "RTC advance    : %lu second(s)\r\n",
             (unsigned long)delta);

    Console_Write(buffer);


    if ((delta >= 1U) && (delta <= 2U))
    {
        Console_Printf("RTC TEST      : PASS\r\n--- RTC / LSE TEST PASSED ---\r\n");
    }
    else
    {
        Console_Printf("RTC TEST      : FAIL\r\n--- RTC / LSE TEST FAILED ---\r\n");
    }
}
