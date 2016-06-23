/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/util/mongoutils/str.h"

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

    auto swConnString = ConnectionString::parse(firstElement.valuestrsafe());
    if (!swConnString.isOK()) {
        return swConnString.getStatus();
    }
    ConnectionString connString = std::move(swConnString.getValue());

    if (connString.type() != ConnectionString::MASTER &&
        connString.type() != ConnectionString::SET) {
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
    std::vector<HostAndPort> serverAddrs = _connString.getServers();
    for (size_t i = 0; i < serverAddrs.size(); i++) {
        if (serverAddrs[i].isLocalHost() != allowLocalHost) {
            string errmsg = str::stream()
                << "Can't use localhost as a shard since all shards need to"
                << " communicate. Either use all shards and configdbs in localhost"
                << " or all in actual IPs. host: " << serverAddrs[i].toString()
                << " isLocalHost:" << serverAddrs[i].isLocalHost();
            return Status(ErrorCodes::InvalidOptions, errmsg);
        }

        // If the server has no port, assign it the default port.
        if (!serverAddrs[i].hasPort()) {
            serverAddrs[i] =
                HostAndPort(serverAddrs[i].host(), ServerGlobalParams::ShardServerPort);
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
