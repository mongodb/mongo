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
        auto shard = grid.shardRegistry()->getShardNoReload(target.toString());
        return _writeRequestMetadataForShard(shard, metadataBob);
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ShardingEgressMetadataHook::writeRequestMetadata(const HostAndPort& target,
                                                        BSONObjBuilder* metadataBob) {
    try {
        audit::writeImpersonatedUsersToMetadata(metadataBob);
        auto shard = grid.shardRegistry()->getShardForHostNoReload(target);
        return _writeRequestMetadataForShard(shard, metadataBob);
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ShardingEgressMetadataHook::_writeRequestMetadataForShard(shared_ptr<Shard> shard,
                                                                 BSONObjBuilder* metadataBob) {
    if (!shard) {
        return Status(ErrorCodes::ShardNotFound,
                      str::stream() << "Shard not found for server: " << shard->toString());
    }
    if (shard->isConfig()) {
        return Status::OK();
    }
    rpc::ConfigServerMetadata(grid.configOpTime()).writeToMetadata(metadataBob);
    return Status::OK();
}

Status ShardingEgressMetadataHook::readReplyMetadata(const StringData replySource,
                                                     const BSONObj& metadataObj) {
    try {
        saveGLEStats(metadataObj, replySource);
        auto shard = grid.shardRegistry()->getShardNoReload(replySource.toString());
        return _readReplyMetadataForShard(shard, metadataObj);
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ShardingEgressMetadataHook::readReplyMetadata(const HostAndPort& replySource,
                                                     const BSONObj& metadataObj) {
    try {
        saveGLEStats(metadataObj, replySource.toString());
        auto shard = grid.shardRegistry()->getShardForHostNoReload(replySource);
        return _readReplyMetadataForShard(shard, metadataObj);
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ShardingEgressMetadataHook::_readReplyMetadataForShard(shared_ptr<Shard> shard,
                                                              const BSONObj& metadataObj) {
    try {
        if (!shard) {
            return Status::OK();
        }

        // If this host is a known shard of ours, look for a config server optime in the
        // response metadata to use to update our notion of the current config server optime.
        auto responseStatus = rpc::ConfigServerMetadata::readFromMetadata(metadataObj);
        if (!responseStatus.isOK()) {
            return responseStatus.getStatus();
        }
        auto opTime = responseStatus.getValue().getOpTime();
        if (opTime.is_initialized()) {
            grid.advanceConfigOpTime(opTime.get());
        }
        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

}  // namespace rpc
}  // namespace mongo
