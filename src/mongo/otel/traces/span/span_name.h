// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::otel::traces {

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
    constexpr SpanName(Passkey, std::string_view name) : _name(name) {}

    constexpr std::string_view getName() const {
        return _name;
    }

    constexpr bool operator==(const SpanName& other) const {
        return getName() == other.getName();
    }

private:
    std::string_view _name;
};

}  // namespace mongo::otel::traces
