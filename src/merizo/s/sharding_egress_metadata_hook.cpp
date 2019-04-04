/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kSharding

#include "merizo/platform/basic.h"

#include <string>

#include "merizo/base/status.h"
#include "merizo/db/service_context.h"
#include "merizo/rpc/metadata/client_metadata_ismaster.h"
#include "merizo/rpc/metadata/config_server_metadata.h"
#include "merizo/rpc/metadata/impersonated_user_metadata.h"
#include "merizo/rpc/metadata/metadata_hook.h"
#include "merizo/rpc/metadata/repl_set_metadata.h"
#include "merizo/s/client/shard_registry.h"
#include "merizo/s/grid.h"
#include "merizo/s/sharding_egress_metadata_hook.h"
#include "merizo/util/net/hostandport.h"

namespace merizo {
namespace rpc {

ShardingEgressMetadataHook::ShardingEgressMetadataHook(ServiceContext* serviceContext)
    : _serviceContext(serviceContext) {
    invariant(_serviceContext);
}

Status ShardingEgressMetadataHook::writeRequestMetadata(OperationContext* opCtx,
                                                        BSONObjBuilder* metadataBob) {
    try {
        writeAuthDataToImpersonatedUserMetadata(opCtx, metadataBob);
        ClientMetadataIsMasterState::writeToMetadata(opCtx, metadataBob);
        rpc::ConfigServerMetadata(_getConfigServerOpTime()).writeToMetadata(metadataBob);
        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ShardingEgressMetadataHook::readReplyMetadata(OperationContext* opCtx,
                                                     StringData replySource,
                                                     const BSONObj& metadataObj) {
    try {
        _saveGLEStats(metadataObj, replySource);
        return _advanceConfigOptimeFromShard(replySource.toString(), metadataObj);
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ShardingEgressMetadataHook::_advanceConfigOptimeFromShard(ShardId shardId,
                                                                 const BSONObj& metadataObj) {
    auto const grid = Grid::get(_serviceContext);

    try {
        auto shard = grid->shardRegistry()->getShardNoReload(shardId);
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
                grid->advanceConfigOpTime(opTime);
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
                grid->advanceConfigOpTime(opTime.get());
            }
        }
        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

}  // namespace rpc
}  // namespace merizo
