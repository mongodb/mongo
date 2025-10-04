/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */
#ifndef LINUX_MODULE_H
#define LINUX_MODULE_H

#define EXPORT_SYMBOL(symbol)                                                  \
  void* __##symbol = symbol
#define EXPORT_SYMBOL_GPL(symbol)                                              \
  void* __##symbol = symbol
#define MODULE_LICENSE(license)
#define MODULE_DESCRIPTION(description)

#endif
