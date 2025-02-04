#ifndef AWS_COMMON_BYTE_BUF_H
#define AWS_COMMON_BYTE_BUF_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/array_list.h>
#include <aws/common/byte_order.h>
#include <aws/common/common.h>

#include <string.h>

AWS_PUSH_SANE_WARNING_LEVEL

/**
 * Represents a length-delimited binary string or buffer. If byte buffer points
 * to constant memory or memory that should otherwise not be freed by this
 * struct, set allocator to NULL and free function will be a no-op.
 *
 * This structure used to define the output for all functions that write to a buffer.
 *
 * Note that this structure allocates memory at the buffer pointer only. The
 * struct itself does not get dynamically allocated and must be either
 * maintained or copied to avoid losing access to the memory.
 */
struct aws_byte_buf {
    /* do not reorder this, this struct lines up nicely with windows buffer structures--saving us allocations.*/
    size_t len;
    uint8_t *buffer;
    size_t capacity;
    struct aws_allocator *allocator;
};

/**
 * Represents a movable pointer within a larger binary string or buffer.
 *
 * This structure is used to define buffers for reading.
 */
struct aws_byte_cursor {
    /* do not reorder this, this struct lines up nicely with windows buffer structures--saving us allocations */
    size_t len;
    uint8_t *ptr;
};

/**
 * Helper macro for passing aws_byte_cursor to the printf family of functions.
 * Intended for use with the PRInSTR format macro.
 * Ex: printf(PRInSTR "\n", AWS_BYTE_CURSOR_PRI(my_cursor));
 */
#define AWS_BYTE_CURSOR_PRI(C) ((int)(C).len < 0 ? 0 : (int)(C).len), (const char *)(C).ptr

/**
 * Helper macro for passing aws_byte_buf to the printf family of functions.
 * Intended for use with the PRInSTR format macro.
 * Ex: printf(PRInSTR "\n", AWS_BYTE_BUF_PRI(my_buf));
 */
#define AWS_BYTE_BUF_PRI(B) ((int)(B).len < 0 ? 0 : (int)(B).len), (const char *)(B).buffer

/**
 * Helper Macro for initializing a byte cursor from a string literal
 */
#define AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(literal)                                                                 \
    {.ptr = (uint8_t *)(const char *)(literal), .len = sizeof(literal) - 1}

/**
 * Signature for function argument to trim APIs
 */
typedef bool(aws_byte_predicate_fn)(uint8_t value);

AWS_EXTERN_C_BEGIN

/**
 * Compare two arrays.
 * Return whether their contents are equivalent.
 * NULL may be passed as the array pointer if its length is declared to be 0.
 */
AWS_COMMON_API
bool aws_array_eq(const void *const array_a, const size_t len_a, const void *array_b, const size_t len_b);

/**
 * Perform a case-insensitive string comparison of two arrays.
 * Return whether their contents are equivalent.
 * NULL may be passed as the array pointer if its length is declared to be 0.
 * The "C" locale is used for comparing upper and lowercase letters.
 * Data is assumed to be ASCII text, UTF-8 will work fine too.
 */
AWS_COMMON_API
bool aws_array_eq_ignore_case(
    const void *const array_a,
    const size_t len_a,
    const void *const array_b,
    const size_t len_b);

/**
 * Compare an array and a null-terminated string.
 * Returns true if their contents are equivalent.
 * The array should NOT contain a null-terminator, or the comparison will always return false.
 * NULL may be passed as the array pointer if its length is declared to be 0.
 */
AWS_COMMON_API
bool aws_array_eq_c_str(const void *const array, const size_t array_len, const char *const c_str);

/**
 * Perform a case-insensitive string comparison of an array and a null-terminated string.
 * Return whether their contents are equivalent.
 * The array should NOT contain a null-terminator, or the comparison will always return false.
 * NULL may be passed as the array pointer if its length is declared to be 0.
 * The "C" locale is used for comparing upper and lowercase letters.
 * Data is assumed to be ASCII text, UTF-8 will work fine too.
 */
AWS_COMMON_API
bool aws_array_eq_c_str_ignore_case(const void *const array, const size_t array_len, const char *const c_str);

AWS_COMMON_API
int aws_byte_buf_init(struct aws_byte_buf *buf, struct aws_allocator *allocator, size_t capacity);

/**
 * Initializes an aws_byte_buf structure base on another valid one.
 * Requires: *src and *allocator are valid objects.
 * Ensures: *dest is a valid aws_byte_buf with a new backing array dest->buffer
 * which is a copy of the elements from src->buffer.
 */
AWS_COMMON_API int aws_byte_buf_init_copy(
    struct aws_byte_buf *dest,
    struct aws_allocator *allocator,
    const struct aws_byte_buf *src);

/**
 * Reads 'filename' into 'out_buf'. If successful, 'out_buf' is allocated and filled with the data;
 * It is your responsibility to call 'aws_byte_buf_clean_up()' on it. Otherwise, 'out_buf' remains
 * unused. In the very unfortunate case where some API needs to treat out_buf as a c_string, a null terminator
 * is appended, but is not included as part of the length field.
 */
