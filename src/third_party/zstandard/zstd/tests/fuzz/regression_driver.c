/*
 * Copyright (c) Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include "fuzz.h"
#include "fuzz_helpers.h"
#include "util.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char const **argv) {
  size_t const kMaxFileSize = (size_t)1 << 27;
  int const kFollowLinks = 1;
  FileNamesTable* files;
  const char** const fnTable = argv + 1;
  uint8_t *buffer = NULL;
  size_t bufferSize = 0;
  unsigned i;
  unsigned numFilesTested = 0;
  int ret = 0;

  {
    unsigned const numFiles = (unsigned)(argc - 1);
#ifdef UTIL_HAS_CREATEFILELIST
    files = UTIL_createExpandedFNT(fnTable, numFiles, kFollowLinks);
#else
    files = UTIL_createFNT_fromROTable(fnTable, numFiles);
    assert(numFiles == files->tableSize);
#endif
  }
  if (!files) {
    fprintf(stderr, "ERROR: Failed to create file names table\n");
    return 1;
  }
  if (files->tableSize == 0)
    fprintf(stderr, "WARNING: No files passed to %s\n", argv[0]);
  for (i = 0; i < files->tableSize; ++i) {
    char const *fileName = files->fileNames[i];
    DEBUGLOG(3, "Running %s", fileName);
    size_t const fileSize = UTIL_getFileSize(fileName);
    size_t readSize;
    FILE *file;

    /* Check that it is a regular file, and that the fileSize is valid.
     * If it is not a regular file, then it may have been deleted since we
     * constructed the list, so just skip it, but return an error exit code.
     */
    if (!UTIL_isRegularFile(fileName)) {
      ret = 1;
      continue;
    }
    FUZZ_ASSERT_MSG(fileSize <= kMaxFileSize, fileName);
    /* Ensure we have a large enough buffer allocated */
    if (fileSize > bufferSize) {
      free(buffer);
      buffer = (uint8_t *)malloc(fileSize);
      FUZZ_ASSERT_MSG(buffer, fileName);
      bufferSize = fileSize;
    }
    /* Open the file */
    file = fopen(fileName, "rb");
    FUZZ_ASSERT_MSG(file, fileName);
    /* Read the file */
    readSize = fread(buffer, 1, fileSize, file);
    FUZZ_ASSERT_MSG(readSize == fileSize, fileName);
    /* Close the file */
    fclose(file);
    /* Run the fuzz target */
    LLVMFuzzerTestOneInput(buffer, fileSize);
    ++numFilesTested;
  }
  fprintf(stderr, "Tested %u files: ", numFilesTested);
  if (ret == 0) {
    fprintf(stderr, "Success!\n");
  } else {
    fprintf(stderr, "Failure!\n");
  }
  free(buffer);
  UTIL_freeFileNamesTable(files);
  return ret;
}
