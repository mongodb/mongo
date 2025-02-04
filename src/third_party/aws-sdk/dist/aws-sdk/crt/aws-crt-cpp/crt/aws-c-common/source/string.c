/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/string.h>

#ifdef _WIN32
#    include <windows.h>

struct aws_wstring *aws_string_convert_to_wstring(
    struct aws_allocator *allocator,
    const struct aws_string *to_convert) {
    AWS_PRECONDITION(to_convert);

    struct aws_byte_cursor convert_cur = aws_byte_cursor_from_string(to_convert);
    return aws_string_convert_to_wchar_from_byte_cursor(allocator, &convert_cur);
}

struct aws_wstring *aws_string_convert_to_wchar_from_byte_cursor(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *to_convert) {
    AWS_PRECONDITION(to_convert);

    /* if a length is passed for the to_convert string, converted size does not include the null terminator,
     * which is a good thing. */
    int converted_size = MultiByteToWideChar(CP_UTF8, 0, (const char *)to_convert->ptr, (int)to_convert->len, NULL, 0);

    if (!converted_size) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    size_t str_len_size = 0;
    size_t malloc_size = 0;

    /* double the size because the return value above is # of characters, not bytes size. */
    if (aws_mul_size_checked(sizeof(wchar_t), converted_size, &str_len_size)) {
        return NULL;
    }

    /* UTF-16, the NULL terminator is two bytes. */
    if (aws_add_size_checked(sizeof(struct aws_wstring) + 2, str_len_size, &malloc_size)) {
        return NULL;
    }

    struct aws_wstring *str = aws_mem_acquire(allocator, malloc_size);
    if (!str) {
        return NULL;
    }

    /* Fields are declared const, so we need to copy them in like this */
    *(struct aws_allocator **)(&str->allocator) = allocator;
    *(size_t *)(&str->len) = (size_t)converted_size;

    int converted_res = MultiByteToWideChar(
        CP_UTF8, 0, (const char *)to_convert->ptr, (int)to_convert->len, (wchar_t *)str->bytes, converted_size);
    /* windows had its chance to do its thing, no take backsies. */
    AWS_FATAL_ASSERT(converted_res > 0);

    *(wchar_t *)&str->bytes[converted_size] = 0;
    return str;
}

struct aws_wstring *aws_wstring_new_from_cursor(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *w_str_cur) {
    AWS_PRECONDITION(allocator && aws_byte_cursor_is_valid(w_str_cur));
    return aws_wstring_new_from_array(allocator, (wchar_t *)w_str_cur->ptr, w_str_cur->len / sizeof(wchar_t));
}

struct aws_wstring *aws_wstring_new_from_array(struct aws_allocator *allocator, const wchar_t *w_str, size_t len) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(AWS_MEM_IS_READABLE(w_str, len));

    size_t str_byte_len = 0;
    size_t malloc_size = 0;

    /* double the size because the return value above is # of characters, not bytes size. */
    if (aws_mul_size_checked(sizeof(wchar_t), len, &str_byte_len)) {
        return NULL;
    }

    /* UTF-16, the NULL terminator is two bytes. */
    if (aws_add_size_checked(sizeof(struct aws_wstring) + 2, str_byte_len, &malloc_size)) {
        return NULL;
    }

    struct aws_wstring *str = aws_mem_acquire(allocator, malloc_size);

    /* Fields are declared const, so we need to copy them in like this */
    *(struct aws_allocator **)(&str->allocator) = allocator;
    *(size_t *)(&str->len) = len;
    if (len > 0) {
        memcpy((void *)str->bytes, w_str, str_byte_len);
    }
    /* in case this is a utf-16 string in the array, allow that here. */
    *(wchar_t *)&str->bytes[len] = 0;
    AWS_RETURN_WITH_POSTCONDITION(str, aws_wstring_is_valid(str));
}

bool aws_wstring_is_valid(const struct aws_wstring *str) {
    return str && AWS_MEM_IS_READABLE(&str->bytes[0], str->len + 1) && str->bytes[str->len] == 0;
}

void aws_wstring_destroy(struct aws_wstring *str) {
    AWS_PRECONDITION(!str || aws_wstring_is_valid(str));
    if (str && str->allocator) {
        aws_mem_release(str->allocator, str);
    }
}

