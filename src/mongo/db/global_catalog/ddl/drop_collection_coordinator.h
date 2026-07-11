// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/ddl/drop_collection_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/shard_role/shard_catalog/drop_collection.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] DropCollectionCoordinator final
    : public RecoverableShardingDDLCoordinator<DropCollectionCoordinatorDocument> {
public:
    DropCollectionCoordinator(ShardingCoordinatorService* service, const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(service, "DropCollectionCoordinator", initialState),
          _critSecReason(BSON("command"
                              << "dropCollection"
                              << "ns"
                              << NamespaceStringUtil::serialize(
                                     originalNss(), SerializationContext::stateDefault()))) {}

    ~DropCollectionCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& doc) const final {}

    /**
     * Locally drops a collection, cleans its CollectionShardingRuntime metadata and, when
     * forceLegacyRefresh is true, refreshes the filtering metadata cache.
     *
     * When fromMigrate is set, the related oplog entry will be marked with a 'fromMigrate' field to
     * reduce its visibility.
     *
     * When dropSystemCollections is set, system collections are allowed to be dropped. Therefore,
     * if nss is a system collection but dropSystemCollections is false, the drop will fail.
     *
     * If expectedUUID is set and doesn't match the value persisted on the CollectionCatalog, then
     * this is a no-op. If expectedUUID is not set, no UUID check will be performed.
     *
     * If requireCollectionEmpty is set to true and the collection has records, this is a no-op.
     */
    static void dropCollectionLocally(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      bool fromMigrate,
                                      bool dropSystemCollections,
                                      bool forceLegacyRefresh,
                                      const boost::optional<UUID>& expectedUUID = boost::none,
                                      bool requireCollectionEmpty = false);

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
    const BSONObj _critSecReason;

    bool _mustAlwaysMakeProgress() override {
        return _doc.getPhase() > Phase::kUnset;
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    void _checkPreconditionsAndSaveArgumentsOnDoc();

    void _freezeMigrations(OperationContext* opCtx,
                           std::shared_ptr<executor::ScopedTaskExecutor> executor);

    void _enterCriticalSection(OperationContext* opCtx,
                               std::shared_ptr<executor::ScopedTaskExecutor> executor,
                               const CancellationToken& token);

    void _commitDropCollection(OperationContext* opCtx,
                               std::shared_ptr<executor::ScopedTaskExecutor> executor,
                               const CancellationToken& token);

    void _exitCriticalSection(OperationContext* opCtx,
                              std::shared_ptr<executor::ScopedTaskExecutor> executor,
                              const CancellationToken& token);
};

}  // namespace mongo
