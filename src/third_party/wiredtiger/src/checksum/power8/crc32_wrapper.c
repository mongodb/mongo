#if defined(__powerpc64__)
#define CRC_TABLE
#include "crc32_constants.h"

#define VMX_ALIGN	16U
#define VMX_ALIGN_MASK	(VMX_ALIGN-1)

#ifdef REFLECT
static unsigned int crc32_align(unsigned int crc, const unsigned char *p,
			       unsigned long len)
{
	while (len--)
		crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return crc;
}
#else
static unsigned int crc32_align(unsigned int crc, const unsigned char *p,
				unsigned long len)
{
	while (len--)
		crc = crc_table[((crc >> 24) ^ *p++) & 0xff] ^ (crc << 8);
	return crc;
}
#endif

unsigned int __crc32_vpmsum(unsigned int crc, const unsigned char *p,
			    unsigned long len);

/* -Werror=missing-prototypes */
unsigned int crc32_vpmsum(unsigned int crc, const unsigned char *p,
			  unsigned long len);
unsigned int crc32_vpmsum(unsigned int crc, const unsigned char *p,
			  unsigned long len)
{
	unsigned int prealign;
	unsigned int tail;

#ifdef CRC_XOR
	crc ^= 0xffffffff;
#endif

	if (len < VMX_ALIGN + VMX_ALIGN_MASK) {
		crc = crc32_align(crc, p, len);
		goto out;
	}

	if ((unsigned long)p & VMX_ALIGN_MASK) {
		prealign = VMX_ALIGN - ((unsigned long)p & VMX_ALIGN_MASK);
		crc = crc32_align(crc, p, prealign);
		len -= prealign;
		p += prealign;
	}

	crc = __crc32_vpmsum(crc, p, len & ~VMX_ALIGN_MASK);

	tail = len & VMX_ALIGN_MASK;
	if (tail) {
		p += len & ~VMX_ALIGN_MASK;
		crc = crc32_align(crc, p, tail);
	}

out:
#ifdef CRC_XOR
	crc ^= 0xffffffff;
#endif

	return crc;
}
#endif

#include "wt_internal.h"

/*
 * __wt_cksum --
 *	WiredTiger: return a checksum for a chunk of memory.
 */
uint32_t
__wt_cksum(const void *chunk, size_t len)
{
	return crc32_vpmsum(0, chunk, len);
}

/*
 * __wt_cksum_init --
 *	WiredTiger: detect CRC hardware and set the checksum function.
 */
void
__wt_cksum_init(void)
{
	/* None needed. */
}
