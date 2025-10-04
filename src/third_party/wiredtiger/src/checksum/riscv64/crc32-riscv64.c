/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <wiredtiger_config.h>
#include <inttypes.h>
#include <stddef.h>

extern uint32_t __wt_checksum_sw(const void *chunk, size_t len);
extern uint32_t __wt_checksum_with_seed_sw(uint32_t, const void *chunk, size_t len);

#if defined(__GNUC__)
extern uint32_t (*wiredtiger_crc32c_func(void))(const void *, size_t)
  __attribute__((visibility("default")));
extern uint32_t (*wiredtiger_crc32c_with_seed_func(void))(uint32_t, const void *, size_t)
  __attribute__((visibility("default")));
#else
extern uint32_t (*wiredtiger_crc32c_func(void))(const void *, size_t);
extern uint32_t (*wiredtiger_crc32c_with_seed_func(void))(uint32_t, const void *, size_t);
#endif

/*
 * wiredtiger_crc32c_func --
 *     WiredTiger: detect CRC hardware and return the checksum function.
 */
uint32_t (*wiredtiger_crc32c_func(void))(const void *, size_t)
{
    return (__wt_checksum_sw);
}

/*
 * wiredtiger_crc32c_with_seed_func --
 *     WiredTiger: detect CRC hardware and return the checksum function that accepts a starting
 *     seed.
 */
uint32_t (*wiredtiger_crc32c_with_seed_func(void))(uint32_t, const void *, size_t)
{
    return (__wt_checksum_with_seed_sw);
}
