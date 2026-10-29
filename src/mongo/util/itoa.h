// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * A utility class for performing itoa style integer formatting. This class is highly optimized
 * and only really should be used in hot code paths.
 */
class ItoA {
public:
    // digits10 is 1 less than the maximum number of digits.
    static constexpr size_t kBufSize = std::numeric_limits<std::uint64_t>::digits10 + 1;

    explicit ItoA(std::uint64_t i);
    ItoA(const ItoA&) = delete;
    ItoA& operator=(const ItoA&) = delete;

    std::string toString() const {
        return std::string{_str};
    }

    std::string_view toStringData() const {
        return _str;
    }

    operator std::string_view() const {
        return _str;
    }

private:
    std::string_view _str;
    char _buf[kBufSize];
};

}  // namespace mongo
