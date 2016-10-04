/**
 *    Copyright (C) 2016 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/type_shard_identity.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/s/write_ops/batched_update_document.h"
#include "mongo/s/write_ops/batched_update_request.h"
#include "mongo/util/assert_util.h"

namespace mongo {

const std::string ShardIdentityType::IdName("shardIdentity");

const BSONField<std::string> ShardIdentityType::configsvrConnString("configsvrConnectionString");
const BSONField<std::string> ShardIdentityType::shardName("shardName");
const BSONField<OID> ShardIdentityType::clusterId("clusterId");

StatusWith<ShardIdentityType> ShardIdentityType::fromBSON(const BSONObj& source) {
    if (!source.hasField("_id")) {
        return {ErrorCodes::NoSuchKey,
                str::stream() << "missing _id field for shardIdentity document"};
    }

    ShardIdentityType shardIdentity;

    {
        std::string docId;
        Status status = bsonExtractStringField(source, "_id", &docId);
        if (!status.isOK()) {
            return status;
        }

        if (docId != IdName) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "got _id: " << docId << " instead of " << IdName};
        }
    }

    {
        std::string connString;
        Status status = bsonExtractStringField(source, configsvrConnString(), &connString);
        if (!status.isOK()) {
            return status;
        }

        try {
            // Note: ConnectionString::parse can uassert from HostAndPort constructor.
            auto parsedConfigConnStrStatus = ConnectionString::parse(connString);
            if (!parsedConfigConnStrStatus.isOK()) {
                return parsedConfigConnStrStatus.getStatus();
            }

            auto configSvrConnStr = parsedConfigConnStrStatus.getValue();
            if (configSvrConnStr.type() != ConnectionString::SET) {
                return Status(ErrorCodes::UnsupportedFormat,
                              str::stream()
                                  << "config server connection string can only be replica sets: "
                                  << configSvrConnStr.toString());
            }

            shardIdentity.setConfigsvrConnString(std::move(configSvrConnStr));
        } catch (const UserException& parseException) {
            return parseException.toStatus();
        }
    }

    {
        std::string name;
        Status status = bsonExtractStringField(source, shardName(), &name);
        if (!status.isOK()) {
            return status;
        }

        shardIdentity.setShardName(name);
    }

    {
        OID oid;
        Status status = bsonExtractOIDField(source, clusterId(), &oid);
        if (!status.isOK()) {
            return status;
        }

        shardIdentity.setClusterId(oid);
    }

    return shardIdentity;
}

Status ShardIdentityType::validate() const {
    if (!_configsvrConnString) {
        return {ErrorCodes::NoSuchKey,
                str::stream() << "missing " << configsvrConnString() << " field"};
    }

    if (_configsvrConnString->type() != ConnectionString::SET) {
        return {ErrorCodes::UnsupportedFormat,
                str::stream() << "config connection string can only be replica sets, got "
                              << ConnectionString::typeToString(_configsvrConnString->type())};
    }

    if (!_shardName || _shardName->empty()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << shardName() << " field"};
    }

    if (!_clusterId || !_clusterId->isSet()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << clusterId() << " field"};
    }

    return Status::OK();
}

BSONObj ShardIdentityType::toBSON() const {
    BSONObjBuilder builder;

    builder.append("_id", IdName);

    if (_configsvrConnString) {
        builder << configsvrConnString(_configsvrConnString->toString());
    }

    if (_shardName) {
        builder << shardName(_shardName.get());
    }

    if (_clusterId) {
        builder << clusterId(_clusterId.get());
    }

    return builder.obj();
}

std::string ShardIdentityType::toString() const {
    return toBSON().toString();
}

bool ShardIdentityType::isConfigsvrConnStringSet() const {
    return _configsvrConnString.is_initialized();
}

const ConnectionString& ShardIdentityType::getConfigsvrConnString() const {
    invariant(_configsvrConnString);
    return _configsvrConnString.get();
}

void ShardIdentityType::setConfigsvrConnString(ConnectionString connString) {
    _configsvrConnString = std::move(connString);
}

bool ShardIdentityType::isShardNameSet() const {
    return _shardName.is_initialized();
}

const std::string& ShardIdentityType::getShardName() const {
    invariant(_shardName);
    return _shardName.get();
}

void ShardIdentityType::setShardName(std::string shardName) {
    _shardName = std::move(shardName);
}

bool ShardIdentityType::isClusterIdSet() const {
    return _clusterId.is_initialized();
}

const OID& ShardIdentityType::getClusterId() const {
    invariant(_clusterId);
    return _clusterId.get();
}

void ShardIdentityType::setClusterId(OID clusterId) {
    _clusterId = std::move(clusterId);
}

BSONObj ShardIdentityType::createConfigServerUpdateObject(const std::string& newConnString) {
    BSONObjBuilder builder;
    BSONObjBuilder setConfigBuilder(builder.subobjStart("$set"));
    setConfigBuilder.append(configsvrConnString(), newConnString);
    setConfigBuilder.doneFast();
    return builder.obj();
}

}  // namespace mongo
