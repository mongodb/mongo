/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/s/collection_metadata.h"

namespace mongo {

/**
 * OpObserver that forwards operations during migration to the chunk cloner.
 *
 * Contains logic that used to reside in OpObserverImpl that was extracted in SERVER-36084
 * and placed in OpObserverShardingImpl as privately overridden functions separate from the
 * OpObserver methods.
 *
 * This class replaces OpObserverShardingImpl without deriving directly from OpObserverImpl
 * while implementing the standard OpObserver methods.
 *
 * See ShardServerOpObserver.
 */
class MigrationChunkClonerSourceOpObserver : public OpObserverNoop {
public:
    /**
     * Write operations do shard version checking, but if an update operation runs as part of a
     * 'readConcern:snapshot' transaction, the router could have used the metadata at the snapshot
     * time and yet set the latest shard version on the request. This is why the write can get
     * routed to a shard which no longer owns the chunk being written to. In such cases, throw a
     * MigrationConflict exception to indicate that the transaction needs to be rolled-back and
     * restarted.
     */
    static void assertIntersectingChunkHasNotMoved(OperationContext* opCtx,
                                                   const CollectionMetadata& metadata,
                                                   const BSONObj& shardKey,
                                                   const LogicalTime& atClusterTime);

    /**
     * Ensures that there is no movePrimary operation in progress for the given namespace.
     */
    static void assertNoMovePrimaryInProgress(OperationContext* opCtx, const NamespaceString& nss);
};

}  // namespace mongo