AWS_COMMON_API
int aws_byte_buf_init_from_file(struct aws_byte_buf *out_buf, struct aws_allocator *alloc, const char *filename);

/**
 * Same as aws_byte_buf_init_from_file(), but for reading "special files" like /proc/cpuinfo.
 * These files don't accurately report their size, so size_hint is used as initial buffer size,
 * and the buffer grows until the while file is read.
 */
AWS_COMMON_API
int aws_byte_buf_init_from_file_with_size_hint(
    struct aws_byte_buf *out_buf,
    struct aws_allocator *alloc,
    const char *filename,
    size_t size_hint);

/**
 * Evaluates the set of properties that define the shape of all valid aws_byte_buf structures.
 * It is also a cheap check, in the sense it run in constant time (i.e., no loops or recursion).
 */
AWS_COMMON_API
bool aws_byte_buf_is_valid(const struct aws_byte_buf *const buf);

/**
 * Evaluates the set of properties that define the shape of all valid aws_byte_cursor structures.
 * It is also a cheap check, in the sense it runs in constant time (i.e., no loops or recursion).
 */
AWS_COMMON_API
bool aws_byte_cursor_is_valid(const struct aws_byte_cursor *cursor);

/**
 * Copies src buffer into dest and sets the correct len and capacity.
 * A new memory zone is allocated for dest->buffer. When dest is no longer needed it will have to be cleaned-up using
 * aws_byte_buf_clean_up(dest).
 * Dest capacity and len will be equal to the src len. Allocator of the dest will be identical with parameter allocator.
 * If src buffer is null the dest will have a null buffer with a len and a capacity of 0
 * Returns AWS_OP_SUCCESS in case of success or AWS_OP_ERR when memory can't be allocated.
 */
AWS_COMMON_API
int aws_byte_buf_init_copy_from_cursor(
    struct aws_byte_buf *dest,
    struct aws_allocator *allocator,
    struct aws_byte_cursor src);

/**
 * Init buffer with contents of multiple cursors, and update cursors to reference the memory stored in the buffer.
 * Each cursor arg must be an `struct aws_byte_cursor *`. NULL must be passed as the final arg.
 * NOTE: Do not append/grow/resize buffers initialized this way, or the cursors will end up referencing invalid memory.
 * Returns AWS_OP_SUCCESS in case of success.
 * AWS_OP_ERR is returned if memory can't be allocated or the total cursor length exceeds SIZE_MAX.
 */
AWS_COMMON_API
int aws_byte_buf_init_cache_and_update_cursors(struct aws_byte_buf *dest, struct aws_allocator *allocator, ...);

AWS_COMMON_API
void aws_byte_buf_clean_up(struct aws_byte_buf *buf);

/**
 * Equivalent to calling aws_byte_buf_secure_zero and then aws_byte_buf_clean_up
 * on the buffer.
 */
AWS_COMMON_API
void aws_byte_buf_clean_up_secure(struct aws_byte_buf *buf);

/**
 * Resets the len of the buffer to 0, but does not free the memory. The buffer can then be reused.
 * Optionally zeroes the contents, if the "zero_contents" flag is true.
 */
AWS_COMMON_API
void aws_byte_buf_reset(struct aws_byte_buf *buf, bool zero_contents);

/**
 * Sets all bytes of buffer to zero and resets len to zero.
 */
AWS_COMMON_API
void aws_byte_buf_secure_zero(struct aws_byte_buf *buf);

/**
 * Compare two aws_byte_buf structures.
 * Return whether their contents are equivalent.
 */
AWS_COMMON_API
bool aws_byte_buf_eq(const struct aws_byte_buf *const a, const struct aws_byte_buf *const b);

/**
 * Perform a case-insensitive string comparison of two aws_byte_buf structures.
 * Return whether their contents are equivalent.
 * The "C" locale is used for comparing upper and lowercase letters.
 * Data is assumed to be ASCII text, UTF-8 will work fine too.
 */
AWS_COMMON_API
bool aws_byte_buf_eq_ignore_case(const struct aws_byte_buf *const a, const struct aws_byte_buf *const b);

/**
 * Compare an aws_byte_buf and a null-terminated string.
 * Returns true if their contents are equivalent.
 * The buffer should NOT contain a null-terminator, or the comparison will always return false.
 */
AWS_COMMON_API
bool aws_byte_buf_eq_c_str(const struct aws_byte_buf *const buf, const char *const c_str);

/**
 * Perform a case-insensitive string comparison of an aws_byte_buf and a null-terminated string.
 * Return whether their contents are equivalent.
 * The buffer should NOT contain a null-terminator, or the comparison will always return false.
 * The "C" locale is used for comparing upper and lowercase letters.
 * Data is assumed to be ASCII text, UTF-8 will work fine too.
 */
AWS_COMMON_API
bool aws_byte_buf_eq_c_str_ignore_case(const struct aws_byte_buf *const buf, const char *const c_str);

