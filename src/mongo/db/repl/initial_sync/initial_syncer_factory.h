/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

namespace MONGO_MOD_PUB mongo {
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
}  // namespace MONGO_MOD_PUB mongo
