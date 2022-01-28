/*
 * Copyright (c) 2016-2021, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */
#ifndef LINUX_COMPILER_H
#define LINUX_COMPILER_H

#ifndef inline
#define inline __inline __attribute__((unused))
#endif

#ifndef noinline
#define noinline __attribute__((noinline))
#endif

#define fallthrough __attribute__((__fallthrough__))

#endif
