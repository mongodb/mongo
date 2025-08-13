/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/clone_authoritative_metadata_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"

namespace mongo {

class CloneAuthoritativeMetadataCoordinator final
    : public RecoverableShardingDDLCoordinator<CloneAuthoritativeMetadataCoordinatorDocument,
                                               CloneAuthoritativeMetadataCoordinatorPhaseEnum> {
public:
    using StateDoc = CloneAuthoritativeMetadataCoordinatorDocument;
    using Phase = CloneAuthoritativeMetadataCoordinatorPhaseEnum;

    CloneAuthoritativeMetadataCoordinator(ShardingDDLCoordinatorService* service,
                                          const BSONObj& initialStateDoc)
        : RecoverableShardingDDLCoordinator(
              service, "CloneAuthoritativeMetadataCoordinator", initialStateDoc) {};

    void checkIfOptionsConflict(const BSONObj& stateDoc) const final {}

private:
    StringData serializePhase(const Phase& phase) const override {
        return CloneAuthoritativeMetadataCoordinatorPhase_serializer(phase);
    }

    bool _mustAlwaysMakeProgress() override {
        return _doc.getPhase() >= Phase::kGetDatabasesToClone;
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    /**
     * Retrieves the list of databases that need to be cloned for this shard from the global
     * catalog.
     */
    void _prepareDbsToClone(OperationContext* opCtx);

    /**
     * Clones the metadata for all databases that were marked for cloning to the shard
     * catalog. This function iterates through the list of databases and attempts to clone them
     * individually.
     */
    void _clone(OperationContext* opCtx);

    /**
     * Clones the metadata for a single database while entering in shard role.
     */
    void _cloneSingleDatabaseWithShardRole(OperationContext* opCtx, const DatabaseName& dbName);

    /**
     * Removes a database from the list of databases to be cloned.
     */
    void _removeDbFromCloningList(OperationContext* opCtx, const DatabaseName& dbName);
};

}  // namespace mongo
