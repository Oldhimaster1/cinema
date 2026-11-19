#include "cinema_tinyfmt.h"

#include <stdarg.h>
#include <stdint.h>

typedef struct {
    char *buffer;
    size_t capacity;
    size_t used;
} cinema_fmt_out_t;

static void fmt_char(cinema_fmt_out_t *out, char c)
{
    if (out->capacity && out->used < out->capacity - 1u)
        out->buffer[out->used] = c;
    out->used++;
}

static void fmt_string(cinema_fmt_out_t *out, const char *string)
{
    if (!string) string = "(null)";
    while (*string) fmt_char(out, *string++);
}

static void fmt_number(cinema_fmt_out_t *out, uint32_t value, unsigned base,
                       unsigned width, char sign, char pad)
{
    char digits[12];
    unsigned count = 0, needed;
    do {
        unsigned digit = (unsigned)(value % base);
        digits[count++] = (char)(digit < 10u ? '0' + digit : 'A' + digit - 10u);
        value /= base;
    } while (value && count < sizeof(digits));
    needed = count + (sign ? 1u : 0u);
    if (pad == '0' && sign) { fmt_char(out, sign); sign = 0; }
    while (needed < width) { fmt_char(out, pad); ++needed; }
    if (sign) fmt_char(out, sign);
    while (count) fmt_char(out, digits[--count]);
}

static int cinema_vformat(char *buffer, size_t size, const char *format, va_list args)
{
    cinema_fmt_out_t out = {buffer, size, 0};
    while (*format) {
        unsigned width = 0;
        char pad = ' ', sign = 0, spec;
        int is_long = 0, plus = 0;
        uint32_t value;
        if (*format != '%') { fmt_char(&out, *format++); continue; }
        ++format;
        if (*format == '%') { fmt_char(&out, *format++); continue; }
        if (*format == '+') { plus = 1; ++format; }
        if (*format == '0') { pad = '0'; ++format; }
        while (*format >= '0' && *format <= '9') {
            width = width * 10u + (unsigned)(*format++ - '0');
            if (width > 99u) width = 99u;
        }
        if (*format == 'l') { is_long = 1; ++format; }
        spec = *format;
        if (!spec) break;
        ++format;
        switch (spec) {
            case 's': fmt_string(&out, va_arg(args, const char *)); break;
            case 'c': fmt_char(&out, (char)va_arg(args, int)); break;
            case 'd': {
                int32_t signed_value = is_long ? (int32_t)va_arg(args, long)
                                               : (int32_t)va_arg(args, int);
                if (signed_value < 0) {
                    sign = '-';
                    value = 0u - (uint32_t)signed_value;
                } else {
                    if (plus) sign = '+';
                    value = (uint32_t)signed_value;
                }
                fmt_number(&out, value, 10u, width, sign, pad);
                break;
            }
            case 'u':
            case 'X':
                value = is_long ? (uint32_t)va_arg(args, unsigned long)
                                : (uint32_t)va_arg(args, unsigned int);
                fmt_number(&out, value, spec == 'X' ? 16u : 10u, width, 0, pad);
                break;
            default:
                fmt_char(&out, '%');
                fmt_char(&out, spec);
                break;
        }
    }
    if (size) out.buffer[out.used < size ? out.used : size - 1u] = '\0';
    return (int)out.used;
}

int cinema_snprintf(char *buffer, size_t size, const char *format, ...)
{
    int result;
    va_list args;
    va_start(args, format);
    result = cinema_vformat(buffer, size, format, args);
    va_end(args);
    return result;
}

int cinema_sprintf(char *buffer, const char *format, ...)
{
    int result;
    va_list args;
    va_start(args, format);
    result = cinema_vformat(buffer, (size_t)-1, format, args);
    va_end(args);
    return result;
}
