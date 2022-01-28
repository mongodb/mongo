/**
 * \file simple.c
 * Simple standalone example of using the single-file \c zstddeclib.
 * 
 * \note In this simple example we include the amalgamated source and compile
 * just this single file, but we could equally (and more conventionally)
 * include \c zstd.h and compile both this file and \c zstddeclib.c (the
 * resulting binaries differ slightly in size but perform the same).
 * 
 * \author Carl Woffenden, Numfum GmbH (released under a CC0 license)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../zstddeclib.c"

//************************* Test Data (DXT texture) **************************/

/**
 * Raw 256x256 DXT1 data (used to compare the result).
 * \n
 * See \c testcard.png for the original.
 */
static uint8_t const rawDxt1[] = {
#include "testcard-dxt1.inl"
};

/**
 * Zstd compressed version of \c #rawDxt1.
 * \n
 * See \c testcard.png for the original.
 */
static uint8_t const srcZstd[] = {
#include "testcard-zstd.inl"
};

/**
 * Destination for decoding \c #srcZstd.
 */
static uint8_t dstDxt1[sizeof rawDxt1] = {};

#ifndef ZSTD_VERSION_MAJOR
/**
 * For the case where the decompression library hasn't been included we add a
 * dummy function to fake the process and stop the buffers being optimised out.
 */
size_t ZSTD_decompress(void* dst, size_t dstLen, const void* src, size_t srcLen) {
	return (memcmp(dst, src, (srcLen < dstLen) ? srcLen : dstLen)) ? 0 : dstLen;
}
#endif

//****************************************************************************/

/**
 * Simple single-file test to decompress \c #srcZstd into \c # dstDxt1 then
 * compare the resulting bytes with \c #rawDxt1.
 * \n
 * As a (naive) comparison, removing Zstd and building with "-Os -g0 simple.c"
 * results in a 44kB binary (macOS 10.14, Clang 10); re-adding Zstd increases
 * the binary by 56kB (after calling \c strip).
 */
int main() {
	size_t size = ZSTD_decompress(dstDxt1, sizeof dstDxt1, srcZstd, sizeof srcZstd);
	int compare = memcmp(rawDxt1, dstDxt1, sizeof dstDxt1);
	printf("Decompressed size: %s\n", (size == sizeof dstDxt1) ? "PASSED" : "FAILED");
	printf("Byte comparison: %s\n", (compare == 0) ? "PASSED" : "FAILED");
	if (size == sizeof dstDxt1 && compare == 0) {
		return EXIT_SUCCESS;
	}
	return EXIT_FAILURE;
}
