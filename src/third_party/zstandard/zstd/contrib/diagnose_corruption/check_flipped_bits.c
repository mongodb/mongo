/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "zstd_errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  char *input;
  size_t input_size;

  char *perturbed; /* same size as input */

  char *output;
  size_t output_size;

  const char *dict_file_name;
  const char *dict_file_dir_name;
  int32_t dict_id;
  char *dict;
  size_t dict_size;
  ZSTD_DDict* ddict;

  ZSTD_DCtx* dctx;

  int success_count;
  int error_counts[ZSTD_error_maxCode];
} stuff_t;

static void free_stuff(stuff_t* stuff) {
  free(stuff->input);
  free(stuff->output);
  ZSTD_freeDDict(stuff->ddict);
  free(stuff->dict);
  ZSTD_freeDCtx(stuff->dctx);
}

static void usage(void) {
  fprintf(stderr, "check_flipped_bits input_filename [-d dict] [-D dict_dir]\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Arguments:\n");
  fprintf(stderr, "    -d file: path to a dictionary file to use.\n");
  fprintf(stderr, "    -D dir : path to a directory, with files containing dictionaries, of the\n"
                  "             form DICTID.zstd-dict, e.g., 12345.zstd-dict.\n");
  exit(1);
}

static void print_summary(stuff_t* stuff) {
  int error_code;
  fprintf(stderr, "%9d successful decompressions\n", stuff->success_count);
  for (error_code = 0; error_code < ZSTD_error_maxCode; error_code++) {
    int count = stuff->error_counts[error_code];
    if (count) {
      fprintf(
          stderr, "%9d failed decompressions with message: %s\n",
          count, ZSTD_getErrorString(error_code));
    }
  }
}

static char* readFile(const char* filename, size_t* size) {
  struct stat statbuf;
  int ret;
  FILE* f;
  char *buf;
  size_t bytes_read;

  ret = stat(filename, &statbuf);
  if (ret != 0) {
    fprintf(stderr, "stat failed: %m\n");
    return NULL;
  }
  if ((statbuf.st_mode & S_IFREG) != S_IFREG) {
    fprintf(stderr, "Input must be regular file\n");
    return NULL;
  }

  *size = statbuf.st_size;

  f = fopen(filename, "r");
  if (f == NULL) {
    fprintf(stderr, "fopen failed: %m\n");
    return NULL;
  }

  buf = malloc(*size);
  if (buf == NULL) {
    fprintf(stderr, "malloc failed\n");
    fclose(f);
    return NULL;
  }

  bytes_read = fread(buf, 1, *size, f);
  if (bytes_read != *size) {
    fprintf(stderr, "failed to read whole file\n");
    fclose(f);
    free(buf);
    return NULL;
  }

  ret = fclose(f);
  if (ret != 0) {
    fprintf(stderr, "fclose failed: %m\n");
    free(buf);
    return NULL;
  }

  return buf;
}

static ZSTD_DDict* readDict(const char* filename, char **buf, size_t* size, int32_t* dict_id) {
  ZSTD_DDict* ddict;
  *buf = readFile(filename, size);
  if (*buf == NULL) {
    fprintf(stderr, "Opening dictionary file '%s' failed\n", filename);
    return NULL;
  }

  ddict = ZSTD_createDDict_advanced(*buf, *size, ZSTD_dlm_byRef, ZSTD_dct_auto, ZSTD_defaultCMem);
  if (ddict == NULL) {
    fprintf(stderr, "Failed to create ddict.\n");
    return NULL;
  }
  if (dict_id != NULL) {
    *dict_id = ZSTD_getDictID_fromDDict(ddict);
  }
  return ddict;
}

