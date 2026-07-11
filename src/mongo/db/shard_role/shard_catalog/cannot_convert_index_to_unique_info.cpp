// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/cannot_convert_index_to_unique_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(CannotConvertIndexToUniqueInfo);

}  // namespace

void CannotConvertIndexToUniqueInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("violations", _violations);
}

std::shared_ptr<const ErrorExtraInfo> CannotConvertIndexToUniqueInfo::parse(const BSONObj& obj) {
    return std::make_shared<CannotConvertIndexToUniqueInfo>(BSONArray(obj["violations"].Obj()));
}

}  // namespace mongo