/**
 * No copies, no buffer allocations. Iterates over input_str, and returns the
 * next substring between split_on instances relative to previous substr.
 * Behaves similar to strtok with substr being used as state for next split.
 *
 * Returns true each time substr is set and false when there is no more splits
 * (substr is set to empty in that case).
 *
 * Example usage.
 * struct aws_byte_cursor substr = {0};
 * while (aws_byte_cursor_next_split(&input_str, ';', &substr)) {
 *   // ...use substr...
 * }
 *
 * Note: It is the user's responsibility zero-initialize substr before the first call.
 *
 * Edge case rules are as follows:
 * empty input will have single empty split. ex. "" splits into ""
 * if input starts with split_on then first split is empty. ex ";A" splits into "", "A"
 * adjacent split tokens result in empty split. ex "A;;B" splits into "A", "", "B"
 * If the input ends with split_on, last split is empty. ex. "A;" splits into "A", ""
 *
 * It is the user's responsibility to make sure the input buffer stays in memory
 * long enough to use the results.
 */
AWS_COMMON_API
bool aws_byte_cursor_next_split(
    const struct aws_byte_cursor *AWS_RESTRICT input_str,
    char split_on,
    struct aws_byte_cursor *AWS_RESTRICT substr);

/**
 * No copies, no buffer allocations. Fills in output with a list of
 * aws_byte_cursor instances where buffer is an offset into the input_str and
 * len is the length of that string in the original buffer.
 *
 * Edge case rules are as follows:
 * if the input begins with split_on, an empty cursor will be the first entry in
 * output. if the input has two adjacent split_on tokens, an empty cursor will
 * be inserted into the output. if the input ends with split_on, an empty cursor
 * will be appended to the output.
 *
 * It is the user's responsibility to properly initialize output. Recommended number of preallocated elements from
 * output is your most likely guess for the upper bound of the number of elements resulting from the split.
 *
 * The type that will be stored in output is struct aws_byte_cursor (you'll need
 * this for the item size param).
 *
 * It is the user's responsibility to make sure the input buffer stays in memory
 * long enough to use the results.
 */
AWS_COMMON_API
int aws_byte_cursor_split_on_char(
    const struct aws_byte_cursor *AWS_RESTRICT input_str,
    char split_on,
    struct aws_array_list *AWS_RESTRICT output);

/**
 * No copies, no buffer allocations. Fills in output with a list of aws_byte_cursor instances where buffer is
 * an offset into the input_str and len is the length of that string in the original buffer. N is the max number of
 * splits, if this value is zero, it will add all splits to the output.
 *
 * Edge case rules are as follows:
 * if the input begins with split_on, an empty cursor will be the first entry in output
 * if the input has two adjacent split_on tokens, an empty cursor will be inserted into the output.
 * if the input ends with split_on, an empty cursor will be appended to the output.
 *
 * It is the user's responsibility to properly initialize output. Recommended number of preallocated elements from
 * output is your most likely guess for the upper bound of the number of elements resulting from the split.
 *
 * If the output array is not large enough, input_str will be updated to point to the first character after the last
 * processed split_on instance.
 *
 * The type that will be stored in output is struct aws_byte_cursor (you'll need this for the item size param).
 *
 * It is the user's responsibility to make sure the input buffer stays in memory long enough to use the results.
 */
AWS_COMMON_API
int aws_byte_cursor_split_on_char_n(
    const struct aws_byte_cursor *AWS_RESTRICT input_str,
    char split_on,
    size_t n,
    struct aws_array_list *AWS_RESTRICT output);

/**
 * Search for an exact byte match inside a cursor. The first match will be returned. Returns AWS_OP_SUCCESS
 * on successful match and first_find will be set to the offset in input_str, and length will be the remaining length
 * from input_str past the returned offset. If the match was not found, AWS_OP_ERR will be returned and
 * AWS_ERROR_STRING_MATCH_NOT_FOUND will be raised.
 */
AWS_COMMON_API
int aws_byte_cursor_find_exact(
    const struct aws_byte_cursor *AWS_RESTRICT input_str,
    const struct aws_byte_cursor *AWS_RESTRICT to_find,
    struct aws_byte_cursor *first_find);

/**
 *
 * Shrinks a byte cursor from the right for as long as the supplied predicate is true
 */
AWS_COMMON_API
struct aws_byte_cursor aws_byte_cursor_right_trim_pred(
    const struct aws_byte_cursor *source,
    aws_byte_predicate_fn *predicate);

/**
 * Shrinks a byte cursor from the left for as long as the supplied predicate is true
 */
AWS_COMMON_API
struct aws_byte_cursor aws_byte_cursor_left_trim_pred(
    const struct aws_byte_cursor *source,
    aws_byte_predicate_fn *predicate);

/**
 * Shrinks a byte cursor from both sides for as long as the supplied predicate is true
 */
AWS_COMMON_API
struct aws_byte_cursor aws_byte_cursor_trim_pred(
    const struct aws_byte_cursor *source,
    aws_byte_predicate_fn *predicate);

