/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef IGZIP_WRAPPER_H

#define DEFLATE_METHOD 8
#define ZLIB_DICT_FLAG (1 << 5)
#define TEXT_FLAG (1 << 0)
#define HCRC_FLAG (1 << 1)
#define EXTRA_FLAG (1 << 2)
#define NAME_FLAG (1 << 3)
#define COMMENT_FLAG (1 << 4)
#define UNDEFINED_FLAG (-1)

#define GZIP_HDR_BASE 10
#define GZIP_EXTRA_LEN 2
#define GZIP_HCRC_LEN 2
#define GZIP_TRAILER_LEN 8

#define ZLIB_HDR_BASE 2
#define ZLIB_DICT_LEN 4
#define ZLIB_INFO_OFFSET 4
#define ZLIB_LEVEL_OFFSET 6
#define ZLIB_TRAILER_LEN 4

#endif
