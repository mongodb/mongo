/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Memory reporting infrastructure. */

#ifndef mozilla_MemoryReporting_h
#define mozilla_MemoryReporting_h

#include <stddef.h>

#ifdef __cplusplus

namespace mozilla {

/*
 * This is for functions that are like malloc_usable_size.  Such functions are
 * used for measuring the size of data structures.
 */
typedef size_t (*MallocSizeOf)(const void* p);

} /* namespace mozilla */

#endif /* __cplusplus */

typedef size_t (*MozMallocSizeOf)(const void* p);

#endif /* mozilla_MemoryReporting_h */