/**
 * Returns true if the byte cursor's range of bytes all satisfy the predicate
 */
AWS_COMMON_API
bool aws_byte_cursor_satisfies_pred(const struct aws_byte_cursor *source, aws_byte_predicate_fn *predicate);

/**
 * Copies from to to. If to is too small, AWS_ERROR_DEST_COPY_TOO_SMALL will be
 * returned. dest->len will contain the amount of data actually copied to dest.
 *
 * from and to may be the same buffer, permitting copying a buffer into itself.
 */
AWS_COMMON_API
int aws_byte_buf_append(struct aws_byte_buf *to, const struct aws_byte_cursor *from);

/**
 * Copies from to to while converting bytes via the passed in lookup table.
 * If to is too small, AWS_ERROR_DEST_COPY_TOO_SMALL will be
 * returned. to->len will contain its original size plus the amount of data actually copied to to.
 *
 * from and to should not be the same buffer (overlap is not handled)
 * lookup_table must be at least 256 bytes
 */
AWS_COMMON_API
int aws_byte_buf_append_with_lookup(
    struct aws_byte_buf *AWS_RESTRICT to,
    const struct aws_byte_cursor *AWS_RESTRICT from,
    const uint8_t *lookup_table);

/**
 * Copies from to to. If to is too small, the buffer will be grown appropriately and
 * the old contents copied to, before the new contents are appended.
 *
 * If the grow fails (overflow or OOM), then an error will be returned.
 *
 * from and to may be the same buffer, permitting copying a buffer into itself.
 */
AWS_COMMON_API
int aws_byte_buf_append_dynamic(struct aws_byte_buf *to, const struct aws_byte_cursor *from);

/**
 * Copies `from` to `to`. If `to` is too small, the buffer will be grown appropriately and
 * the old contents copied over, before the new contents are appended.
 *
 * If the grow fails (overflow or OOM), then an error will be returned.
 *
 * If the buffer is grown, the old buffer will be securely cleared before getting freed.
 *
 * `from` and `to` may be the same buffer, permitting copying a buffer into itself.
 */
AWS_COMMON_API
int aws_byte_buf_append_dynamic_secure(struct aws_byte_buf *to, const struct aws_byte_cursor *from);

/**
 * Copies a single byte into `to`. If `to` is too small, the buffer will be grown appropriately and
 * the old contents copied over, before the byte is appended.
 *
 * If the grow fails (overflow or OOM), then an error will be returned.
 */
AWS_COMMON_API
int aws_byte_buf_append_byte_dynamic(struct aws_byte_buf *buffer, uint8_t value);

/**
 * Copies a single byte into `to`. If `to` is too small, the buffer will be grown appropriately and
 * the old contents copied over, before the byte is appended.
 *
 * If the grow fails (overflow or OOM), then an error will be returned.
 *
 * If the buffer is grown, the old buffer will be securely cleared before getting freed.
 */
AWS_COMMON_API
int aws_byte_buf_append_byte_dynamic_secure(struct aws_byte_buf *buffer, uint8_t value);

/**
 * Copy contents of cursor to buffer, then update cursor to reference the memory stored in the buffer.
 * If buffer is too small, AWS_ERROR_DEST_COPY_TOO_SMALL will be returned.
 *
 * The cursor is permitted to reference memory from earlier in the buffer.
 */
AWS_COMMON_API
int aws_byte_buf_append_and_update(struct aws_byte_buf *to, struct aws_byte_cursor *from_and_update);

/**
 * Appends '\0' at the end of the buffer.
 */
AWS_COMMON_API
int aws_byte_buf_append_null_terminator(struct aws_byte_buf *buf);

/**
 * Attempts to increase the capacity of a buffer to the requested capacity
 *
 * If the the buffer's capacity is currently larger than the request capacity, the
 * function does nothing (no shrink is performed).
 */
AWS_COMMON_API
int aws_byte_buf_reserve(struct aws_byte_buf *buffer, size_t requested_capacity);

/**
 * Convenience function that attempts to increase the capacity of a buffer relative to the current
 * length.
 *
 *  aws_byte_buf_reserve_relative(buf, x) ~~ aws_byte_buf_reserve(buf, buf->len + x)
 *
 */
AWS_COMMON_API
int aws_byte_buf_reserve_relative(struct aws_byte_buf *buffer, size_t additional_length);

/**
 * Concatenates a variable number of struct aws_byte_buf * into destination.
 * Number of args must be greater than 1. If dest is too small,
 * AWS_ERROR_DEST_COPY_TOO_SMALL will be returned. dest->len will contain the
 * amount of data actually copied to dest.
 */
AWS_COMMON_API
int aws_byte_buf_cat(struct aws_byte_buf *dest, size_t number_of_args, ...);

/**
 * Compare two aws_byte_cursor structures.
 * Return whether their contents are equivalent.
 */
AWS_COMMON_API
bool aws_byte_cursor_eq(const struct aws_byte_cursor *a, const struct aws_byte_cursor *b);

