// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/config.h"  // IWYU pragma: keep

#include <cstddef>
#include <cstring>

namespace mongo {

#ifdef MONGO_CONFIG_HAVE_STRNLEN
using ::strnlen;
#else
size_t strnlen(const char* s, size_t maxlen);
#endif

}  // namespace mongo
