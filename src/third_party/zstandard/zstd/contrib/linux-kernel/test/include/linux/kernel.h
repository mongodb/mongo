/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */
#ifndef LINUX_KERNEL_H
#define LINUX_KERNEL_H

#define WARN_ON(x)

#define PTR_ALIGN(p, a) (typeof(p))ALIGN((unsigned long long)(p), (a))
#define ALIGN(x, a)         ALIGN_MASK((x), (a) - 1)
#define ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))

#endif
