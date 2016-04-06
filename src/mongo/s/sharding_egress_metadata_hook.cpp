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

#include <string>

#include "mongo/base/status.h"
#include "mongo/db/audit.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_egress_metadata_hook.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

namespace rpc {

using std::shared_ptr;

Status ShardingEgressMetadataHook::writeRequestMetadata(bool shardedConnection,
                                                        const StringData target,
                                                        BSONObjBuilder* metadataBob) {
    try {
        audit::writeImpersonatedUsersToMetadata(metadataBob);
        if (!shardedConnection) {
            return Status::OK();
        }
        rpc::ConfigServerMetadata(grid.configOpTime()).writeToMetadata(metadataBob);
        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ShardingEgressMetadataHook::writeRequestMetadata(const HostAndPort& target,
                                                        BSONObjBuilder* metadataBob) {
    try {
        audit::writeImpersonatedUsersToMetadata(metadataBob);
        rpc::ConfigServerMetadata(grid.configOpTime()).writeToMetadata(metadataBob);
        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ShardingEgressMetadataHook::readReplyMetadata(const StringData replySource,
                                                     const BSONObj& metadataObj) {
    try {
        _saveGLEStats(metadataObj, replySource);
        return _advanceConfigOptimeFromShard(replySource.toString(), metadataObj);
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ShardingEgressMetadataHook::readReplyMetadata(const HostAndPort& replySource,
                                                     const BSONObj& metadataObj) {
    try {
        _saveGLEStats(metadataObj, replySource.toString());
        return _advanceConfigOptimeFromShard(replySource.toString(), metadataObj);
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ShardingEgressMetadataHook::_advanceConfigOptimeFromShard(ShardId shardId,
                                                                 const BSONObj& metadataObj) {
    try {
        auto shard = grid.shardRegistry()->getShardNoReload(shardId);
        if (!shard) {
            return Status::OK();
        }

        // Update our notion of the config server opTime from the configOpTime in the response.
        if (shard->isConfig()) {
            // Config servers return the config opTime as part of their own metadata.
            if (metadataObj.hasField(rpc::kReplSetMetadataFieldName)) {
                auto parseStatus = rpc::ReplSetMetadata::readFromMetadata(metadataObj);
                if (!parseStatus.isOK()) {
                    return parseStatus.getStatus();
                }

                const auto& replMetadata = parseStatus.getValue();
                auto opTime = replMetadata.getLastOpVisible();
                grid.advanceConfigOpTime(opTime);
            }
        } else {
            // Regular shards return the config opTime as part of ConfigServerMetadata.
            auto parseStatus = rpc::ConfigServerMetadata::readFromMetadata(metadataObj);
            if (!parseStatus.isOK()) {
                return parseStatus.getStatus();
            }

            const auto& configMetadata = parseStatus.getValue();
            auto opTime = configMetadata.getOpTime();
            if (opTime.is_initialized()) {
                grid.advanceConfigOpTime(opTime.get());
            }
        }
        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

void ShardingEgressMetadataHook::_saveGLEStats(const BSONObj& metadata, StringData hostString) {}

}  // namespace rpc
}  // namespace mongo