/**
 * Perform a case-insensitive string comparison of two aws_byte_cursor structures.
 * Return whether their contents are equivalent.
 * The "C" locale is used for comparing upper and lowercase letters.
 * Data is assumed to be ASCII text, UTF-8 will work fine too.
 */
AWS_COMMON_API
bool aws_byte_cursor_eq_ignore_case(const struct aws_byte_cursor *a, const struct aws_byte_cursor *b);

/**
 * Compare an aws_byte_cursor and an aws_byte_buf.
 * Return whether their contents are equivalent.
 */
AWS_COMMON_API
bool aws_byte_cursor_eq_byte_buf(const struct aws_byte_cursor *const a, const struct aws_byte_buf *const b);

/**
 * Perform a case-insensitive string comparison of an aws_byte_cursor and an aws_byte_buf.
 * Return whether their contents are equivalent.
 * The "C" locale is used for comparing upper and lowercase letters.
 * Data is assumed to be ASCII text, UTF-8 will work fine too.
 */
AWS_COMMON_API
bool aws_byte_cursor_eq_byte_buf_ignore_case(const struct aws_byte_cursor *const a, const struct aws_byte_buf *const b);

/**
 * Compare an aws_byte_cursor and a null-terminated string.
 * Returns true if their contents are equivalent.
 * The cursor should NOT contain a null-terminator, or the comparison will always return false.
 */
AWS_COMMON_API
bool aws_byte_cursor_eq_c_str(const struct aws_byte_cursor *const cursor, const char *const c_str);

/**
 * Perform a case-insensitive string comparison of an aws_byte_cursor and a null-terminated string.
 * Return whether their contents are equivalent.
 * The cursor should NOT contain a null-terminator, or the comparison will always return false.
 * The "C" locale is used for comparing upper and lowercase letters.
 * Data is assumed to be ASCII text, UTF-8 will work fine too.
 */
AWS_COMMON_API
bool aws_byte_cursor_eq_c_str_ignore_case(const struct aws_byte_cursor *const cursor, const char *const c_str);

/**
 * Return true if the input starts with the prefix (exact byte comparison).
 */
AWS_COMMON_API
bool aws_byte_cursor_starts_with(const struct aws_byte_cursor *input, const struct aws_byte_cursor *prefix);

/**
 * Return true if the input starts with the prefix (case-insensitive).
 * The "C" locale is used for comparing upper and lowercase letters.
 * Data is assumed to be ASCII text, UTF-8 will work fine too.
 */
AWS_COMMON_API
bool aws_byte_cursor_starts_with_ignore_case(const struct aws_byte_cursor *input, const struct aws_byte_cursor *prefix);

/**
 * Case-insensitive hash function for array containing ASCII or UTF-8 text.
 */
AWS_COMMON_API
uint64_t aws_hash_array_ignore_case(const void *array, const size_t len);

/**
 * Case-insensitive hash function for aws_byte_cursors stored in an aws_hash_table.
 * For case-sensitive hashing, use aws_hash_byte_cursor_ptr().
 */
AWS_COMMON_API
uint64_t aws_hash_byte_cursor_ptr_ignore_case(const void *item);

/**
 * Returns a lookup table for bytes that is the identity transformation with the exception
 * of uppercase ascii characters getting replaced with lowercase characters.  Used in
 * caseless comparisons.
 */
AWS_COMMON_API
const uint8_t *aws_lookup_table_to_lower_get(void);

/**
 * Returns lookup table to go from ASCII/UTF-8 hex character to a number (0-15).
 * Non-hex characters map to 255.
 * Valid examples:
 * '0' -> 0
 * 'F' -> 15
 * 'f' -> 15
 * Invalid examples:
 * ' ' -> 255
 * 'Z' -> 255
 * '\0' -> 255
 */
AWS_COMMON_API
const uint8_t *aws_lookup_table_hex_to_num_get(void);

/**
 * Lexical (byte value) comparison of two byte cursors
 */
AWS_COMMON_API
int aws_byte_cursor_compare_lexical(const struct aws_byte_cursor *lhs, const struct aws_byte_cursor *rhs);

/**
 * Lexical (byte value) comparison of two byte cursors where the raw values are sent through a lookup table first
 */
AWS_COMMON_API
int aws_byte_cursor_compare_lookup(
    const struct aws_byte_cursor *lhs,
    const struct aws_byte_cursor *rhs,
    const uint8_t *lookup_table);

/**
 * For creating a byte buffer from a null-terminated string literal.
 */
AWS_COMMON_API struct aws_byte_buf aws_byte_buf_from_c_str(const char *c_str);

AWS_COMMON_API struct aws_byte_buf aws_byte_buf_from_array(const void *bytes, size_t len);

AWS_COMMON_API struct aws_byte_buf aws_byte_buf_from_empty_array(const void *bytes, size_t capacity);

AWS_COMMON_API struct aws_byte_cursor aws_byte_cursor_from_buf(const struct aws_byte_buf *const buf);