static ZSTD_DDict* readDictByID(stuff_t *stuff, int32_t dict_id, char **buf, size_t* size) {
  if (stuff->dict_file_dir_name == NULL) {
    return NULL;
  } else {
    size_t dir_name_len = strlen(stuff->dict_file_dir_name);
    int dir_needs_separator = 0;
    size_t dict_file_name_alloc_size = dir_name_len + 1 /* '/' */ + 10 /* max int32_t len */ + strlen(".zstd-dict") + 1 /* '\0' */;
    char *dict_file_name = malloc(dict_file_name_alloc_size);
    ZSTD_DDict* ddict;
    int32_t read_dict_id;
    if (dict_file_name == NULL) {
      fprintf(stderr, "malloc failed.\n");
      return 0;
    }

    if (dir_name_len > 0 && stuff->dict_file_dir_name[dir_name_len - 1] != '/') {
      dir_needs_separator = 1;
    }

    snprintf(
      dict_file_name,
      dict_file_name_alloc_size,
      "%s%s%u.zstd-dict",
      stuff->dict_file_dir_name,
      dir_needs_separator ? "/" : "",
      dict_id);

    /* fprintf(stderr, "Loading dict %u from '%s'.\n", dict_id, dict_file_name); */

    ddict = readDict(dict_file_name, buf, size, &read_dict_id);
    if (ddict == NULL) {
      fprintf(stderr, "Failed to create ddict from '%s'.\n", dict_file_name);
      free(dict_file_name);
      return 0;
    }
    if (read_dict_id != dict_id) {
      fprintf(stderr, "Read dictID (%u) does not match expected (%u).\n", read_dict_id, dict_id);
      free(dict_file_name);
      ZSTD_freeDDict(ddict);
      return 0;
    }

    free(dict_file_name);
    return ddict;
  }
}

static int init_stuff(stuff_t* stuff, int argc, char *argv[]) {
  const char* input_filename;

  if (argc < 2) {
    usage();
  }

  input_filename = argv[1];
  stuff->input_size = 0;
  stuff->input = readFile(input_filename, &stuff->input_size);
  if (stuff->input == NULL) {
    fprintf(stderr, "Failed to read input file.\n");
    return 0;
  }

  stuff->perturbed = malloc(stuff->input_size);
  if (stuff->perturbed == NULL) {
    fprintf(stderr, "malloc failed.\n");
    return 0;
  }
  memcpy(stuff->perturbed, stuff->input, stuff->input_size);

  stuff->output_size = ZSTD_DStreamOutSize();
  stuff->output = malloc(stuff->output_size);
  if (stuff->output == NULL) {
    fprintf(stderr, "malloc failed.\n");
    return 0;
  }

  stuff->dict_file_name = NULL;
  stuff->dict_file_dir_name = NULL;
  stuff->dict_id = 0;
  stuff->dict = NULL;
  stuff->dict_size = 0;
  stuff->ddict = NULL;

  if (argc > 2) {
    if (!strcmp(argv[2], "-d")) {
      if (argc > 3) {
        stuff->dict_file_name = argv[3];
      } else {
        usage();
      }
    } else
    if (!strcmp(argv[2], "-D")) {
      if (argc > 3) {
        stuff->dict_file_dir_name = argv[3];
      } else {
        usage();
      }
    } else {
      usage();
    }
  }

  if (stuff->dict_file_dir_name) {
    int32_t dict_id = ZSTD_getDictID_fromFrame(stuff->input, stuff->input_size);
    if (dict_id != 0) {
      stuff->ddict = readDictByID(stuff, dict_id, &stuff->dict, &stuff->dict_size);
      if (stuff->ddict == NULL) {
        fprintf(stderr, "Failed to create cached ddict.\n");
        return 0;
      }
      stuff->dict_id = dict_id;
    }
  } else
  if (stuff->dict_file_name) {
    stuff->ddict = readDict(stuff->dict_file_name, &stuff->dict, &stuff->dict_size, &stuff->dict_id);
    if (stuff->ddict == NULL) {
      fprintf(stderr, "Failed to create ddict from '%s'.\n", stuff->dict_file_name);
      return 0;
    }
  }

  stuff->dctx = ZSTD_createDCtx();
  if (stuff->dctx == NULL) {
    return 0;
  }

  stuff->success_count = 0;
  memset(stuff->error_counts, 0, sizeof(stuff->error_counts));

  return 1;
}

