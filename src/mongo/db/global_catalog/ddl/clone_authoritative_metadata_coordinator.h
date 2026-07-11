// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/clone_authoritative_metadata_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] CloneAuthoritativeMetadataCoordinator final
    : public RecoverableShardingDDLCoordinator<CloneAuthoritativeMetadataCoordinatorDocument> {
public:
    CloneAuthoritativeMetadataCoordinator(ShardingCoordinatorService* service,
                                          const BSONObj& initialStateDoc)
        : RecoverableShardingDDLCoordinator(
              service, "CloneAuthoritativeMetadataCoordinator", initialStateDoc) {};

    void checkIfOptionsConflict(const BSONObj& stateDoc) const final {}

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
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
    void _clone(OperationContext* opCtx,
                const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                const CancellationToken& token);

    /**
     * Enters the shard role then clones the database and collection metadata for a single database.
     */
    void _cloneSingleDatabaseWithShardRole(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token);

    /**
     * Clones the metadata of config.system.sessions, the only collection of the config database
     * that is allowed to be tracked. The config database is not registered in config.databases, so
     * it is not part of the databases handled by '_clone'; this collection is therefore cloned
     * explicitly. Only invoked on the config server, which is the primary shard of the config
     * database.
     */
    void _cloneConfigSystemSessions(OperationContext* opCtx,
                                    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                                    const CancellationToken& token);

    /**
     * Removes a database from the list of databases to be cloned.
     */
    void _removeDbFromCloningList(OperationContext* opCtx, const DatabaseName& dbName);

    /**
     * Persists the last collection cloned for the current database.
     */
    void _updateLastClonedCollection(OperationContext* opCtx, const NamespaceString& nss);
};

}  // namespace mongo
