/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */
/* minimal set of defines required to generate zstd.res from zstd.rc */

#define VS_VERSION_INFO         1

#define VS_FFI_FILEFLAGSMASK    0x0000003FL
#define VOS_NT_WINDOWS32        0x00040004L
#define VFT_DLL                 0x00000002L
#define VFT2_UNKNOWN            0x00000000L
