// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
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
