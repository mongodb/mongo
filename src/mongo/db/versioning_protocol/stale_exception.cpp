// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/versioning_protocol/stale_exception.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(StaleConfigInfo);
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(StaleEpochInfo);
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(StaleDbRoutingVersion);

}  // namespace

void StaleConfigInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("ns", NamespaceStringUtil::serialize(_nss, SerializationContext::stateDefault()));
    _received.serialize("vReceived", bob);
    if (_wanted)
        _wanted->serialize("vWanted", bob);

    bob->append("shardId", _shardId.toString());
}

std::shared_ptr<const ErrorExtraInfo> StaleConfigInfo::parse(const BSONObj& obj) {
    auto shardId = obj["shardId"].String();
    uassert(ErrorCodes::NoSuchKey, "The shardId field is missing", !shardId.empty());

    return std::make_shared<StaleConfigInfo>(
        NamespaceStringUtil::deserialize(
            boost::none, obj["ns"].String(), SerializationContext::stateDefault()),
        ShardVersion::parse(obj["vReceived"]),
        [&] {
            if (auto vWantedElem = obj["vWanted"])
                return boost::make_optional(ShardVersion::parse(vWantedElem));
            return boost::optional<ShardVersion>();
        }(),
        ShardId(std::move(shardId)));
}

void StaleEpochInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("ns", NamespaceStringUtil::serialize(_nss, SerializationContext::stateDefault()));
    if (_received) {
        _received->serialize("vReceived", bob);
    } else {
        // TODO (SERVER-117117): remove once 9.0 becomes last LTS.
        ShardVersion{}.serialize("vReceived", bob);
    }
    if (_wanted) {
        _wanted->serialize("vWanted", bob);
    } else {
        // TODO (SERVER-117117): remove once 9.0 becomes last LTS.
        ShardVersion{}.serialize("vWanted", bob);
    }
}

std::shared_ptr<const ErrorExtraInfo> StaleEpochInfo::parse(const BSONObj& obj) {
    boost::optional<ShardVersion> received;
    if (auto vReceivedElem = obj["vReceived"])
        received = ShardVersion::parse(vReceivedElem);

    boost::optional<ShardVersion> wanted;
    if (auto vWantedElem = obj["vWanted"])
        wanted = ShardVersion::parse(vWantedElem);

    return std::make_shared<StaleEpochInfo>(
        NamespaceStringUtil::deserialize(
            boost::none, obj["ns"].String(), SerializationContext::stateDefault()),
        received,
        wanted);
}

void StaleDbRoutingVersion::serialize(BSONObjBuilder* bob) const {
    bob->append("db", _db.toStringForErrorMsg());
    bob->append("vReceived", _received.toBSON());
    if (_wanted) {
        bob->append("vWanted", _wanted->toBSON());
    }
}

std::shared_ptr<const ErrorExtraInfo> StaleDbRoutingVersion::parse(const BSONObj& obj) {
    return std::make_shared<StaleDbRoutingVersion>(
        DatabaseNameUtil::deserializeForErrorMsg(obj["db"].String()),
        DatabaseVersion(obj["vReceived"].Obj()),
        !obj["vWanted"].eoo() ? DatabaseVersion(obj["vWanted"].Obj())
                              : boost::optional<DatabaseVersion>{});
}

bool isStaleShardingMetadataError(ErrorCodes::Error errorCode) {
    return errorCode == ErrorCodes::StaleConfig || errorCode == ErrorCodes::StaleDbVersion ||
        errorCode == ErrorCodes::ShardCannotRefreshDueToLocksHeld;
}

}  // namespace mongo
