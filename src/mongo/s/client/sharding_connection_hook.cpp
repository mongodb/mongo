/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/client/sharding_connection_hook.h"

#include <string>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/client.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/audit_metadata.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/s/client/scc_fast_query_handler.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/grid.h"
#include "mongo/s/version_manager.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;

namespace {

// A hook that parses the reply metadata from every response to a command sent from a DBClient
// created by mongos or a sharding aware mongod and being used for sharded operations.
// Used by mongos to capture the GLE stats so that we can target the correct node when subsequent
// getLastError calls are made, as well as by both mongod and mongos to update the stored config
// server optime.
Status _shardingReplyMetadataReader(const BSONObj& metadataObj, StringData hostString) {
    saveGLEStats(metadataObj, hostString);

    auto shard = grid.shardRegistry()->getShardNoReload(hostString.toString());
    if (!shard) {
        return Status::OK();
    }
    // If this host is a known shard of ours, look for a config server optime in the response
    // metadata to use to update our notion of the current config server optime.
    auto responseStatus = rpc::ConfigServerMetadata::readFromMetadata(metadataObj);
    if (!responseStatus.isOK()) {
        return responseStatus.getStatus();
    }
    auto opTime = responseStatus.getValue().getOpTime();
    if (opTime.is_initialized()) {
        grid.shardRegistry()->advanceConfigOpTime(opTime.get());
    }
    return Status::OK();
}

// A hook that will append impersonated users to the metadata of every runCommand run by a DBClient
// created by mongos or a sharding aware mongod.  mongos uses this information to send information
// to mongod so that the mongod can produce auditing records attributed to the proper authenticated
// user(s).
// Additionally, if the connection is sharding-aware, also appends the stored config server optime.
Status _shardingRequestMetadataWriter(bool shardedConn,
                                      BSONObjBuilder* metadataBob,
                                      StringData hostStringData) {
    audit::writeImpersonatedUsersToMetadata(metadataBob);
    if (!shardedConn) {
        return Status::OK();
    }

    // Add config server optime to metadata sent to shards.
    std::string hostString = hostStringData.toString();
    auto shard = grid.shardRegistry()->getShardNoReload(hostString);
    if (!shard) {
        return Status(ErrorCodes::ShardNotFound,
                      str::stream() << "Shard not found for server: " << hostString);
    }
    if (shard->isConfig()) {
        return Status::OK();
    }
    rpc::ConfigServerMetadata(grid.shardRegistry()->getConfigOpTime()).writeToMetadata(metadataBob);

    return Status::OK();
}

}  // namespace

ShardingConnectionHook::ShardingConnectionHook(bool shardedConnections)
    : _shardedConnections(shardedConnections) {}

void ShardingConnectionHook::onCreate(DBClientBase* conn) {
    // Authenticate as the first thing we do
    // NOTE: Replica set authentication allows authentication against *any* online host
    if (getGlobalAuthorizationManager()->isAuthEnabled()) {
        LOG(2) << "calling onCreate auth for " << conn->toString();

        bool result = conn->authenticateInternalUser();

        uassert(15847,
                str::stream() << "can't authenticate to server " << conn->getServerAddress(),
                result);
    }

    if (_shardedConnections) {
        conn->setReplyMetadataReader(_shardingReplyMetadataReader);
    }

    conn->setRequestMetadataWriter(
        [this](BSONObjBuilder* metadataBob, StringData hostStringData) -> Status {
            return _shardingRequestMetadataWriter(_shardedConnections, metadataBob, hostStringData);
        });

    if (conn->type() == ConnectionString::SYNC) {
        throw UserException(ErrorCodes::UnsupportedFormat,
                            str::stream() << "Unrecognized connection string type: " << conn->type()
                                          << ".");
    }

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

        uassert(28785,
                str::stream() << "Unrecognized configsvr version number: " << configServerModeNumber
                              << ". Expected either 0 or 1",
                configServerModeNumber == 0 || configServerModeNumber == 1);

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
