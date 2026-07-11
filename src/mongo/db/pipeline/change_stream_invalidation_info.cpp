// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/change_stream_invalidation_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(ChangeStreamInvalidationInfo);

}  // namespace

std::shared_ptr<const ErrorExtraInfo> ChangeStreamInvalidationInfo::parse(const BSONObj& obj) {
    return std::make_shared<ChangeStreamInvalidationInfo>(obj["invalidateToken"].Obj());
}

void ChangeStreamInvalidationInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("invalidateToken", _invalidateToken);
}

}  // namespace mongo