AWS_COMMON_API struct aws_byte_cursor aws_byte_cursor_from_c_str(const char *c_str);

AWS_COMMON_API struct aws_byte_cursor aws_byte_cursor_from_array(const void *const bytes, const size_t len);

/**
 * Tests if the given aws_byte_cursor has at least len bytes remaining. If so,
 * *buf is advanced by len bytes (incrementing ->ptr and decrementing ->len),
 * and an aws_byte_cursor referring to the first len bytes of the original *buf
 * is returned. Otherwise, an aws_byte_cursor with ->ptr = NULL, ->len = 0 is
 * returned.
 *
 * Note that if len is above (SIZE_MAX / 2), this function will also treat it as
 * a buffer overflow, and return NULL without changing *buf.
 */
AWS_COMMON_API struct aws_byte_cursor aws_byte_cursor_advance(struct aws_byte_cursor *const cursor, const size_t len);

/**
 * Behaves identically to aws_byte_cursor_advance, but avoids speculative
 * execution potentially reading out-of-bounds pointers (by returning an
 * empty ptr in such speculated paths).
 *
 * This should generally be done when using an untrusted or
 * data-dependent value for 'len', to avoid speculating into a path where
 * cursor->ptr points outside the true ptr length.
 */

AWS_COMMON_API struct aws_byte_cursor aws_byte_cursor_advance_nospec(struct aws_byte_cursor *const cursor, size_t len);

/**
 * Reads specified length of data from byte cursor and copies it to the
 * destination array.
 *
 * On success, returns true and updates the cursor pointer/length accordingly.
 * If there is insufficient space in the cursor, returns false, leaving the
 * cursor unchanged.
 */
AWS_COMMON_API bool aws_byte_cursor_read(
    struct aws_byte_cursor *AWS_RESTRICT cur,
    void *AWS_RESTRICT dest,
    const size_t len);

/**
 * Reads as many bytes from cursor as size of buffer, and copies them to buffer.
 *
 * On success, returns true and updates the cursor pointer/length accordingly.
 * If there is insufficient space in the cursor, returns false, leaving the
 * cursor unchanged.
 */
AWS_COMMON_API bool aws_byte_cursor_read_and_fill_buffer(
    struct aws_byte_cursor *AWS_RESTRICT cur,
    struct aws_byte_buf *AWS_RESTRICT dest);

/**
 * Reads a single byte from cursor, placing it in *var.
 *
 * On success, returns true and updates the cursor pointer/length accordingly.
 * If there is insufficient space in the cursor, returns false, leaving the
 * cursor unchanged.
 */
AWS_COMMON_API bool aws_byte_cursor_read_u8(struct aws_byte_cursor *AWS_RESTRICT cur, uint8_t *AWS_RESTRICT var);

/**
 * Reads a 16-bit value in network byte order from cur, and places it in host
 * byte order into var.
 *
 * On success, returns true and updates the cursor pointer/length accordingly.
 * If there is insufficient space in the cursor, returns false, leaving the
 * cursor unchanged.
 */
AWS_COMMON_API bool aws_byte_cursor_read_be16(struct aws_byte_cursor *cur, uint16_t *var);

/**
 * Reads an unsigned 24-bit value (3 bytes) in network byte order from cur,
 * and places it in host byte order into 32-bit var.
 * Ex: if cur's next 3 bytes are {0xAA, 0xBB, 0xCC}, then var becomes 0x00AABBCC.
 *
 * On success, returns true and updates the cursor pointer/length accordingly.
 * If there is insufficient space in the cursor, returns false, leaving the
 * cursor unchanged.
 */
AWS_COMMON_API bool aws_byte_cursor_read_be24(struct aws_byte_cursor *cur, uint32_t *var);

/**
 * Reads a 32-bit value in network byte order from cur, and places it in host
 * byte order into var.
 *
 * On success, returns true and updates the cursor pointer/length accordingly.
 * If there is insufficient space in the cursor, returns false, leaving the
 * cursor unchanged.
 */
AWS_COMMON_API bool aws_byte_cursor_read_be32(struct aws_byte_cursor *cur, uint32_t *var);

/**
 * Reads a 64-bit value in network byte order from cur, and places it in host
 * byte order into var.
 *
 * On success, returns true and updates the cursor pointer/length accordingly.
 * If there is insufficient space in the cursor, returns false, leaving the
 * cursor unchanged.
 */
AWS_COMMON_API bool aws_byte_cursor_read_be64(struct aws_byte_cursor *cur, uint64_t *var);

/**
 * Reads a 32-bit value in network byte order from cur, and places it in host
 * byte order into var.
 *
 * On success, returns true and updates the cursor pointer/length accordingly.
 * If there is insufficient space in the cursor, returns false, leaving the
 * cursor unchanged.
 */
AWS_COMMON_API bool aws_byte_cursor_read_float_be32(struct aws_byte_cursor *cur, float *var);

/**
 * Reads a 64-bit value in network byte order from cur, and places it in host
 * byte order into var.
 *
 * On success, returns true and updates the cursor pointer/length accordingly.
 * If there is insufficient space in the cursor, returns false, leaving the
 * cursor unchanged.
 */
