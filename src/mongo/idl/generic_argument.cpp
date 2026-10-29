// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/idl/generic_argument.h"

#include "mongo/base/error_codes.h"

#include <limits>

#include <fmt/format.h>

namespace mongo {

Status validateMaxTimeMSOpOnly(std::int64_t val) {
    constexpr std::int64_t kMaxTimeMSOpOnlyMax = std::numeric_limits<std::int32_t>::max() + 100LL;

    if (val < 0 || val > kMaxTimeMSOpOnlyMax) {
        return Status(ErrorCodes::BadValue,
                      fmt::format("{} value for {} is out of range [{}, {}]",
                                  val,
                                  "maxTimeMSOpOnly",
                                  0,
                                  kMaxTimeMSOpOnlyMax));
    }
    return Status::OK();
}

}  // namespace mongo
