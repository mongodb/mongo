/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
