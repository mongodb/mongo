// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/prepare_conflict_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

#include <limits>

namespace mongo {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(PrepareConflictInfo);

PrepareConflictInfo::PrepareConflictInfo(uint64_t generation) {
    uassert(ErrorCodes::BadValue,
            "Prepare-conflict generation exceeds the signed 64-bit range",
            generation <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
    _generation = static_cast<int64_t>(generation);
}

void PrepareConflictInfo::serialize(BSONObjBuilder* builder) const {
    builder->append("generation", static_cast<long long>(_generation));
}

std::shared_ptr<const ErrorExtraInfo> PrepareConflictInfo::parse(const BSONObj& obj) {
    const auto generation = obj["generation"].Long();
    uassert(
        ErrorCodes::BadValue, "Prepare-conflict generation must be nonnegative", generation >= 0);
    return std::make_shared<PrepareConflictInfo>(static_cast<uint64_t>(generation));
}

}  // namespace mongo