static int test_decompress(stuff_t* stuff) {
  size_t ret;
  ZSTD_inBuffer in = {stuff->perturbed, stuff->input_size, 0};
  ZSTD_outBuffer out = {stuff->output, stuff->output_size, 0};
  ZSTD_DCtx* dctx = stuff->dctx;
  int32_t custom_dict_id = ZSTD_getDictID_fromFrame(in.src, in.size);
  char *custom_dict = NULL;
  size_t custom_dict_size = 0;
  ZSTD_DDict* custom_ddict = NULL;

  if (custom_dict_id != 0 && custom_dict_id != stuff->dict_id) {
    /* fprintf(stderr, "Instead of dict %u, this perturbed blob wants dict %u.\n", stuff->dict_id, custom_dict_id); */
    custom_ddict = readDictByID(stuff, custom_dict_id, &custom_dict, &custom_dict_size);
  }

  ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);

  if (custom_ddict != NULL) {
    ZSTD_DCtx_refDDict(dctx, custom_ddict);
  } else {
    ZSTD_DCtx_refDDict(dctx, stuff->ddict);
  }

  while (in.pos != in.size) {
    out.pos = 0;
    ret = ZSTD_decompressStream(dctx, &out, &in);

    if (ZSTD_isError(ret)) {
      unsigned int code = ZSTD_getErrorCode(ret);
      if (code >= ZSTD_error_maxCode) {
        fprintf(stderr, "Received unexpected error code!\n");
        exit(1);
      }
      stuff->error_counts[code]++;
      /*
      fprintf(
          stderr, "Decompression failed: %s\n", ZSTD_getErrorName(ret));
      */
      if (custom_ddict != NULL) {
        ZSTD_freeDDict(custom_ddict);
        free(custom_dict);
      }
      return 0;
    }
  }

  stuff->success_count++;

  if (custom_ddict != NULL) {
    ZSTD_freeDDict(custom_ddict);
    free(custom_dict);
  }
  return 1;
}

static int perturb_bits(stuff_t* stuff) {
  size_t pos;
  size_t bit;
  for (pos = 0; pos < stuff->input_size; pos++) {
    unsigned char old_val = stuff->input[pos];
    if (pos % 1000 == 0) {
      fprintf(stderr, "Perturbing byte %zu / %zu\n", pos, stuff->input_size);
    }
    for (bit = 0; bit < 8; bit++) {
      unsigned char new_val = old_val ^ (1 << bit);
      stuff->perturbed[pos] = new_val;
      if (test_decompress(stuff)) {
        fprintf(
            stderr,
            "Flipping byte %zu bit %zu (0x%02x -> 0x%02x) "
            "produced a successful decompression!\n",
            pos, bit, old_val, new_val);
      }
    }
    stuff->perturbed[pos] = old_val;
  }
  return 1;
}

static int perturb_bytes(stuff_t* stuff) {
  size_t pos;
  size_t new_val;
  for (pos = 0; pos < stuff->input_size; pos++) {
    unsigned char old_val = stuff->input[pos];
    if (pos % 1000 == 0) {
      fprintf(stderr, "Perturbing byte %zu / %zu\n", pos, stuff->input_size);
    }
    for (new_val = 0; new_val < 256; new_val++) {
      stuff->perturbed[pos] = new_val;
      if (test_decompress(stuff)) {
        fprintf(
            stderr,
            "Changing byte %zu (0x%02x -> 0x%02x) "
            "produced a successful decompression!\n",
            pos, old_val, (unsigned char)new_val);
      }
    }
    stuff->perturbed[pos] = old_val;
  }
  return 1;
}

int main(int argc, char* argv[]) {
  stuff_t stuff;

  if(!init_stuff(&stuff, argc, argv)) {
    fprintf(stderr, "Failed to init.\n");
    return 1;
  }

  if (test_decompress(&stuff)) {
    fprintf(stderr, "Blob already decompresses successfully!\n");
    return 1;
  }

  perturb_bits(&stuff);

  perturb_bytes(&stuff);

  print_summary(&stuff);

  free_stuff(&stuff);

  return 0;
}