static struct aws_string *s_convert_from_wchar(
    struct aws_allocator *allocator,
    const wchar_t *to_convert,
    int len_chars) {
    AWS_FATAL_PRECONDITION(to_convert);

    int bytes_size = WideCharToMultiByte(CP_UTF8, 0, to_convert, len_chars, NULL, 0, NULL, NULL);

    if (!bytes_size) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    size_t malloc_size = 0;

    /* bytes_size already contains the space for the null terminator */
    if (aws_add_size_checked(sizeof(struct aws_string), bytes_size, &malloc_size)) {
        return NULL;
    }

    struct aws_string *str = aws_mem_acquire(allocator, malloc_size);
    if (!str) {
        return NULL;
    }

    /* Fields are declared const, so we need to copy them in like this */
    *(struct aws_allocator **)(&str->allocator) = allocator;
    *(size_t *)(&str->len) = (size_t)bytes_size - 1;

    int converted_res =
        WideCharToMultiByte(CP_UTF8, 0, to_convert, len_chars, (char *)str->bytes, bytes_size, NULL, NULL);
    /* windows had its chance to do its thing, no take backsies. */
    AWS_FATAL_ASSERT(converted_res > 0);

    *(uint8_t *)&str->bytes[str->len] = 0;
    return str;
}

struct aws_string *aws_string_convert_from_wchar_str(
    struct aws_allocator *allocator,
    const struct aws_wstring *to_convert) {
    AWS_FATAL_PRECONDITION(to_convert);

    return s_convert_from_wchar(allocator, aws_wstring_c_str(to_convert), (int)aws_wstring_num_chars(to_convert));
}
struct aws_string *aws_string_convert_from_wchar_c_str(struct aws_allocator *allocator, const wchar_t *to_convert) {
    return s_convert_from_wchar(allocator, to_convert, -1);
}

const wchar_t *aws_wstring_c_str(const struct aws_wstring *str) {
    AWS_PRECONDITION(str);
    return str->bytes;
}

size_t aws_wstring_num_chars(const struct aws_wstring *str) {
    AWS_PRECONDITION(str);

    if (str->len == 0) {
        return 0;
    }

    return str->len;
}

size_t aws_wstring_size_bytes(const struct aws_wstring *str) {
    AWS_PRECONDITION(str);

    return aws_wstring_num_chars(str) * sizeof(wchar_t);
}

#endif /* _WIN32 */

struct aws_string *aws_string_new_from_c_str(struct aws_allocator *allocator, const char *c_str) {
    AWS_PRECONDITION(allocator && c_str);
    return aws_string_new_from_array(allocator, (const uint8_t *)c_str, strlen(c_str));
}

struct aws_string *aws_string_new_from_array(struct aws_allocator *allocator, const uint8_t *bytes, size_t len) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(AWS_MEM_IS_READABLE(bytes, len));

    struct aws_string *str = aws_mem_acquire(allocator, offsetof(struct aws_string, bytes[len + 1]));
    if (!str) {
        return NULL;
    }

    /* Fields are declared const, so we need to copy them in like this */
    *(struct aws_allocator **)(&str->allocator) = allocator;
    *(size_t *)(&str->len) = len;
    if (len > 0) {
        memcpy((void *)str->bytes, bytes, len);
    }
    *(uint8_t *)&str->bytes[len] = 0;
    AWS_RETURN_WITH_POSTCONDITION(str, aws_string_is_valid(str));
}

struct aws_string *aws_string_new_from_string(struct aws_allocator *allocator, const struct aws_string *str) {
    AWS_PRECONDITION(allocator && aws_string_is_valid(str));
    return aws_string_new_from_array(allocator, str->bytes, str->len);
}

struct aws_string *aws_string_new_from_cursor(struct aws_allocator *allocator, const struct aws_byte_cursor *cursor) {
    AWS_PRECONDITION(allocator && aws_byte_cursor_is_valid(cursor));
    return aws_string_new_from_array(allocator, cursor->ptr, cursor->len);
}

struct aws_string *aws_string_new_from_buf(struct aws_allocator *allocator, const struct aws_byte_buf *buf) {
    AWS_PRECONDITION(allocator && aws_byte_buf_is_valid(buf));
    return aws_string_new_from_array(allocator, buf->buffer, buf->len);
}

