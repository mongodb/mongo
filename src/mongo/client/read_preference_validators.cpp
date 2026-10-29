// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/duration.h"

#include <fmt/format.h>


namespace mongo {

constexpr char kMaxStalenessSecondsFieldName[] = "maxStalenessSeconds";

Status validateMaxStalenessSecondsExternal(const std::int64_t maxStalenessSeconds,
                                           const boost::optional<TenantId>&) {
    if (!maxStalenessSeconds) {
        return Status::OK();
    }

    if (MONGO_unlikely(maxStalenessSeconds < 0)) {
        return Status(
            ErrorCodes::BadValue,
            fmt::format("{} must be a non-negative integer", kMaxStalenessSecondsFieldName));
    } else if (MONGO_unlikely(maxStalenessSeconds >= Seconds::max().count())) {
        return Status(ErrorCodes::BadValue,
                      fmt::format("{} value cannot exceed {}",
                                  kMaxStalenessSecondsFieldName,
                                  Seconds::max().count()));
    } else if (MONGO_unlikely(maxStalenessSeconds <
                              ReadPreferenceSetting::kMinimalMaxStalenessValue.count())) {
        return Status(ErrorCodes::MaxStalenessOutOfRange,
                      fmt::format("{} value cannot be less than {}",
                                  kMaxStalenessSecondsFieldName,
                                  ReadPreferenceSetting::kMinimalMaxStalenessValue.count()));
    }
    return Status::OK();
}

}  // namespace mongo
