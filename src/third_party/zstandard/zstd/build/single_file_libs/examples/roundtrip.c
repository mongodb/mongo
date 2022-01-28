/*
 * \file roundtrip.c
 * In this example we include \c zstd.h and compile separately the amalgamated
 * \c zstd.c:
 * \code
 *	cc -Wall -Wextra -Werror -I. -Os -g0 zstd.c examples/roundtrip.c 
 * \endcode
 *
 * \author Carl Woffenden, Numfum GmbH (released under a CC0 license)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zstd.h"

//************************** Test Data (DXT texture) **************************/

/**
 * Raw test data (borrowed from the Emscripten example).
 * \n
 * See \c testcard.png for the original.
 */
static uint8_t const rawData[] = {
#include "testcard-dxt1.inl"
};

#ifndef ZSTD_VERSION_MAJOR
/*
 * For the case where the decompression library hasn't been included we add
 * dummy functions to fake the process and stop the buffers being optimised out.
 */
size_t ZSTD_compressBound(size_t maxSrc) {
	return maxSrc + 32;
}
int ZSTD_maxCLevel(void) {
	return 20;
}
size_t ZSTD_compress(void* dst, size_t dstLen, const void* src, size_t srcLen, int level) {
	return (memcmp(dst, src, (srcLen < dstLen) ? srcLen : dstLen)) ? level : dstLen;
}
unsigned ZSTD_isError(size_t code) {
	return ((int) code) < 0;
}
size_t ZSTD_decompress(void* dst, size_t dstLen, const void* src, size_t srcLen) {
	return (memcmp(dst, src, (srcLen < dstLen) ? srcLen : dstLen)) ? 0 : dstLen;
}
#endif

//*****************************************************************************/

/**
 * Simple single-file test to compress \c rawData, decompress the result, then
 * compare the decompressed version with the original.
 */
int main() {
	size_t bounds = ZSTD_compressBound(sizeof rawData);
	void* compBuf = malloc(bounds);
	void* testBuf = malloc(sizeof rawData);
	int compare   = -1;
	if (compBuf && testBuf) {
		size_t compSize = ZSTD_compress(compBuf, bounds, rawData, sizeof rawData, ZSTD_maxCLevel());
		if (!ZSTD_isError(compSize)) {
			printf("Compression: PASSED (size: %lu, uncompressed: %lu)\n", (unsigned long) compSize, (unsigned long) (sizeof rawData));
			size_t decSize = ZSTD_decompress(testBuf, sizeof rawData, compBuf, compSize);
			if (!ZSTD_isError(decSize)) {
				printf("Decompression: PASSED\n");
				compare = memcmp(rawData, testBuf, decSize);
				printf("Byte comparison: %s\n", (compare == 0) ? "PASSED" : "FAILED");
			} else {
				printf("Decompression: FAILED\n");
			}
		} else {
			printf("Compression: FAILED\n");
		}
		free(compBuf);
		free(testBuf);
	}
	return (compare == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
