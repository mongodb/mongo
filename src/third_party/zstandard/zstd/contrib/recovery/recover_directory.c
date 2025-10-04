/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define ZSTD_STATIC_LINKING_ONLY
#include "util.h"
#include "zstd.h"

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d CHECK(%s) failed: ", __FILE__, __LINE__, #cond);  \
      fprintf(stderr, "" __VA_ARGS__);                                         \
      fprintf(stderr, "\n");                                                   \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

static void usage(char const *program) {
  fprintf(stderr, "USAGE: %s FILE.zst PREFIX\n", program);
  fprintf(stderr, "FILE.zst: A zstd compressed file with multiple frames\n");
  fprintf(stderr, "PREFIX:   The output prefix. Uncompressed files will be "
                  "created named ${PREFIX}0 ${PREFIX}1...\n\n");
  fprintf(stderr, "This program takes concatenated zstd frames and "
                  "decompresses them into individual files.\n");
  fprintf(stderr, "E.g. files created with a command like: zstd -r directory "
                  "-o file.zst\n");
}

typedef struct {
  char *data;
  size_t size;
  size_t frames;
  size_t maxFrameSize;
} ZstdFrames;

static ZstdFrames readFile(char const *fileName) {
  U64 const fileSize = UTIL_getFileSize(fileName);
  CHECK(fileSize != UTIL_FILESIZE_UNKNOWN, "Unknown file size!");

  char *const data = (char *)malloc(fileSize);
  CHECK(data != NULL, "Allocation failed");

  FILE *file = fopen(fileName, "rb");
  CHECK(file != NULL, "fopen failed");

  size_t const readSize = fread(data, 1, fileSize, file);
  CHECK(readSize == fileSize, "fread failed");

  fclose(file);
  ZstdFrames frames;
  frames.data = (char *)data;
  frames.size = fileSize;
  frames.frames = 0;

  size_t index;
  size_t maxFrameSize = 0;
  for (index = 0; index < fileSize;) {
    size_t const frameSize =
        ZSTD_findFrameCompressedSize(data + index, fileSize - index);
    CHECK(!ZSTD_isError(frameSize), "Bad zstd frame: %s",
          ZSTD_getErrorName(frameSize));
    if (frameSize > maxFrameSize)
      maxFrameSize = frameSize;
    frames.frames += 1;
    index += frameSize;
  }
  CHECK(index == fileSize, "Zstd file corrupt!");
  frames.maxFrameSize = maxFrameSize;

  return frames;
}

static int computePadding(size_t numFrames) {
  return snprintf(NULL, 0, "%u", (unsigned)numFrames);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    usage(argv[0]);
    exit(1);
  }
  char const *const zstdFile = argv[1];
  char const *const prefix = argv[2];

  ZstdFrames frames = readFile(zstdFile);

  if (frames.frames <= 1) {
    fprintf(
        stderr,
        "%s only has %u zstd frame. Simply use `zstd -d` to decompress it.\n",
        zstdFile, (unsigned)frames.frames);
    exit(1);
  }

  int const padding = computePadding(frames.frames - 1);

  size_t const outFileNameSize = strlen(prefix) + padding + 1;
  char* outFileName = malloc(outFileNameSize);
  CHECK(outFileName != NULL, "Allocation failure");

  size_t const bufferSize = 128 * 1024;
  void *buffer = malloc(bufferSize);
  CHECK(buffer != NULL, "Allocation failure");

  ZSTD_DCtx* dctx = ZSTD_createDCtx();
  CHECK(dctx != NULL, "Allocation failure");

  fprintf(stderr, "Recovering %u files...\n", (unsigned)frames.frames);

  size_t index;
  size_t frame = 0;
  for (index = 0; index < frames.size; ++frame) {
    size_t const frameSize =
        ZSTD_findFrameCompressedSize(frames.data + index, frames.size - index);

    int const ret = snprintf(outFileName, outFileNameSize, "%s%0*u", prefix, padding, (unsigned)frame);
    CHECK(ret >= 0 && (size_t)ret <= outFileNameSize, "snprintf failed!");

    FILE* outFile = fopen(outFileName, "wb");
    CHECK(outFile != NULL, "fopen failed");

    ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
    ZSTD_inBuffer in = {frames.data + index, frameSize, 0};
    while (in.pos < in.size) {
        ZSTD_outBuffer out = {buffer, bufferSize, 0};
        CHECK(!ZSTD_isError(ZSTD_decompressStream(dctx, &out, &in)), "decompression failed");
        size_t const writeSize = fwrite(out.dst, 1, out.pos, outFile);
        CHECK(writeSize == out.pos, "fwrite failed");
    }
    fclose(outFile);
    fprintf(stderr, "Recovered %s\n", outFileName);
    index += frameSize;
  }
  fprintf(stderr, "Complete\n");

  free(outFileName);
  ZSTD_freeDCtx(dctx);
  free(buffer);
  free(frames.data);
  return 0;
}
