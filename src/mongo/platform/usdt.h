/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
