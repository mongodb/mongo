// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/config.h"

namespace mongo {

#ifdef MONGO_CONFIG_USDT_ENABLED

#if MONGO_CONFIG_USDT_PROVIDER == SDT
#include <sys/sdt.h>
#endif

#define MONGO_USDT_SUFFIX_(...) \
    MONGO_USDT_SUFFIX_I_(__VA_ARGS__, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, )

#define MONGO_USDT_SUFFIX_I_(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, N, ...) N

#define MONGO_USDT_CONCAT_(a, b) MONGO_USDT_CONCAT_I_(a, b)

#define MONGO_USDT_CONCAT_I_(a, b) a##b

/**
 * Create a USDT probe with a symbol defined by the first argument. Between 0 and 12 additional
 * parameters can be optionally passed to the probe.
 *
 * Example usage:
 * MONGO_USDT(probe1); // symbol is probe1, no additional arguments passed to probe
 * MONGO_USDT(probe2, arg1, arg2, arg3); // symbol is probe2, passing 3 arguments to probe
 */
#define MONGO_USDT(...) \
    MONGO_USDT_CONCAT_(DTRACE_PROBE, MONGO_USDT_SUFFIX_(__VA_ARGS__))(mongodb, __VA_ARGS__)

#else

#define MONGO_USDT(...)

#endif  // MONGO_CONFIG_USDT_ENABLED

}  // namespace mongo
