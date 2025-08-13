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

#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

const BSONField<std::string> ShardType::name("_id");
const BSONField<std::string> ShardType::host("host");
const BSONField<bool> ShardType::draining("draining");
const BSONField<BSONArray> ShardType::tags("tags");
const BSONField<ShardType::ShardState> ShardType::state("state");
const BSONField<Timestamp> ShardType::topologyTime("topologyTime");
const BSONField<long long> ShardType::replSetConfigVersion("replSetConfigVersion");

ShardType::ShardType(std::string name, std::string host, std::vector<std::string> tags)
    : _name(std::move(name)), _host(std::move(host)), _tags(std::move(tags)) {}

StatusWith<ShardType> ShardType::fromBSON(const BSONObj& source) {
    ShardType shard;

    {
        std::string shardName;
        Status status = bsonExtractStringField(source, name.name(), &shardName);
        if (!status.isOK())
            return status;
        shard._name = shardName;
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
            // draining field can be mssing in which case it is presumed false
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
        long long shardState;
        Status status = bsonExtractIntegerField(source, state.name(), &shardState);
        if (status.isOK()) {
            // Make sure the state field falls within the valid range of ShardState values.
            if (!(shardState >= static_cast<std::underlying_type<ShardState>::type>(
                                    ShardState::kNotShardAware) &&
                  shardState <= static_cast<std::underlying_type<ShardState>::type>(
                                    ShardState::kShardAware))) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Invalid shard state value: " << shardState);
            } else {
                shard._state = static_cast<ShardState>(shardState);
            }
        } else if (status == ErrorCodes::NoSuchKey) {
            // state field can be mssing in which case it is presumed kNotShardAware
        } else {
            return status;
        }
    }

    {
        Timestamp shardTopologyTime;
        Status status = bsonExtractTimestampField(source, topologyTime.name(), &shardTopologyTime);
        if (status.isOK()) {
            shard._topologyTime = shardTopologyTime;
        } else if (status == ErrorCodes::NoSuchKey) {
            // topologyTime field can be mssing in which case it is presumed to be an uninitialized
            // timestamp
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
    if (!_name.has_value() || _name->empty()) {
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

    if (_name)
        builder.append(name(), getName());
    if (_host)
        builder.append(host(), getHost());
    if (_draining)
        builder.append(draining(), getDraining());
    if (_tags)
        builder.append(tags(), getTags());
    if (_state)
        builder.append(state(), static_cast<std::underlying_type<ShardState>::type>(getState()));
    if (_topologyTime)
        builder.append(topologyTime(), getTopologyTime());
    if (_replSetConfigVersion)
        builder.append(replSetConfigVersion(), getReplSetConfigVersion());

    return builder.obj();
}

std::string ShardType::toString() const {
    return toBSON().toString();
}

void ShardType::setName(const std::string& name) {
    _name = name;
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

void ShardType::setState(const ShardState state) {
    invariant(!_state.has_value());
    _state = state;
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
