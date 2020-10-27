/**
 * Copyright (C) Mellanox Technologies Ltd. 2019.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCS_STRING_BUFFER_H_
#define UCS_STRING_BUFFER_H_

#include <ucs/sys/compiler_def.h>
#include <ucs/type/status.h>
#include <stddef.h>
#include <ucs/datastruct/array.h>


BEGIN_C_DECLS

UCS_ARRAY_DECLARE_TYPE(string_buffer, size_t, char)


/**
 * Declare a string buffer which is using a static array as backing store.
 * Such string buffer does not allocate additional memory and does not have to
 * be cleaned-up.
 *
 * @param _var     String buffer variable name
 * @param _buffer  Buffer to use as backing store.
 *
 * Example:
 *
 * @code{.c}
 * char buffer[100];
 * UCS_STRING_BUFFER_FIXED(strb, buffer);
 *
 * ucs_string_buffer_appendf(&strb, "%x%x", 57005, 48879);
 * @endcode
 */
#define UCS_STRING_BUFFER_FIXED(_var, _buffer) \
    ucs_string_buffer_t _var = { \
        UCS_ARRAY_FIXED_INITIALIZER((_buffer), ucs_static_array_size(_buffer)) \
    }


/**
 * String buffer - a dynamic NULL-terminated character buffer which can grow
 * on demand.
 */
typedef struct ucs_string_buffer {
    ucs_array_t(string_buffer) str;
} ucs_string_buffer_t;


/**
 * Initialize a string buffer
 *
 * @param [out] strb   String buffer to initialize.
 */
void ucs_string_buffer_init(ucs_string_buffer_t *strb);


/**
 * Cleanup a string buffer and release any memory associated with it.
 *
 * @param [out] strb   String buffer to clean up.
 */
void ucs_string_buffer_cleanup(ucs_string_buffer_t *strb);


/**
 * Get the number of characters in a string buffer
 *
 * @param [out] strb   Return the length of this string buffer.
 *
 * @return String buffer length.
 */
size_t ucs_string_buffer_length(ucs_string_buffer_t *strb);


/**
 * Append a formatted string to the string buffer.
 *
 * @param [inout] strb   String buffer to append to.
 * @param [in]    fmt    Format string.
 *
 * @note If the string cannot grow to the required length, only some of the
 *       characters would be appended.
 */
void ucs_string_buffer_appendf(ucs_string_buffer_t *strb, const char *fmt, ...)
    UCS_F_PRINTF(2, 3);


/**
 * Remove specific characters from the end of the string.
 *
 * @param [inout] strb     String buffer remote characters from.
 * @param [in]    charset  C-string with the set of characters to remove.
 *                         If NULL, this function removes whitespace characters,
 *                         as defined by isspace (3).
 *
 * This function removes the largest contiguous suffix from the input string
 * 'strb', which consists entirely of characters in 'charset'.
 */
void ucs_string_buffer_rtrim(ucs_string_buffer_t *strb, const char *charset);


/**
 * Return a temporary pointer to a C-style string which represents the string
 * buffer. The returned string is valid only as long as no other operation is
 * done on the string buffer (including append).
 *
 * @param [in]   strb   String buffer to convert to a C-style string
 *
 * @return C-style string representing the data in the buffer.
 */
const char *ucs_string_buffer_cstr(const ucs_string_buffer_t *strb);


END_C_DECLS

#endif
