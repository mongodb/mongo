// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shutdown_in_progress_quiesce_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(ShutdownInProgressQuiesceInfo);

}  // namespace

void ShutdownInProgressQuiesceInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("remainingQuiesceTimeMillis", _remainingQuiesceTimeMillis);
}

std::shared_ptr<const ErrorExtraInfo> ShutdownInProgressQuiesceInfo::parse(const BSONObj& obj) {
    return std::make_shared<ShutdownInProgressQuiesceInfo>(
        obj["remainingQuiesceTimeMillis"].safeNumberLong());
}

}  // namespace mongo
