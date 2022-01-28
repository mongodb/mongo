/*
 * Copyright (c) Facebook, Inc.
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

#include <linux/zstd.h>

#define CONTROL(x)                                                             \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%s:%u: %s failed!\n", __FUNCTION__, __LINE__, #x);      \
      abort();                                                                 \
    }                                                                          \
  } while (0)

typedef struct {
  char *data;
  char *data2;
  size_t dataSize;
  char *comp;
  size_t compSize;
} test_data_t;

static test_data_t create_test_data(void) {
  test_data_t data;
  data.dataSize = 128 * 1024;
  data.data = (char*)malloc(data.dataSize);
  CONTROL(data.data != NULL);
  data.data2 = (char*)malloc(data.dataSize);
  CONTROL(data.data2 != NULL);
  data.compSize = zstd_compress_bound(data.dataSize);
  data.comp = (char*)malloc(data.compSize);
  CONTROL(data.comp != NULL);
  memset(data.data, 0, data.dataSize);
  return data;
}

static void free_test_data(test_data_t const *data) {
  free(data->data);
  free(data->data2);
  free(data->comp);
}

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static void test_btrfs(test_data_t const *data) {
  size_t const size = MIN(data->dataSize, 128 * 1024);
  fprintf(stderr, "testing btrfs use cases... ");
  for (int level = -1; level < 16; ++level) {
    zstd_parameters params = zstd_get_params(level, size);
    size_t const workspaceSize =
        MAX(zstd_cstream_workspace_bound(&params.cParams),
            zstd_dstream_workspace_bound(size));
    void *workspace = malloc(workspaceSize);

    char const *ip = data->data;
    char const *iend = ip + size;
    char *op = data->comp;
    char *oend = op + data->compSize;

    CONTROL(params.cParams.windowLog <= 17);
    CONTROL(workspace != NULL);
    {
      zstd_cstream *cctx = zstd_init_cstream(&params, size, workspace, workspaceSize);
      zstd_out_buffer out = {NULL, 0, 0};
      zstd_in_buffer in = {NULL, 0, 0};
      CONTROL(cctx != NULL);
      for (;;) {
        if (in.pos == in.size) {
          in.src = ip;
          in.size = MIN(4096, iend - ip);
          in.pos = 0;
          ip += in.size;
        }

        if (out.pos == out.size) {
          out.dst = op;
          out.size = MIN(4096, oend - op);
          out.pos = 0;
          op += out.size;
        }

        if (ip != iend || in.pos < in.size) {
          CONTROL(!zstd_is_error(zstd_compress_stream(cctx, &out, &in)));
        } else {
          size_t const ret = zstd_end_stream(cctx, &out);
          CONTROL(!zstd_is_error(ret));
          if (ret == 0) {
            break;
          }
        }
      }
      op += out.pos;
    }

    ip = data->comp;
    iend = op;
    op = data->data2;
    oend = op + size;
    {
      zstd_dstream *dctx = zstd_init_dstream(1ULL << params.cParams.windowLog, workspace, workspaceSize);
      zstd_out_buffer out = {NULL, 0, 0};
      zstd_in_buffer in = {NULL, 0, 0};
      CONTROL(dctx != NULL);
      for (;;) {
        if (in.pos == in.size) {
          in.src = ip;
          in.size = MIN(4096, iend - ip);
          in.pos = 0;
          ip += in.size;
        }

        if (out.pos == out.size) {
          out.dst = op;
          out.size = MIN(4096, oend - op);
          out.pos = 0;
          op += out.size;
        }
        {
          size_t const ret = zstd_decompress_stream(dctx, &out, &in);
          CONTROL(!zstd_is_error(ret));
          if (ret == 0) {
            break;
          }
        }
      }
    }
    CONTROL((size_t)(op - data->data2) == data->dataSize);
    CONTROL(!memcmp(data->data, data->data2, data->dataSize));
    free(workspace);
  }
  fprintf(stderr, "Ok\n");
}

static void test_decompress_unzstd(test_data_t const *data) {
    size_t cSize;
    fprintf(stderr, "Testing decompress unzstd... ");
    {
        zstd_parameters params = zstd_get_params(19, 0);
        size_t const wkspSize = zstd_cctx_workspace_bound(&params.cParams);
        void* wksp = malloc(wkspSize);
        zstd_cctx* cctx = zstd_init_cctx(wksp, wkspSize);
        CONTROL(wksp != NULL);
        CONTROL(cctx != NULL);
        cSize = zstd_compress_cctx(cctx, data->comp, data->compSize, data->data, data->dataSize, &params);
        CONTROL(!zstd_is_error(cSize));
        free(wksp);
    }
    {
        size_t const wkspSize = zstd_dctx_workspace_bound();
        void* wksp = malloc(wkspSize);
        zstd_dctx* dctx = zstd_init_dctx(wksp, wkspSize);
        CONTROL(wksp != NULL);
        CONTROL(dctx != NULL);
        {
          size_t const dSize = zstd_decompress_dctx(dctx, data->data2, data->dataSize, data->comp, cSize);
          CONTROL(!zstd_is_error(dSize));
          CONTROL(dSize == data->dataSize);
        }
        CONTROL(!memcmp(data->data, data->data2, data->dataSize));
        free(wksp);
    }
    fprintf(stderr, "Ok\n");
}

static void test_f2fs(void) {
  fprintf(stderr, "testing f2fs uses... ");
  CONTROL(zstd_min_clevel() < 0);
  CONTROL(zstd_max_clevel() == 22);
  fprintf(stderr, "Ok\n");
}

static char *g_stack = NULL;

static void __attribute__((noinline)) use(void *x) {
  asm volatile("" : "+r"(x));
}

static void __attribute__((noinline)) set_stack(void) {

  char stack[8192];
  g_stack = stack;
  memset(g_stack, 0x33, 8192);
  use(g_stack);
}

static void __attribute__((noinline)) check_stack(void) {
  size_t cleanStack = 0;
  while (cleanStack < 8192 && g_stack[cleanStack] == 0x33) {
    ++cleanStack;
  }
  {
    size_t const stackSize = 8192 - cleanStack;
    fprintf(stderr, "Maximum stack size: %zu\n", stackSize);
    CONTROL(stackSize <= 2048 + 512);
  }
}

static void test_stack_usage(test_data_t const *data) {
  set_stack();
  test_f2fs();
  test_btrfs(data);
  test_decompress_unzstd(data);
  check_stack();
}

int main(void) {
  test_data_t data = create_test_data();
  test_f2fs();
  test_btrfs(&data);
  test_decompress_unzstd(&data);
  test_stack_usage(&data);
  free_test_data(&data);
  return 0;
}