void aws_string_destroy(struct aws_string *str) {
    AWS_PRECONDITION(!str || aws_string_is_valid(str));
    if (str && str->allocator) {
        aws_mem_release(str->allocator, str);
    }
}

void aws_string_destroy_secure(struct aws_string *str) {
    AWS_PRECONDITION(!str || aws_string_is_valid(str));
    if (str) {
        aws_secure_zero((void *)aws_string_bytes(str), str->len);
        if (str->allocator) {
            aws_mem_release(str->allocator, str);
        }
    }
}

int aws_string_compare(const struct aws_string *a, const struct aws_string *b) {
    AWS_PRECONDITION(!a || aws_string_is_valid(a));
    AWS_PRECONDITION(!b || aws_string_is_valid(b));
    if (a == b) {
        return 0; /* strings identical */
    }
    if (a == NULL) {
        return -1;
    }
    if (b == NULL) {
        return 1;
    }

    size_t len_a = a->len;
    size_t len_b = b->len;
    size_t min_len = len_a < len_b ? len_a : len_b;

    int ret = memcmp(aws_string_bytes(a), aws_string_bytes(b), min_len);
    AWS_POSTCONDITION(aws_string_is_valid(a));
    AWS_POSTCONDITION(aws_string_is_valid(b));
    if (ret) {
        return ret; /* overlapping characters differ */
    }
    if (len_a == len_b) {
        return 0; /* strings identical */
    }
    if (len_a > len_b) {
        return 1; /* string b is first n characters of string a */
    }
    return -1; /* string a is first n characters of string b */
}

int aws_array_list_comparator_string(const void *a, const void *b) {
    if (a == b) {
        return 0; /* strings identical */
    }
    if (a == NULL) {
        return -1;
    }
    if (b == NULL) {
        return 1;
    }
    const struct aws_string *str_a = *(const struct aws_string **)a;
    const struct aws_string *str_b = *(const struct aws_string **)b;
    return aws_string_compare(str_a, str_b);
}

/**
 * Returns true if bytes of string are the same, false otherwise.
 */
bool aws_string_eq(const struct aws_string *a, const struct aws_string *b) {
    AWS_PRECONDITION(!a || aws_string_is_valid(a));
    AWS_PRECONDITION(!b || aws_string_is_valid(b));
    if (a == b) {
        return true;
    }
    if (a == NULL || b == NULL) {
        return false;
    }
    return aws_array_eq(a->bytes, a->len, b->bytes, b->len);
}

/**
 * Returns true if bytes of string are equivalent, using a case-insensitive comparison.
 */
bool aws_string_eq_ignore_case(const struct aws_string *a, const struct aws_string *b) {
    AWS_PRECONDITION(!a || aws_string_is_valid(a));
    AWS_PRECONDITION(!b || aws_string_is_valid(b));
    if (a == b) {
        return true;
    }
    if (a == NULL || b == NULL) {
        return false;
    }
    return aws_array_eq_ignore_case(a->bytes, a->len, b->bytes, b->len);
}

/**
 * Returns true if bytes of string and cursor are the same, false otherwise.
 */
bool aws_string_eq_byte_cursor(const struct aws_string *str, const struct aws_byte_cursor *cur) {
    AWS_PRECONDITION(!str || aws_string_is_valid(str));
    AWS_PRECONDITION(!cur || aws_byte_cursor_is_valid(cur));
    if (str == NULL && cur == NULL) {
        return true;
    }
    if (str == NULL || cur == NULL) {
        return false;
    }
    return aws_array_eq(str->bytes, str->len, cur->ptr, cur->len);
}

/**
 * Returns true if bytes of string and cursor are equivalent, using a case-insensitive comparison.
 */

bool aws_string_eq_byte_cursor_ignore_case(const struct aws_string *str, const struct aws_byte_cursor *cur) {
    AWS_PRECONDITION(!str || aws_string_is_valid(str));
    AWS_PRECONDITION(!cur || aws_byte_cursor_is_valid(cur));
    if (str == NULL && cur == NULL) {
        return true;
    }
    if (str == NULL || cur == NULL) {
        return false;
    }
    return aws_array_eq_ignore_case(str->bytes, str->len, cur->ptr, cur->len);
}

