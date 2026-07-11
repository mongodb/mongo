// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_shard.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

// The UUID is the word "config" in hex (636f6e66-6967) plus the v4
// required bits.
const UUID ShardType::kConfigServerUuid =
    UUID::parse("636f6e66-6967-4000-8000-000000000000").getValue();

const BSONField<std::string> ShardType::name("_id");
const BSONField<UUID> ShardType::uuid("uuid");
const BSONField<std::string> ShardType::host("host");
const BSONField<bool> ShardType::draining("draining");
const BSONField<BSONArray> ShardType::tags("tags");
// TODO SERVER-116437 Remove once 9.0 becomes last lts.
const BSONField<ShardType::ShardState> ShardType::state("state");
const BSONField<Timestamp> ShardType::topologyTime("topologyTime");
const BSONField<long long> ShardType::replSetConfigVersion("replSetConfigVersion");

ShardType::ShardType(std::string name, std::string host, std::vector<std::string> tags)
    : ShardType(std::move(name), boost::none, std::move(host), std::move(tags)) {}

ShardType::ShardType(std::string name,
                     boost::optional<UUID> uuid,
                     std::string host,
                     std::vector<std::string> tags)
    : _handle(ShardHandle(std::move(name), std::move(uuid))),
      _host(std::move(host)),
      _tags(std::move(tags)) {}

StatusWith<ShardType> ShardType::fromBSON(const BSONObj& source) {
    ShardType shard;

    {
        std::string shardName;
        Status status = bsonExtractStringField(source, name.name(), &shardName);
        if (!status.isOK())
            return status;

        auto swUuid = [&]() -> StatusWith<boost::optional<UUID>> {
            BSONElement uuidElem = source[uuid.name()];
            if (uuidElem.eoo()) {
                return boost::none;
            }

            if (uuidElem.type() != BSONType::binData ||
                uuidElem.binDataType() != BinDataType::newUUID) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "\"" << uuid.name() << "\" must be a UUID");
            }

            return UUID::parse(uuidElem);
        }();

        if (!swUuid.isOK()) {
            return swUuid.getStatus();
        }

        shard._handle.emplace(std::move(shardName), swUuid.getValue());
    }

    {
        std::string shardHost;
        Status status = bsonExtractStringField(source, host.name(), &shardHost);
        if (!status.isOK())
            return status;
        shard._host = shardHost;
    }

    {
        bool isShardDraining;
        Status status = bsonExtractBooleanField(source, draining.name(), &isShardDraining);
        if (status.isOK()) {
            shard._draining = isShardDraining;
        } else if (status == ErrorCodes::NoSuchKey) {
            // draining field can be missing in which case it is presumed false
        } else {
            return status;
        }
    }

    if (source.hasField(tags.name())) {
        shard._tags = std::vector<std::string>();
        BSONElement tagsElement;
        Status status = bsonExtractTypedField(source, tags.name(), BSONType::array, &tagsElement);
        if (!status.isOK())
            return status;

        BSONObjIterator it(tagsElement.Obj());
        while (it.more()) {
            BSONElement tagElement = it.next();
            if (tagElement.type() != BSONType::string) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "Elements in \"" << tags.name()
                                            << "\" array must be strings but found "
                                            << typeName(tagElement.type()));
            }
            shard._tags->push_back(tagElement.String());
        }
    }

    {
        Timestamp shardTopologyTime;
        Status status = bsonExtractTimestampField(source, topologyTime.name(), &shardTopologyTime);
        if (status.isOK()) {
            shard._topologyTime = shardTopologyTime;
        } else if (status == ErrorCodes::NoSuchKey) {
            // topologyTime field can be missing in which case it is presumed to be an
            // uninitialized timestamp
        } else {
            return status;
        }
    }

    {
        long long shardReplSetConfigVersion;
        Status status = bsonExtractIntegerField(
            source, replSetConfigVersion.name(), &shardReplSetConfigVersion);
        if (status.isOK()) {
            shard._replSetConfigVersion = shardReplSetConfigVersion;
        } else if (status == ErrorCodes::NoSuchKey) {
            // replSetConfigVersion field can be missing in which case it is presumed to be
            // uninitialized.
        } else {
            return status;
        }
    }

    return shard;
}

Status ShardType::validate() const {
    if (!_handle.has_value() || !_handle->name().isValid()) {
        return Status(ErrorCodes::NoSuchKey,
                      str::stream() << "missing " << name.name() << " field");
    }

    if (!_host.has_value() || _host->empty()) {
        return Status(ErrorCodes::NoSuchKey,
                      str::stream() << "missing " << host.name() << " field");
    }

    if (_replSetConfigVersion != kUninitializedReplSetConfigVersion &&
        getReplSetConfigVersion() < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "replSetConfigVersion can't be negative");
    }

    return Status::OK();
}

BSONObj ShardType::toBSON() const {
    BSONObjBuilder builder;

    if (_handle) {
        builder.append(name(), getName());
        if (_handle->uuid()) {
            _handle->uuid()->appendToBuilder(&builder, uuid.name());
        }
    }
    if (_host)
        builder.append(host(), getHost());
    if (_draining)
        builder.append(draining(), getDraining());
    if (_tags)
        builder.append(tags(), getTags());
    if (_topologyTime)
        builder.append(topologyTime(), getTopologyTime());
    if (_replSetConfigVersion)
        builder.append(replSetConfigVersion(), getReplSetConfigVersion());

    return builder.obj();
}

std::string ShardType::toString() const {
    return toBSON().toString();
}

const std::string& ShardType::getName() const {
    invariant(_handle);
    return _handle->name().toString();
}

const boost::optional<UUID>& ShardType::getUuid() const {
    invariant(_handle);
    return _handle->uuid();
}

void ShardType::setUuid(boost::optional<UUID> uuid) {
    invariant(_handle);
    ShardHandle newHandle(_handle->name(), std::move(uuid));
    _handle = std::move(newHandle);
}

const ShardHandle& ShardType::getHandle() const {
    invariant(_handle);
    return *_handle;
}

void ShardType::setHandle(ShardHandle handle) {
    _handle = std::move(handle);
}

void ShardType::setHost(const std::string& host) {
    invariant(!host.empty());
    _host = host;
}

void ShardType::setDraining(const bool isDraining) {
    _draining = isDraining;
}

void ShardType::setTags(const std::vector<std::string>& tags) {
    invariant(tags.size() > 0);
    _tags = tags;
}

void ShardType::setTopologyTime(const Timestamp& topologyTime) {
    invariant(!_topologyTime);
    _topologyTime = topologyTime;
}

void ShardType::setReplSetConfigVersion(const long long replSetConfigVersion) {
    invariant(replSetConfigVersion >= 0);
    _replSetConfigVersion = replSetConfigVersion;
}

}  // namespace mongo
