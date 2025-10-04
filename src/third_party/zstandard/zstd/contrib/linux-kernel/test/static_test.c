/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decompress_sources.h"
#include <linux/zstd.h>

#define CONTROL(x)                                                             \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%s:%u: %s failed!\n", __FUNCTION__, __LINE__, #x);      \
      abort();                                                                 \
    }                                                                          \
  } while (0)


static const char kEmptyZstdFrame[] = {
    0x28, 0xb5, 0x2f, 0xfd, 0x24, 0x00, 0x01, 0x00, 0x00, 0x99, 0xe9, 0xd8, 0x51
};

static void test_decompress_unzstd(void) {
    fprintf(stderr, "Testing decompress unzstd... ");
    {
        size_t const wkspSize = zstd_dctx_workspace_bound();
        void* wksp = malloc(wkspSize);
        ZSTD_DCtx* dctx = zstd_init_dctx(wksp, wkspSize);
        CONTROL(wksp != NULL);
        CONTROL(dctx != NULL);
        {
          size_t const dSize = zstd_decompress_dctx(dctx, NULL, 0, kEmptyZstdFrame, sizeof(kEmptyZstdFrame));
          CONTROL(!zstd_is_error(dSize));
          CONTROL(dSize == 0);
        }
        free(wksp);
    }
    fprintf(stderr, "Ok\n");
}

int main(void) {
  test_decompress_unzstd();
  return 0;
}
