#include <wiredtiger_config.h>
#include <inttypes.h>
#include <stddef.h>

#if defined(__powerpc64__) && !defined(HAVE_NO_CRC32_HARDWARE)

unsigned int crc32_vpmsum(unsigned int crc, const unsigned char *p, unsigned long len);

/*
 * __checksum_with_seed_hw --
 *     WiredTiger: return a checksum for a chunk of memory when given a starting seed.
 */
static uint32_t
__checksum_with_seed_hw(uint32_t seed, const void *chunk, size_t len)
{
    return (crc32_vpmsum(seed, chunk, len));
}

/*
 * __checksum_hw --
 *     WiredTiger: return a checksum for a chunk of memory.
 */
static uint32_t
__checksum_hw(const void *chunk, size_t len)
{
    return (crc32_vpmsum(0, chunk, len));
}
#endif

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
#if defined(__powerpc64__) && !defined(HAVE_NO_CRC32_HARDWARE)
    return (__checksum_hw);
#else
    return (__wt_checksum_sw);
#endif
}

/*
 * wiredtiger_crc32c_with_seed_func --
 *     WiredTiger: detect CRC hardware and return the checksum function that accepts a starting
 *     seed.
 */
uint32_t (*wiredtiger_crc32c_with_seed_func(void))(uint32_t, const void *, size_t)
{
#if defined(__powerpc64__) && !defined(HAVE_NO_CRC32_HARDWARE)
    return (__checksum_with_seed_hw);
#else
    return (__wt_checksum_with_seed_sw);
#endif
}
