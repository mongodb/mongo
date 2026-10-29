// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/initial_sync/initial_syncer_interface.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace repl {

/**
 * An object factory for creating InitialSyncer instances via calling registered builders.
 */
class InitialSyncerFactory {
    InitialSyncerFactory(const InitialSyncerFactory&) = delete;
    InitialSyncerFactory& operator=(const InitialSyncerFactory&) = delete;

public:
    InitialSyncerFactory() = default;
    ~InitialSyncerFactory() = default;

    /**
     * Returns a pointer to the initial syncer factory defined within the specified ServiceContext.
     */
    static InitialSyncerFactory* get(ServiceContext* svcCtx);

    /**
     * Registers the new initial syncer factory within the specified ServiceContext.
     */
    static void set(ServiceContext* svcCtx,
                    std::unique_ptr<InitialSyncerFactory> newInitialSyncerFactory);

    using CreateInitialSyncerFunction = std::function<std::shared_ptr<InitialSyncerInterface>(
        InitialSyncerInterface::Options opts,
        std::unique_ptr<DataReplicatorExternalState> dataReplicatorExternalState,
        ThreadPool* workerPool,
        StorageInterface* storage,
        ReplicationProcess* replicationProcess,
        const InitialSyncerInterface::OnCompletionFn& onCompletion)>;

    using InitialSyncCrashRecoveryFunction = std::function<void()>;

    /**
     * Make an InitialSyncer if the initialSyncMethod is "logical", or a FileCopyBasedInitialSyncer
     * if the initialSyncMethod is "fileCopyBased".
     */
    StatusWith<std::shared_ptr<InitialSyncerInterface>> makeInitialSyncer(
        const std::string& initialSyncMethod,
        InitialSyncerInterface::Options opts,
        std::unique_ptr<DataReplicatorExternalState> dataReplicatorExternalState,
        ThreadPool* workerPool,
        StorageInterface* storage,
        ReplicationProcess* replicationProcess,
        const InitialSyncerInterface::OnCompletionFn& onCompletion);

    /**
     * Runs crash recovery for every registered initial syncer type that requires it.
     */
    void runCrashRecovery();

    /**
     * Add a function that creates an initial syncer using the given initialSyncMethod to the
     * _createInitialSyncerFunctionMap.
     */
    void registerInitialSyncer(const std::string& initialSyncMethod,
                               CreateInitialSyncerFunction createInitialSyncerFunction,
                               InitialSyncCrashRecoveryFunction crashRecoveryFunction =
                                   InitialSyncCrashRecoveryFunction());

private:
    StringMap<CreateInitialSyncerFunction> _createInitialSyncerFunctionMap;
    std::vector<InitialSyncCrashRecoveryFunction> _crashRecoveryFunctions;
};
}  // namespace repl
}  // namespace mongo
