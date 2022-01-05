#include <wiredtiger_config.h>
#include <inttypes.h>
#include <stddef.h>

#if defined(__powerpc64__) && !defined(HAVE_NO_CRC32_HARDWARE)

unsigned int crc32_vpmsum(unsigned int crc, const unsigned char *p, unsigned long len);

/*
 * __wt_checksum_hw --
 *     WiredTiger: return a checksum for a chunk of memory.
 */
static uint32_t
__wt_checksum_hw(const void *chunk, size_t len)
{
    return (crc32_vpmsum(0, chunk, len));
}
#endif

extern uint32_t __wt_checksum_sw(const void *chunk, size_t len);
#if defined(__GNUC__)
extern uint32_t (*wiredtiger_crc32c_func(void))(const void *, size_t)
  __attribute__((visibility("default")));
#else
extern uint32_t (*wiredtiger_crc32c_func(void))(const void *, size_t);
#endif

/*
 * wiredtiger_crc32c_func --
 *     WiredTiger: detect CRC hardware and return the checksum function.
 */
uint32_t (*wiredtiger_crc32c_func(void))(const void *, size_t)
{
#if defined(__powerpc64__) && !defined(HAVE_NO_CRC32_HARDWARE)
    return (__wt_checksum_hw);
#else
    return (__wt_checksum_sw);
#endif
}
