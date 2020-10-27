/**
 * Copyright (C) Mellanox Technologies Ltd. 2019.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "string_buffer.h"

#include <ucs/debug/assert.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack.h>
#include <ucs/sys/math.h>
#include <string.h>
#include <ctype.h>

#include <ucs/datastruct/array.inl>


/* Minimal reserve size when appending new data */
#define UCS_STRING_BUFFER_RESERVE  32

UCS_ARRAY_IMPL(string_buffer, size_t, char, static UCS_F_ALWAYS_INLINE)

void ucs_string_buffer_init(ucs_string_buffer_t *strb)
{
    ucs_array_init_dynamic(string_buffer, &strb->str);
}

void ucs_string_buffer_cleanup(ucs_string_buffer_t *strb)
{
    ucs_array_cleanup_dynamic(string_buffer, &strb->str);
}

size_t ucs_string_buffer_length(ucs_string_buffer_t *strb)
{
    return ucs_array_length(&strb->str);
}

void ucs_string_buffer_appendf(ucs_string_buffer_t *strb, const char *fmt, ...)
{
    ucs_status_t status;
    size_t max_print;
    va_list ap;
    int ret;

    ucs_array_reserve(string_buffer, &strb->str,
                      ucs_array_length(&strb->str) + UCS_STRING_BUFFER_RESERVE);

    /* try to write to existing buffer */
    va_start(ap, fmt);
    max_print = ucs_array_available_length(&strb->str);
    ret       = vsnprintf(ucs_array_end(&strb->str), max_print, fmt, ap);
    va_end(ap);

    /* if failed, grow the buffer accommodate for the expected extra length */
    if (ret >= max_print) {
        status = ucs_array_reserve(string_buffer, &strb->str,
                                   ucs_array_length(&strb->str) + ret + 1);
        if (status != UCS_OK) {
            /* cannot grow the buffer, just set null terminator at the end, and
             * the string will contain only what could fit in.
             */
            ucs_array_length(&strb->str) = ucs_array_capacity(&strb->str) - 1;
            *ucs_array_end(&strb->str)   = '\0';
            goto out;
        }

        va_start(ap, fmt);
        max_print = ucs_array_available_length(&strb->str);
        ret       = vsnprintf(ucs_array_end(&strb->str), max_print, fmt, ap);
        va_end(ap);

        /* since we've grown the buffer, it should be sufficient now */
        ucs_assertv(ret < max_print, "ret=%d max_print=%zu", ret, max_print);
    }

    /* string length grows by the amount of characters written by vsnprintf */
    ucs_array_set_length(&strb->str, ucs_array_length(&strb->str) + ret);

    /* \0 should be written by vsnprintf */
out:
    ucs_assert(ucs_array_available_length(&strb->str) >= 1);
    ucs_assert(*ucs_array_end(&strb->str) == '\0');
}

void ucs_string_buffer_rtrim(ucs_string_buffer_t *strb, const char *charset)
{
    char *ptr = ucs_array_end(&strb->str);

    while (ucs_array_length(&strb->str) > 0) {
        --ptr;
        if (((charset == NULL) && !isspace(*ptr)) ||
            ((charset != NULL) && (strchr(charset, *ptr) == NULL))) {
            /* if the last character should NOT be removed - stop */
            break;
        }

        ucs_array_set_length(&strb->str, ucs_array_length(&strb->str) - 1);
    }

    /* mark the new end of string */
    *(ptr + 1) = '\0';
}

const char *ucs_string_buffer_cstr(const ucs_string_buffer_t *strb)
{
    char *c_str;

    if (ucs_array_is_empty(&strb->str)) {
        return "";
    }

    c_str = ucs_array_begin(&strb->str);
    ucs_assert(c_str != NULL);
    return c_str;
}
