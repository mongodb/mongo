/*-
 *  Copyright 2016 MongoDB, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  Local modifications to add __powerpc64__ conditional defines, and
 *  code formatting fixes.
 */
#if defined(__powerpc64__)
#define	CRC_TABLE
#include "crc32_constants.h"

#define	VMX_ALIGN	16
#define	VMX_ALIGN_MASK	(VMX_ALIGN-1)

#ifdef REFLECT
static unsigned int crc32_align(unsigned int crc, unsigned char *p,
			       unsigned long len)
{
	while (len--)
		crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return (crc);
}
#else
static unsigned int crc32_align(unsigned int crc, unsigned char *p,
				unsigned long len)
{
	while (len--)
		crc = crc_table[((crc >> 24) ^ *p++) & 0xff] ^ (crc << 8);
	return (crc);
}
#endif

unsigned int __crc32_vpmsum(unsigned int crc, unsigned char *p,
			    unsigned long len);

unsigned int crc32_vpmsum(unsigned int crc, unsigned char *p,
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

	return (crc);
}
#endif
