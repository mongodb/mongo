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

#include "mongo/db/global_catalog/type_shard_identity.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

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
            ShardIdentity::parse(shardIdentityBSON, IDLParserContext("ShardIdentity"));

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

Status ShardIdentityType::validate(bool fassert) const {
    const auto& configsvrConnStr = getConfigsvrConnectionString();
    if (configsvrConnStr.type() != ConnectionString::ConnectionType::kReplicaSet) {
        return {ErrorCodes::UnsupportedFormat,
                str::stream() << "config connection string can only be replica sets, got "
                              << ConnectionString::typeToString(configsvrConnStr.type())};
    }

    // (Ignore FCV check): Auto-bootstrapping happens irrespective of the FCV when
    // gFeatureFlagAllMongodsAreSharded is enabled.
    if (gFeatureFlagAllMongodsAreSharded.isEnabledAndIgnoreFCVUnsafe() && fassert) {
        // With auto-bootstrapping, we rely on detecting a discrepancy between a server's cluster
        // role and the shard identity document to prevent a replica set from running with mixed
        // cluster roles. See SERVER-80249 for more information.
        const bool isShardIdConfigServer = getShardName() == ShardId::kConfigServerId;
        if (!isShardIdConfigServer &&
            serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            LOGV2_FATAL_NOTRACE(8024901,
                                "Shard identity document for a shard server was detected, but the "
                                "server is not a dedicated shard server. To fix this, restart this "
                                "server with --shardsvr.");
        } else if (isShardIdConfigServer && serverGlobalParams.clusterRole.isShardOnly()) {
            LOGV2_FATAL_NOTRACE(8024902,
                                "Shard identity document for a config server was detected, but the "
                                "server is not a config server. To fix this, restart this server "
                                "without a cluster role or with --configsvr.");
        }
    } else {
        if (getShardName() == ShardId::kConfigServerId &&
            !serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            return {ErrorCodes::UnsupportedFormat,
                    str::stream() << "Invalid shard identity document: the shard name for a shard "
                                     "server cannot be \""
                                  << ShardId::kConfigServerId.toString() << "\""};
        }

        if (getShardName() != ShardId::kConfigServerId &&
            serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            return {ErrorCodes::UnsupportedFormat,
                    str::stream() << "Invalid shard identity document: the shard name for a config "
                                     "server cannot be \""
                                  << getShardName() << "\""};
        }
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
