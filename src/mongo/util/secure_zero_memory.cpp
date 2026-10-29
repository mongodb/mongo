// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/config.h"  // IWYU pragma: keep

#if defined(MONGO_CONFIG_HAVE_MEMSET_S)
#define __STDC_WANT_LIB_EXT1__ 1
#endif

#include "mongo/util/assert_util.h"

#include <cstring>

namespace mongo {

void secureZeroMemory(void* mem, size_t size) {
    if (mem == nullptr) {
        fassert(28751, size == 0);
        return;
    }

#if defined(_WIN32)
    // Windows provides a simple function for zeroing memory
    SecureZeroMemory(mem, size);
#elif defined(MONGO_CONFIG_HAVE_EXPLICIT_BZERO)
    // Gblic 2.25+, OpenBSD 5.5+ and FreeBSD 11.0+ offer explicit_bzero
    explicit_bzero(mem, size);
#elif defined(MONGO_CONFIG_HAVE_MEMSET_S)
    // Some C11 libraries provide a variant of memset which is guaranteed to not be optimized away
    fassert(28752, memset_s(mem, size, 0, size) == 0);
#else
    // fall back to using volatile pointer
    // using volatile to disable compiler optimizations
    volatile char* p = reinterpret_cast<volatile char*>(mem);  // NOLINT
    while (size--) {
        *p++ = 0;
    }
#endif
}

}  // namespace mongo
