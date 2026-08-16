#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Self-test-only printf compatibility shim.
 *
 * The small embedded printf configuration used by the project does not
 * correctly render the %llu conversion used for 64-bit Unix timestamps.
 * Normal firmware printf calls are intentionally left untouched.
 *
 * Current self-test %llu calls contain exactly one conversion per format
 * string.  All other formats are passed directly to the normal vprintf().
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
    va_list args;
    int written = 0;

    if (format == NULL)
    {
        return -1;
    }

    conversion = strstr(format, "%llu");

    va_start(args, format);

    if (conversion == NULL)
    {
        written = vprintf(format, args);
    }
    else
    {
        char decimal[21];
        uint64_t value = (uint64_t)va_arg(args, unsigned long long);
        size_t prefix_length = (size_t)(conversion - format);
        size_t decimal_length = BAII_SelfTest_U64ToDecimal(value, decimal);
        const char *suffix = conversion + 4;
        size_t suffix_length = strlen(suffix);

        if (prefix_length > 0U)
        {
            written += (int)fwrite(format, 1U, prefix_length, stdout);
        }

        written += (int)fwrite(decimal, 1U, decimal_length, stdout);

        if (suffix_length > 0U)
        {
            written += (int)fwrite(suffix, 1U, suffix_length, stdout);
        }
    }

    va_end(args);
    return written;
}
