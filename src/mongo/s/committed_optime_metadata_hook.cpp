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

#include "mongo/s/committed_optime_metadata_hook.h"

#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"

namespace mongo {

namespace rpc {

namespace {
const char kLastCommittedOpTimeFieldName[] = "lastCommittedOpTime";
}

CommittedOpTimeMetadataHook::CommittedOpTimeMetadataHook(ServiceContext* service)
    : _service(service) {}

Status CommittedOpTimeMetadataHook::writeRequestMetadata(OperationContext* opCtx,
                                                         BSONObjBuilder* metadataBob) {
    return Status::OK();
}

Status CommittedOpTimeMetadataHook::readReplyMetadata(OperationContext* opCtx,
                                                      StringData replySource,
                                                      const BSONObj& metadataObj) {
    auto lastCommittedOpTimeField = metadataObj[kLastCommittedOpTimeFieldName];
    if (lastCommittedOpTimeField.eoo()) {
        return Status::OK();
    }

    invariant(lastCommittedOpTimeField.type() == BSONType::bsonTimestamp);

    // replySource is the HostAndPort of a single server, except when this hook is triggered
    // through DBClientReplicaSet, when it will be a replica set connection string. The
    // shardRegistry stores connection strings and hosts in its lookup table, in addition to shard
    // ids, so replySource can be correctly passed on to ShardRegistry::getShardNoReload.
    auto shard = Grid::get(_service)->shardRegistry()->getShardNoReload(replySource.toString());
    if (shard) {
        shard->updateLastCommittedOpTime(LogicalTime(lastCommittedOpTimeField.timestamp()));
    }

    return Status::OK();
}

}  // namespace rpc
}  // namespace mongo
