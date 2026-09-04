#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "console.h"

#define BAII_SELFTEST_PRINT_BUFFER_SIZE 320U

/*
 * Self-test-only printf compatibility shim.
 *
 * The small embedded printf configuration used by the project does not
 * correctly render the %llu conversion used for 64-bit Unix timestamps.
 * The wrapper submits one complete formatted message to the DMA console.
 *
 * Current self-test %llu calls contain exactly one conversion per format
 * string. All other formats are rendered into the same bounded message
 * buffer before being queued.
 */

static size_t BAII_SelfTest_U64ToDecimal(
        uint64_t value,
        char output[21])
{
    char reverse[20];
    size_t digits = 0U;

    do
    {
        reverse[digits++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while ((value != 0ULL) && (digits < sizeof(reverse)));

    for (size_t i = 0U; i < digits; i++)
    {
        output[i] = reverse[digits - 1U - i];
    }

    output[digits] = '\0';
    return digits;
}


int BAII_SelfTest_Printf(const char *format, ...)
{
    const char *conversion;
    char buffer[BAII_SELFTEST_PRINT_BUFFER_SIZE];
    va_list args;
    int written;

    if (format == NULL)
    {
        return -1;
    }

    conversion = strstr(format, "%llu");

    va_start(args, format);

    if (conversion == NULL)
    {
        written = vsnprintf(buffer, sizeof(buffer), format, args);
    }
    else
    {
        char decimal[21];
        uint64_t value = (uint64_t)va_arg(args, unsigned long long);
        size_t prefix_length = (size_t)(conversion - format);
        size_t decimal_length = BAII_SelfTest_U64ToDecimal(value, decimal);
        const char *suffix = conversion + 4;
        size_t suffix_length = strlen(suffix);

        if ((prefix_length + decimal_length + suffix_length) >= sizeof(buffer))
        {
            written = -1;
        }
        else
        {
            (void)memcpy(buffer, format, prefix_length);
            (void)memcpy(&buffer[prefix_length], decimal, decimal_length);
            (void)memcpy(&buffer[prefix_length + decimal_length],
                         suffix, suffix_length + 1U);
            written = (int)(prefix_length + decimal_length + suffix_length);
        }
    }

    va_end(args);

    if (written > 0)
    {
        if ((size_t)written >= sizeof(buffer))
        {
            written = (int)sizeof(buffer) - 1;
            buffer[written] = '\0';
        }

        Console_Write(buffer);
    }

    return written;
}
