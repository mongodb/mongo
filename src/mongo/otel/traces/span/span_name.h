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

#include <string_view>

namespace mongo::otel::traces {

/**
 * Indicates whether a span can initiate a sampled trace by default assuming no other configuration
 * on this specific span.
 *
 * `true` means this span is an entry point to an operation we always want to trace (e.g., the
 * root span of a tracked operation). Only a small number of spans should be `true` — typically
 * only the outermost entry points to operations we want to gather traces for by default. Child
 * spans within a sampled trace are captured automatically regardless of this value.
 *
 * `false` means a trace is never sampled due to the inclusion of this span (but could be via other
 * means).
 */
enum class SampledByDefault : bool {};

/** Wrapper class around a string to ensure `SpanName`s are only constructed in certain places. */
class [[MONGO_MOD_PUBLIC]] SpanName {
private:
    class Passkey {
        friend class SpanName;
        explicit constexpr Passkey() = default;
    };

public:
    /** N&O team must own all uses of this passkey variable. */
    [[MONGO_MOD_PRIVATE]] static constexpr Passkey passkeyForNetworkingAndObservabilityOnly{};

    /**
     * Note that this requires a passkey for construction, which only N&O code is allowed to use.
     */
    constexpr SpanName(Passkey, std::string_view name, SampledByDefault sampledByDefault)
        : _name(name), _sampledByDefault{sampledByDefault} {}

    constexpr std::string_view getName() const {
        return _name;
    }

    constexpr SampledByDefault getSampledByDefault() const {
        return _sampledByDefault;
    }

    constexpr bool operator==(const SpanName& other) const {
        return getName() == other.getName();
    }

private:
    std::string_view _name;
    SampledByDefault _sampledByDefault;
};

}  // namespace mongo::otel::traces
