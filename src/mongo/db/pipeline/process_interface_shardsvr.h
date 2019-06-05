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

#pragma once

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface_standalone.h"

namespace mongo {

/**
 * Specialized version of the MongoDInterface when this node is a shard server.
 */
class MongoInterfaceShardServer final : public MongoInterfaceStandalone {
public:
    using MongoInterfaceStandalone::MongoInterfaceStandalone;

    void checkRoutingInfoEpochOrThrow(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const NamespaceString& nss,
                                      ChunkVersion targetCollectionVersion) const final;

    std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFieldsForHostedCollection(
        OperationContext* opCtx, const NamespaceString&, UUID) const final;

    std::vector<FieldPath> collectDocumentKeyFieldsActingAsRouter(
        OperationContext*, const NamespaceString&) const final {
        // We don't expect anyone to use this method on the shard itself (yet). This is currently
        // only used for $merge. For $out in a sharded cluster, the mongos is responsible for
        // collecting the document key fields before serializing them and sending them to the
        // shards. This is logically a MONGO_UNREACHABLE, but a malicious user could construct a
        // request to send directly to the shards which does not include the uniqueKey, so we must
        // be prepared to gracefully error.
        uasserted(50997, "Unexpected attempt to consult catalog cache on a shard server");
    }

    /**
     * Inserts the documents 'objs' into the namespace 'ns' using the ClusterWriter for locking,
     * routing, stale config handling, etc.
     */
    Status insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  const NamespaceString& ns,
                  std::vector<BSONObj>&& objs,
                  const WriteConcernOptions& wc,
                  boost::optional<OID> targetEpoch) final;

    /**
     * Replaces the documents matching 'queries' with 'updates' using the ClusterWriter for locking,
     * routing, stale config handling, etc.
     */
    StatusWith<UpdateResult> update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    const NamespaceString& ns,
                                    BatchedObjects&& batch,
                                    const WriteConcernOptions& wc,
                                    bool upsert,
                                    bool multi,
                                    boost::optional<OID> targetEpoch) final;

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* pipeline) final;
};

}  // namespace mongo