/**
 * Returns true if bytes of string and buffer are the same, false otherwise.
 */
bool aws_string_eq_byte_buf(const struct aws_string *str, const struct aws_byte_buf *buf) {
    AWS_PRECONDITION(!str || aws_string_is_valid(str));
    AWS_PRECONDITION(!buf || aws_byte_buf_is_valid(buf));
    if (str == NULL && buf == NULL) {
        return true;
    }
    if (str == NULL || buf == NULL) {
        return false;
    }
    return aws_array_eq(str->bytes, str->len, buf->buffer, buf->len);
}

/**
 * Returns true if bytes of string and buffer are equivalent, using a case-insensitive comparison.
 */

bool aws_string_eq_byte_buf_ignore_case(const struct aws_string *str, const struct aws_byte_buf *buf) {
    AWS_PRECONDITION(!str || aws_string_is_valid(str));
    AWS_PRECONDITION(!buf || aws_byte_buf_is_valid(buf));
    if (str == NULL && buf == NULL) {
        return true;
    }
    if (str == NULL || buf == NULL) {
        return false;
    }
    return aws_array_eq_ignore_case(str->bytes, str->len, buf->buffer, buf->len);
}

bool aws_string_eq_c_str(const struct aws_string *str, const char *c_str) {
    AWS_PRECONDITION(!str || aws_string_is_valid(str));
    if (str == NULL && c_str == NULL) {
        return true;
    }
    if (str == NULL || c_str == NULL) {
        return false;
    }
    return aws_array_eq_c_str(str->bytes, str->len, c_str);
}

/**
 * Returns true if bytes of strings are equivalent, using a case-insensitive comparison.
 */
bool aws_string_eq_c_str_ignore_case(const struct aws_string *str, const char *c_str) {
    AWS_PRECONDITION(!str || aws_string_is_valid(str));
    if (str == NULL && c_str == NULL) {
        return true;
    }
    if (str == NULL || c_str == NULL) {
        return false;
    }
    return aws_array_eq_c_str_ignore_case(str->bytes, str->len, c_str);
}

bool aws_byte_buf_write_from_whole_string(
    struct aws_byte_buf *AWS_RESTRICT buf,
    const struct aws_string *AWS_RESTRICT src) {
    AWS_PRECONDITION(!buf || aws_byte_buf_is_valid(buf));
    AWS_PRECONDITION(!src || aws_string_is_valid(src));
    if (buf == NULL || src == NULL) {
        return false;
    }
    return aws_byte_buf_write(buf, aws_string_bytes(src), src->len);
}

/**
 * Creates an aws_byte_cursor from an existing string.
 */
struct aws_byte_cursor aws_byte_cursor_from_string(const struct aws_string *src) {
    AWS_PRECONDITION(aws_string_is_valid(src));
    return aws_byte_cursor_from_array(aws_string_bytes(src), src->len);
}

struct aws_string *aws_string_clone_or_reuse(struct aws_allocator *allocator, const struct aws_string *str) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(aws_string_is_valid(str));

    if (str->allocator == NULL) {
        /* Since the string cannot be deallocated, we assume that it will remain valid for the lifetime of the
         * application */
        AWS_POSTCONDITION(aws_string_is_valid(str));
        return (struct aws_string *)str;
    }

    AWS_POSTCONDITION(aws_string_is_valid(str));
    return aws_string_new_from_string(allocator, str);
}

int aws_secure_strlen(const char *str, size_t max_read_len, size_t *str_len) {
    AWS_ERROR_PRECONDITION(str && str_len, AWS_ERROR_INVALID_ARGUMENT);

    /* why not strnlen? It doesn't work everywhere as it wasn't standardized til C11, and is considered
     * a GNU extension. This should be faster anyways. This should work for ascii and utf8.
     * Any other character sets in use deserve what they get. */
    char *null_char_ptr = memchr(str, '\0', max_read_len);

    if (null_char_ptr) {
        *str_len = null_char_ptr - str;
        return AWS_OP_SUCCESS;
    }

    return aws_raise_error(AWS_ERROR_C_STRING_BUFFER_NOT_NULL_TERMINATED);
}
