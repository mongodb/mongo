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

#include "mongo/db/s/type_shard_identity.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"

namespace mongo {

const std::string ShardIdentityType::IdName("shardIdentity");

StatusWith<ShardIdentityType> ShardIdentityType::fromShardIdentityDocument(const BSONObj& source) {
    if (!source.hasField("_id")) {
        return {ErrorCodes::NoSuchKey,
                str::stream() << "missing _id field for shardIdentity document"};
    }
    // Strip the id field since it's always the same and we don't store it
    auto shardIdentityBSON = source.removeField("_id");

    try {
        ShardIdentityType shardIdentity =
            ShardIdentity::parse(IDLParserContext("ShardIdentity"), shardIdentityBSON);

        const auto& configsvrConnStr = shardIdentity.getConfigsvrConnectionString();
        if (configsvrConnStr.type() != ConnectionString::ConnectionType::kReplicaSet) {
            return Status(ErrorCodes::UnsupportedFormat,
                          str::stream()
                              << "config server connection string can only be replica sets: "
                              << configsvrConnStr.toString());
        }

        return shardIdentity;
    } catch (const AssertionException& parseException) {
        return parseException.toStatus();
    }
}

Status ShardIdentityType::validate() const {
    const auto& configsvrConnStr = getConfigsvrConnectionString();
    if (configsvrConnStr.type() != ConnectionString::ConnectionType::kReplicaSet) {
        return {ErrorCodes::UnsupportedFormat,
                str::stream() << "config connection string can only be replica sets, got "
                              << ConnectionString::typeToString(configsvrConnStr.type())};
    }
    return Status::OK();
}

BSONObj ShardIdentityType::toShardIdentityDocument() const {
    BSONObjBuilder builder;
    builder.append("_id", ShardIdentityType::IdName);
    builder.appendElements(ShardIdentity::toBSON());
    return builder.obj();
}

std::string ShardIdentityType::toString() const {
    return toBSON().toString();
}

BSONObj ShardIdentityType::createConfigServerUpdateObject(const std::string& newConnString) {
    BSONObjBuilder builder;
    BSONObjBuilder setConfigBuilder(builder.subobjStart("$set"));
    setConfigBuilder.append(ShardIdentity::kConfigsvrConnectionStringFieldName, newConnString);
    setConfigBuilder.doneFast();
    return builder.obj();
}

}  // namespace mongo
