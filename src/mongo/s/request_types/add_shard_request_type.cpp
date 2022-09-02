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

#include "mongo/platform/basic.h"

#include "mongo/s/request_types/add_shard_request_type.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/server_options.h"
#include "mongo/util/str.h"

namespace mongo {

using std::string;
using str::stream;

class BSONObj;
template <typename T>
class StatusWith;

const BSONField<std::string> AddShardRequest::mongosAddShard("addShard");
const BSONField<std::string> AddShardRequest::mongosAddShardDeprecated("addshard");
const BSONField<std::string> AddShardRequest::configsvrAddShard("_configsvrAddShard");
const BSONField<std::string> AddShardRequest::shardName("name");
const BSONField<long long> AddShardRequest::maxSizeMB("maxSize");

namespace {
const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kWriteConcernTimeoutSharding);
}

AddShardRequest::AddShardRequest(ConnectionString connString)
    : _connString(std::move(connString)) {}

StatusWith<AddShardRequest> AddShardRequest::parseFromMongosCommand(const BSONObj& obj) {
    invariant(obj.nFields() >= 1);
    invariant(mongosAddShard.name() == obj.firstElement().fieldNameStringData() ||
              mongosAddShardDeprecated.name() == obj.firstElement().fieldNameStringData());
    return parseInternalFields(obj);
}

StatusWith<AddShardRequest> AddShardRequest::parseFromConfigCommand(const BSONObj& obj) {
    invariant(obj.nFields() >= 1);
    invariant(configsvrAddShard.name() == obj.firstElement().fieldNameStringData());
    return parseInternalFields(obj);
}

StatusWith<AddShardRequest> AddShardRequest::parseInternalFields(const BSONObj& obj) {
    // required fields

    auto firstElement = obj.firstElement();
    if (firstElement.type() != BSONType::String) {
        return {ErrorCodes::FailedToParse,
                stream() << "The first argument to " << firstElement.fieldNameStringData()
                         << " must be a string"};
    }

    auto swConnString = ConnectionString::parse(firstElement.str());
    if (!swConnString.isOK()) {
        return swConnString.getStatus();
    }
    ConnectionString connString = std::move(swConnString.getValue());

    if (connString.type() != ConnectionString::ConnectionType::kStandalone &&
        connString.type() != ConnectionString::ConnectionType::kReplicaSet) {
        return {ErrorCodes::FailedToParse,
                stream() << "Invalid connection string " << connString.toString()};
    }

    AddShardRequest request(std::move(connString));

    // optional fields

    {
        string requestShardName;
        Status status = bsonExtractStringField(obj, shardName.name(), &requestShardName);
        if (status.isOK()) {
            request._name = std::move(requestShardName);
        } else if (status != ErrorCodes::NoSuchKey) {
            return status;
        }
    }
    {
        long long requestMaxSizeMB;
        Status status = bsonExtractIntegerField(obj, maxSizeMB.name(), &requestMaxSizeMB);
        if (status.isOK()) {
            request._maxSizeMB = std::move(requestMaxSizeMB);
        } else if (status != ErrorCodes::NoSuchKey) {
            return status;
        }
    }

    return request;
}

BSONObj AddShardRequest::toCommandForConfig() {
    BSONObjBuilder cmdBuilder;
    cmdBuilder.append(configsvrAddShard.name(), _connString.toString());
    if (hasMaxSize()) {
        cmdBuilder.append(maxSizeMB.name(), *_maxSizeMB);
    }
    if (hasName()) {
        cmdBuilder.append(shardName.name(), *_name);
    }
    return cmdBuilder.obj();
}

Status AddShardRequest::validate(bool allowLocalHost) {
    // Check that if one of the new shard's hosts is localhost, we are allowed to use localhost
    // as a hostname. (Using localhost requires that every server in the cluster uses
    // localhost).
    for (const auto& serverAddr : _connString.getServers()) {
        if (serverAddr.isLocalHost() != allowLocalHost) {
            string errmsg = str::stream()
                << "Can't use localhost as a shard since all shards need to"
                << " communicate. Either use all shards and configdbs in localhost"
                << " or all in actual IPs. host: " << serverAddr.toString()
                << " isLocalHost:" << serverAddr.isLocalHost();
            return Status(ErrorCodes::InvalidOptions, errmsg);
        }
    }
    return Status::OK();
}

string AddShardRequest::toString() const {
    stream ss;
    ss << "AddShardRequest shard: " << _connString.toString();
    if (hasName())
        ss << ", name: " << *_name;
    if (hasMaxSize())
        ss << ", maxSize: " << *_maxSizeMB;
    return ss;
}

}  // namespace mongo