AWS_COMMON_API bool aws_byte_cursor_read_float_be64(struct aws_byte_cursor *cur, double *var);

/**
 * Reads 2 hex characters from ASCII/UTF-8 text to produce an 8-bit number.
 * Accepts both lowercase 'a'-'f' and uppercase 'A'-'F'.
 * For example: "0F" produces 15.
 *
 * On success, returns true and advances the cursor by 2.
 * If there is insufficient space in the cursor or an invalid character
 * is encountered, returns false, leaving the cursor unchanged.
 */
AWS_COMMON_API bool aws_byte_cursor_read_hex_u8(struct aws_byte_cursor *cur, uint8_t *var);

/**
 * Appends a sub-buffer to the specified buffer.
 *
 * If the buffer has at least `len' bytes remaining (buffer->capacity - buffer->len >= len),
 * then buffer->len is incremented by len, and an aws_byte_buf is assigned to *output corresponding
 * to the last len bytes of the input buffer. The aws_byte_buf at *output will have a null
 * allocator, a zero initial length, and a capacity of 'len'. The function then returns true.
 *
 * If there is insufficient space, then this function nulls all fields in *output and returns
 * false.
 */
AWS_COMMON_API bool aws_byte_buf_advance(
    struct aws_byte_buf *const AWS_RESTRICT buffer,
    struct aws_byte_buf *const AWS_RESTRICT output,
    const size_t len);

/**
 * Write specified number of bytes from array to byte buffer.
 *
 * On success, returns true and updates the buffer length accordingly.
 * If there is insufficient space in the buffer, returns false, leaving the
 * buffer unchanged.
 */
AWS_COMMON_API bool aws_byte_buf_write(
    struct aws_byte_buf *AWS_RESTRICT buf,
    const uint8_t *AWS_RESTRICT src,
    size_t len);

/**
 * Copies all bytes from buffer to buffer.
 *
 * On success, returns true and updates the buffer /length accordingly.
 * If there is insufficient space in the buffer, returns false, leaving the
 * buffer unchanged.
 */
AWS_COMMON_API bool aws_byte_buf_write_from_whole_buffer(
    struct aws_byte_buf *AWS_RESTRICT buf,
    struct aws_byte_buf src);

/**
 * Copies all bytes from buffer to buffer.
 *
 * On success, returns true and updates the buffer /length accordingly.
 * If there is insufficient space in the buffer, returns false, leaving the
 * buffer unchanged.
 */
AWS_COMMON_API bool aws_byte_buf_write_from_whole_cursor(
    struct aws_byte_buf *AWS_RESTRICT buf,
    struct aws_byte_cursor src);

/**
 * Without increasing buf's capacity, write as much as possible from advancing_cursor into buf.
 *
 * buf's len is updated accordingly.
 * advancing_cursor is advanced so it contains the remaining unwritten parts.
 * Returns the section of advancing_cursor which was written.
 *
 * This function cannot fail. If buf is full (len == capacity) or advancing_len has 0 length,
 * then buf and advancing_cursor are not altered and a cursor with 0 length is returned.
 *
 * Example: Given a buf with 2 bytes of space available and advancing_cursor with contents "abc".
 * "ab" will be written to buf and buf->len will increase 2 and become equal to buf->capacity.
 * advancing_cursor will advance so its contents become the unwritten "c".
 * The returned cursor's contents will be the "ab" from the original advancing_cursor.
 */
AWS_COMMON_API struct aws_byte_cursor aws_byte_buf_write_to_capacity(
    struct aws_byte_buf *buf,
    struct aws_byte_cursor *advancing_cursor);

/**
 * Copies one byte to buffer.
 *
 * On success, returns true and updates the cursor /length
 accordingly.
 *
 * If there is insufficient space in the buffer, returns false, leaving the
 * buffer unchanged.
 */
AWS_COMMON_API bool aws_byte_buf_write_u8(struct aws_byte_buf *AWS_RESTRICT buf, uint8_t c);

/**
 * Writes one byte repeatedly to buffer (like memset)
 *
 * If there is insufficient space in the buffer, returns false, leaving the
 * buffer unchanged.
 */
AWS_COMMON_API bool aws_byte_buf_write_u8_n(struct aws_byte_buf *buf, uint8_t c, size_t count);

/**
 * Writes a 16-bit integer in network byte order (big endian) to buffer.
 *
 * On success, returns true and updates the buffer /length accordingly.
 * If there is insufficient space in the buffer, returns false, leaving the
 * buffer unchanged.
 */
AWS_COMMON_API bool aws_byte_buf_write_be16(struct aws_byte_buf *buf, uint16_t x);

/**
 * Writes low 24-bits (3 bytes) of an unsigned integer in network byte order (big endian) to buffer.
 * Ex: If x is 0x00AABBCC then {0xAA, 0xBB, 0xCC} is written to buffer.
 *
 * On success, returns true and updates the buffer /length accordingly.
 * If there is insufficient space in the buffer, or x's value cannot fit in 3 bytes,
 * returns false, leaving the buffer unchanged.
 */
