// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/cursor_in_use_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(CursorInUseInfo);

}  // namespace

void CursorInUseInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("commandName", _commandName);
}

std::shared_ptr<const ErrorExtraInfo> CursorInUseInfo::parse(const BSONObj& obj) {
    return std::make_shared<CursorInUseInfo>(obj["commandName"].checkAndGetStringData());
}

}  // namespace mongo
