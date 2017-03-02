/**
 *    Copyright (C) 2015 BongoDB Inc.
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

#define BONGO_LOG_DEFAULT_COMPONENT ::bongo::logger::LogComponent::kSharding

#include "bongo/platform/basic.h"

#include <string>

#include "bongo/base/status.h"
#include "bongo/db/audit.h"
#include "bongo/db/service_context.h"
#include "bongo/rpc/metadata/client_metadata_ismaster.h"
#include "bongo/rpc/metadata/config_server_metadata.h"
#include "bongo/rpc/metadata/metadata_hook.h"
#include "bongo/rpc/metadata/repl_set_metadata.h"
#include "bongo/s/client/shard_registry.h"
#include "bongo/s/grid.h"
#include "bongo/s/sharding_egress_metadata_hook.h"
#include "bongo/util/net/hostandport.h"

namespace bongo {

namespace rpc {

using std::shared_ptr;

Status ShardingEgressMetadataHook::writeRequestMetadata(bool shardedConnection,
                                                        OperationContext* txn,
                                                        const StringData target,
                                                        BSONObjBuilder* metadataBob) {
    try {
        audit::writeImpersonatedUsersToMetadata(txn, metadataBob);

        ClientMetadataIsMasterState::writeToMetadata(txn, metadataBob);
        if (!shardedConnection) {
            return Status::OK();
        }
        rpc::ConfigServerMetadata(_getConfigServerOpTime()).writeToMetadata(metadataBob);
        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ShardingEgressMetadataHook::writeRequestMetadata(OperationContext* txn,
                                                        const HostAndPort& target,
                                                        BSONObjBuilder* metadataBob) {
    return writeRequestMetadata(true, txn, target.toString(), metadataBob);
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
    return readReplyMetadata(replySource.toString(), metadataObj);
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

                // Use the last committed optime to advance config optime.
                // For successful majority writes, we could use the optime of the last op
                // from us and lastOpCommitted is always greater than or equal to it.
                // On majority write failures, the last visible optime would be incorrect
                // due to rollback as explained in SERVER-24630 and the last committed optime
                // is safe to use.
                const auto& replMetadata = parseStatus.getValue();
                auto opTime = replMetadata.getLastOpCommitted();
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

}  // namespace rpc
}  // namespace bongo
