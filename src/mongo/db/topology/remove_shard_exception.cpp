// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/remove_shard_exception.h"

#include "mongo/base/init.h"  // IWYU pragma: keep

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(RemoveShardDrainingInfo);

}  // namespace

void RemoveShardDrainingInfo::serialize(BSONObjBuilder* bob) const {
    _progress.serialize(bob);
}

std::shared_ptr<const ErrorExtraInfo> RemoveShardDrainingInfo::parse(const BSONObj& obj) {
    return std::make_shared<RemoveShardDrainingInfo>(
        RemoveShardProgress::parse(obj, IDLParserContext("RemoveShardDrainingInfo")));
}

}  // namespace mongo
