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

#include "mongo/s/write_ops/cluster_write.h"

#include <algorithm>

#include "mongo/base/status.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_writes_tracker.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/write_ops/chunk_manager_targeter.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

void toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setStatus(status);
    dassert(response->isValid(NULL));
}

}  // namespace

void ClusterWriter::write(OperationContext* opCtx,
                          const BatchedCommandRequest& request,
                          BatchWriteExecStats* stats,
                          BatchedCommandResponse* response,
                          boost::optional<OID> targetEpoch) {
    const NamespaceString& nss = request.getNS();

    LastError::Disabled disableLastError(&LastError::get(opCtx->getClient()));

    // Config writes and shard writes are done differently
    if (nss.db() == NamespaceString::kAdminDb) {
        Grid::get(opCtx)->catalogClient()->writeConfigServerDirect(opCtx, request, response);
    } else {
        {
            ChunkManagerTargeter targeter(request.getNS(), targetEpoch);

            Status targetInitStatus = targeter.init(opCtx);
            if (!targetInitStatus.isOK()) {
                toBatchError(targetInitStatus.withContext(
                                 str::stream()
                                 << "unable to initialize targeter for write op for collection "
                                 << request.getNS().ns()),
                             response);
                return;
            }

            auto swEndpoints = targeter.targetCollection();
            if (!swEndpoints.isOK()) {
                toBatchError(swEndpoints.getStatus().withContext(
                                 str::stream() << "unable to target write op for collection "
                                               << request.getNS().ns()),
                             response);
                return;
            }

            const auto& endpoints = swEndpoints.getValue();

            // Handle sharded config server writes differently.
            if (std::any_of(endpoints.begin(), endpoints.end(), [](const auto& it) {
                    return it.shardName == ShardRegistry::kConfigServerShardId;
                })) {
                // There should be no namespaces that partially target config servers.
                invariant(endpoints.size() == 1);

                // For config servers, we do direct writes.
                Grid::get(opCtx)->catalogClient()->writeConfigServerDirect(
                    opCtx, request, response);
                return;
            }

            BatchWriteExec::executeBatch(opCtx, targeter, request, response, stats);
        }
    }
}

}  // namespace mongo
