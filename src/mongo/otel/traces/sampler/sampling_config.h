/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

namespace mongo::otel::traces {

/** Configuration for trace sampling. */
struct MONGO_MOD_PUBLIC SamplingConfig {
    /**
     * The sampling factor for spans that are sampled by default. 0.0 means never sampled, while
     * 1.0 means always sampled.
     */
    double defaultFactor = 0.0;

    /**
     * The number of tokens to refill per second in the token bucket of each span that's sampled
     * by default.
     */
    double defaultRefillRate = 1.0;

    /**
     * The maximum number of tokens that can be held in the token bucket of each span that's sampled
     * by default.
     */
    int defaultMaxTokens = 10;

    /**
     * Per-span-name overrides. Values are sampling factors in [0.0, 1.0], with the same semantics
     * as defaultFactor. An entry here takes precedence over defaultFactor for that span name, if
     * applicable.
     */
    StringMap<double> perSpanFactors;
};

}  // namespace mongo::otel::traces
