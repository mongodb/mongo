// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_start_after_invalidate_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(ChangeStreamStartAfterInvalidateInfo);

}  // namespace

std::shared_ptr<const ErrorExtraInfo> ChangeStreamStartAfterInvalidateInfo::parse(
    const BSONObj& obj) {
    return std::make_shared<ChangeStreamStartAfterInvalidateInfo>(
        obj["startAfterInvalidateEvent"].Obj());
}

void ChangeStreamStartAfterInvalidateInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("startAfterInvalidateEvent", _startAfterInvalidateEvent);
}

}  // namespace mongo
