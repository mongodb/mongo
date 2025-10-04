/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */
#ifndef LINUX_SWAB_H
#define LINUX_SWAB_H

#define swab32(x) __builtin_bswap32((x))
#define swab64(x) __builtin_bswap64((x))

#endif