AWS_COMMON_API bool aws_byte_buf_write_be24(struct aws_byte_buf *buf, uint32_t x);

/**
 * Writes a 32-bit integer in network byte order (big endian) to buffer.
 *
 * On success, returns true and updates the buffer /length accordingly.
 * If there is insufficient space in the buffer, returns false, leaving the
 * buffer unchanged.
 */
AWS_COMMON_API bool aws_byte_buf_write_be32(struct aws_byte_buf *buf, uint32_t x);

/**
 * Writes a 32-bit float in network byte order (big endian) to buffer.
 *
 * On success, returns true and updates the buffer /length accordingly.
 * If there is insufficient space in the buffer, returns false, leaving the
 * buffer unchanged.
 */
AWS_COMMON_API bool aws_byte_buf_write_float_be32(struct aws_byte_buf *buf, float x);

/**
 * Writes a 64-bit integer in network byte order (big endian) to buffer.
 *
 * On success, returns true and updates the buffer /length accordingly.
 * If there is insufficient space in the buffer, returns false, leaving the
 * buffer unchanged.
 */
AWS_COMMON_API bool aws_byte_buf_write_be64(struct aws_byte_buf *buf, uint64_t x);

/**
 * Writes a 64-bit float in network byte order (big endian) to buffer.
 *
 * On success, returns true and updates the buffer /length accordingly.
 * If there is insufficient space in the buffer, returns false, leaving the
 * buffer unchanged.
 */
AWS_COMMON_API bool aws_byte_buf_write_float_be64(struct aws_byte_buf *buf, double x);

/**
 * Like isalnum(), but ignores C locale.
 * Returns true if ch has the value of ASCII/UTF-8: 'a'-'z', 'A'-'Z', or '0'-'9'.
 */
AWS_COMMON_API bool aws_isalnum(uint8_t ch);

/**
 * Like isalpha(), but ignores C locale.
 * Returns true if ch has the value of ASCII/UTF-8: 'a'-'z' or 'A'-'Z'.
 */
AWS_COMMON_API bool aws_isalpha(uint8_t ch);

/**
 * Like isdigit().
 * Returns true if ch has the value of ASCII/UTF-8: '0'-'9'.
 *
 * Note: C's built-in isdigit() is also supposed to ignore the C locale,
 * but cppreference.com claims "some implementations (e.g. Microsoft in 1252 codepage)
 * may classify additional single-byte characters as digits"
 */
AWS_COMMON_API bool aws_isdigit(uint8_t ch);

/**
 * Like isxdigit().
 * Returns true if ch has the value of ASCII/UTF-8: '0'-'9', 'a'-'f', or 'A'-'F'.
 *
 * Note: C's built-in isxdigit() is also supposed to ignore the C locale,
 * but cppreference.com claims "some implementations (e.g. Microsoft in 1252 codepage)
 * may classify additional single-byte characters as digits"
 */

AWS_COMMON_API bool aws_isxdigit(uint8_t ch);

/**
 * Like isspace(), but ignores C locale.
 * Return true if ch has the value of ASCII/UTF-8: space (0x20), form feed (0x0C),
 * line feed (0x0A), carriage return (0x0D), horizontal tab (0x09), or vertical tab (0x0B).
 */
AWS_COMMON_API bool aws_isspace(uint8_t ch);

/**
 * Read entire cursor as ASCII/UTF-8 unsigned base-10 number.
 * Stricter than strtoull(), which allows whitespace and inputs that start with "0x"
 *
 * Examples:
 * "0" -> 0
 * "123" -> 123
 * "00004" -> 4 // leading zeros ok
 *
 * Rejects things like:
 * "-1" // negative numbers not allowed
 * "1,000" // only characters 0-9 allowed
 * "" // blank string not allowed
 * " 0 " // whitespace not allowed
 * "0x0" // hex not allowed
 * "FF" // hex not allowed
 * "999999999999999999999999999999999999999999" // larger than max u64
 */
AWS_COMMON_API
int aws_byte_cursor_utf8_parse_u64(struct aws_byte_cursor cursor, uint64_t *dst);

/**
 * Read entire cursor as ASCII/UTF-8 unsigned base-16 number with NO "0x" prefix.
 *
 * Examples:
 * "F" -> 15
 * "000000ff" -> 255 // leading zeros ok
 * "Ff" -> 255 // mixed case ok
 * "123" -> 291
 * "FFFFFFFFFFFFFFFF" -> 18446744073709551616 // max u64
 *
 * Rejects things like:
 * "0x0" // 0x prefix not allowed
 * "" // blank string not allowed
 * " F " // whitespace not allowed
 * "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF" // larger than max u64
 */
AWS_COMMON_API
int aws_byte_cursor_utf8_parse_u64_hex(struct aws_byte_cursor cursor, uint64_t *dst);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_BYTE_BUF_H */
