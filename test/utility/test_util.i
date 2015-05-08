/*-
 * Public Domain 2014-2015 MongoDB, Inc.
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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <wiredtiger.h>

#ifdef _WIN32
#include "windows_shim.h"
#endif

#ifdef _WIN32
	#define DIR_DELIM '\\'
	#define RM_COMMAND "rd /s /q "
#else
	#define	DIR_DELIM '/'
	#define RM_COMMAND "rm -rf "
#endif

#define	DEFAULT_DIR "WT_TEST"
#define	MKDIR_COMMAND "mkdir "

/*
 * die --
 *	Report an error and quit.
 */
static inline void
testutil_die(int e, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (e != 0)
		fprintf(stderr, ": %s", wiredtiger_strerror(e));
	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
}

/*
 * testutil_work_dir_from_path --
 *	Takes a buffer, its size and the intended work directory.
 *	Creates the full intended work directory in buffer.
 */
static inline int
testutil_work_dir_from_path(char *buffer, size_t inputSize, char *dir)
{
	/* If no directory is provided, use the default. */
	if (dir == NULL) {
		if (inputSize < sizeof(DEFAULT_DIR))
			return (1);
		snprintf(buffer, inputSize, DEFAULT_DIR);
		return (0);
	}

	/* Additional bytes for the directory and WT_TEST. */
	if (inputSize < strlen(dir) + sizeof(DEFAULT_DIR))
		return (1);

	snprintf(buffer, inputSize, "%s%c%s", dir, DIR_DELIM, DEFAULT_DIR);
	return (0);
}

/*
 * testutil_clean_work_dir --
 *	Remove any existing work directories, can optionally fail on error
 */
static inline int
testutil_clean_work_dir(char *dir)
{
	size_t inputSize;
	int ret;
	char *buffer;

	/* Additional bytes for the Windows rd command. */
	inputSize = strlen(dir) + sizeof(RM_COMMAND);
	if ((buffer = malloc(inputSize)) == NULL)
		testutil_die(ENOMEM, "Failed to allocate memory");

	snprintf(buffer, inputSize, "%s%s", RM_COMMAND, dir);
	ret = system(buffer);
	free(buffer);
	return (ret);
}

/*
 * testutil_make_work_dir --
 *	Delete the existing work directory if it exists, then create a new one.
 */
static inline void
testutil_make_work_dir(char *dir)
{
	size_t inputSize;
	int ret;
	char *buffer;

	testutil_clean_work_dir(dir);

	/* Additional bytes for the mkdir command */
	inputSize = strlen(dir) + sizeof(MKDIR_COMMAND);
	if ((buffer = malloc(inputSize)) == NULL)
		testutil_die(ENOMEM, "Failed to allocate memory");

	/* mkdir shares syntax between Windows and Linux */
	snprintf(buffer, inputSize, "%s%s", MKDIR_COMMAND, dir);
	if ((ret = system(buffer)) != 0)
		testutil_die(ret, "directory create call of '%s%s' failed",
		    MKDIR_COMMAND, dir);
	free(buffer);
}

#undef	M_W
#define	M_W	(rnd)[0]
#undef	M_Z
#define	M_Z	(rnd)[1]

/*
 * testutil_random_init --
 *	Initialize return of a 32-bit pseudo-random number.
 */
static inline void
testutil_random_init(uint32_t *rnd)
{
	M_W = 521288629;
	M_Z = 362436069;
}

/*
 * testutil_random --
 *	Return a 32-bit pseudo-random number.
 *
 * This is an implementation of George Marsaglia's multiply-with-carry pseudo-
 * random number generator.  Computationally fast, with reasonable randomness
 * properties.
 *
 * We have to be very careful about races here.  Multiple threads can call
 * __wt_random concurrently, and it is okay if those concurrent calls get the
 * same return value.  What is *not* okay is if reading the shared state races
 * with an update and uses two different values for m_w or m_z.  That could
 * result in a value of zero, in which case they would be stuck on zero
 * forever.  Take local copies of the shared values to avoid this.
 */
static inline uint32_t
testutil_random(uint32_t *rnd)
{
	uint32_t w = M_W, z = M_Z;

	M_Z = z = 36969 * (z & 65535) + (z >> 16);
	M_W = w = 18000 * (w & 65535) + (w >> 16);
	return (z << 16) + (w & 65535);
}
