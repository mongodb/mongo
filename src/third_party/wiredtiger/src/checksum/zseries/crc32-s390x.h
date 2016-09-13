/*
 * Copyright IBM Corp. 2015
 */
#include <sys/types.h>

/* Portable implementations of CRC-32 (IEEE and Castagnoli), both
   big-endian and little-endian variants. */
unsigned int __wt_crc32c_le(unsigned int, const unsigned char *, size_t);

/* Hardware-accelerated versions of the above. It is up to the caller
   to detect the availability of vector facility and kernel support. */
unsigned int __wt_crc32c_le_vx(unsigned int, const unsigned char *, size_t);
