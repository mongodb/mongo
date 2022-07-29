/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/s/stale_exception.h"

#include "mongo/base/init.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(StaleConfigInfo);
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(StaleEpochInfo);
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(StaleDbRoutingVersion);

}  // namespace

void StaleConfigInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("ns", _nss.ns());
    ShardVersion receivedShardVersion(_received);
    receivedShardVersion.serialize("vReceived", bob);
    if (_wanted) {
        ShardVersion wantedShardVersion(*_wanted);
        wantedShardVersion.serialize("vWanted", bob);
    }

    invariant(_shardId != "");
    bob->append("shardId", _shardId.toString());
}

std::shared_ptr<const ErrorExtraInfo> StaleConfigInfo::parse(const BSONObj& obj) {
    auto shardId = obj["shardId"].String();
    uassert(ErrorCodes::NoSuchKey, "The shardId field is missing", !shardId.empty());

    const ChunkVersion& receivedVersion = ShardVersion::parse(obj["vReceived"]);
    return std::make_shared<StaleConfigInfo>(NamespaceString(obj["ns"].String()),
                                             receivedVersion,
                                             [&] {
                                                 if (auto vWantedElem = obj["vWanted"]) {
                                                     const ChunkVersion& wantedVersion =
                                                         ShardVersion::parse(vWantedElem);
                                                     return boost::make_optional(wantedVersion);
                                                 }
                                                 return boost::optional<ChunkVersion>();
                                             }(),
                                             ShardId(std::move(shardId)));
}

void StaleEpochInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("ns", _nss.ns());
}

std::shared_ptr<const ErrorExtraInfo> StaleEpochInfo::parse(const BSONObj& obj) {
    return std::make_shared<StaleEpochInfo>(NamespaceString(obj["ns"].String()));
}

void StaleDbRoutingVersion::serialize(BSONObjBuilder* bob) const {
    bob->append("db", _db);
    bob->append("vReceived", _received.toBSON());
    if (_wanted) {
        bob->append("vWanted", _wanted->toBSON());
    }
}

std::shared_ptr<const ErrorExtraInfo> StaleDbRoutingVersion::parse(const BSONObj& obj) {
    return std::make_shared<StaleDbRoutingVersion>(obj["db"].String(),
                                                   DatabaseVersion(obj["vReceived"].Obj()),
                                                   !obj["vWanted"].eoo()
                                                       ? DatabaseVersion(obj["vWanted"].Obj())
                                                       : boost::optional<DatabaseVersion>{});
}

}  // namespace mongo
