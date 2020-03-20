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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/client/sharding_connection_hook.h"

#include <string>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/authenticate.h"
#include "mongo/db/client.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/client/version_manager.h"

namespace mongo {

using std::string;

ShardingConnectionHook::ShardingConnectionHook(bool shardedConnections,
                                               std::unique_ptr<rpc::EgressMetadataHook> egressHook)
    : _shardedConnections(shardedConnections), _egressHook(std::move(egressHook)) {}

void ShardingConnectionHook::onCreate(DBClientBase* conn) {
    if (conn->type() == ConnectionString::INVALID) {
        uasserted(ErrorCodes::BadValue, str::stream() << "Unrecognized connection string.");
    }

    // Authenticate as the first thing we do
    // NOTE: Replica set authentication allows authentication against *any* online host
    if (auth::isInternalAuthSet()) {
        LOGV2_DEBUG(22722,
                    2,
                    "Calling onCreate auth for {connectionString}",
                    "Calling onCreate auth",
                    "connectionString"_attr = conn->toString());

        uassertStatusOKWithContext(conn->authenticateInternalUser(),
                                   str::stream() << "can't authenticate to server "
                                                 << conn->getServerAddress());
    }

    // Delegate the metadata hook logic to the egress hook; use lambdas to pass the arguments in
    // the order expected by the egress hook.
    if (_shardedConnections) {
        conn->setReplyMetadataReader(
            [this](OperationContext* opCtx, const BSONObj& metadataObj, StringData target) {
                return _egressHook->readReplyMetadata(opCtx, target, metadataObj);
            });
    }
    conn->setRequestMetadataWriter([this](OperationContext* opCtx, BSONObjBuilder* metadataBob) {
        return _egressHook->writeRequestMetadata(opCtx, metadataBob);
    });


    if (conn->type() == ConnectionString::MASTER) {
        BSONObj isMasterResponse;
        if (!conn->runCommand("admin", BSON("ismaster" << 1), isMasterResponse)) {
            uassertStatusOK(getStatusFromCommandResult(isMasterResponse));
        }

        long long configServerModeNumber;
        Status status =
            bsonExtractIntegerField(isMasterResponse, "configsvr", &configServerModeNumber);

        if (status == ErrorCodes::NoSuchKey) {
            // This isn't a config server we're talking to.
            return;
        }
        uassertStatusOK(status);

        const long long minKnownConfigServerMode = 1;
        const long long maxKnownConfigServerMode = 2;
        uassert(28785,
                str::stream() << "Unrecognized configsvr mode number: " << configServerModeNumber
                              << ". Range of known configsvr mode numbers is: ["
                              << minKnownConfigServerMode << ", " << maxKnownConfigServerMode
                              << "]",
                configServerModeNumber >= minKnownConfigServerMode &&
                    configServerModeNumber <= maxKnownConfigServerMode);

        uassertStatusOK(status);
    }
}

void ShardingConnectionHook::onDestroy(DBClientBase* conn) {
    if (_shardedConnections && versionManager.isVersionableCB(conn)) {
        versionManager.resetShardVersionCB(conn);
    }
}

void ShardingConnectionHook::onRelease(DBClientBase* conn) {
    // This is currently for making the replica set connections release
    // secondary connections to the pool.
    conn->reset();
}

}  // namespace mongo
