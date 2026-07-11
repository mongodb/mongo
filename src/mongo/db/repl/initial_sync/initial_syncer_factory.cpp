// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/initial_sync/initial_syncer_factory.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <utility>

namespace mongo {
namespace repl {

const auto serviceInitialSyncerFactory =
    ServiceContext::declareDecoration<std::unique_ptr<InitialSyncerFactory>>();

ServiceContext::ConstructorActionRegisterer initialSyncerFactoryRegisterer{
    "InitialSyncerFactoryRegisterer", [](ServiceContext* svcCtx) {
        auto initialSyncerFactory = std::make_unique<InitialSyncerFactory>();
        InitialSyncerFactory::set(svcCtx, std::move(initialSyncerFactory));
    }};


InitialSyncerFactory* InitialSyncerFactory::get(ServiceContext* svcCtx) {
    return serviceInitialSyncerFactory(svcCtx).get();
}

void InitialSyncerFactory::set(ServiceContext* svcCtx,
                               std::unique_ptr<InitialSyncerFactory> newInitialSyncerFactory) {
    invariant(newInitialSyncerFactory);
    auto& initialSyncerFactory = serviceInitialSyncerFactory(svcCtx);
    initialSyncerFactory = std::move(newInitialSyncerFactory);
}

StatusWith<std::shared_ptr<InitialSyncerInterface>> InitialSyncerFactory::makeInitialSyncer(
    const std::string& initialSyncMethod,
    InitialSyncerInterface::Options opts,
    std::unique_ptr<DataReplicatorExternalState> dataReplicatorExternalState,
    ThreadPool* workerPool,
    StorageInterface* storage,
    ReplicationProcess* replicationProcess,
    const InitialSyncerInterface::OnCompletionFn& onCompletion) {
    auto fn = _createInitialSyncerFunctionMap.find(initialSyncMethod);
    if (fn == _createInitialSyncerFunctionMap.end()) {
        return {ErrorCodes::NotImplemented,
                str::stream() << "The initial sync method " << initialSyncMethod
                              << " is not available."};
    }
    return fn->second(opts,
                      std::move(dataReplicatorExternalState),
                      workerPool,
                      storage,
                      replicationProcess,
                      onCompletion);
}

void InitialSyncerFactory::runCrashRecovery() {
    // We run crash recovery for all initial sync classes, regardless of which one we are configured
    // to use.  Only one of them should actually do anything.
    for (auto& crashRecoveryFunction : _crashRecoveryFunctions) {
        crashRecoveryFunction();
    }
}

void InitialSyncerFactory::registerInitialSyncer(
    const std::string& initialSyncMethod,
    CreateInitialSyncerFunction createInitialSyncerFunction,
    InitialSyncCrashRecoveryFunction crashRecoveryFunction) {
    invariant(_createInitialSyncerFunctionMap.find(initialSyncMethod) ==
              _createInitialSyncerFunctionMap.end());
    _createInitialSyncerFunctionMap[initialSyncMethod] = createInitialSyncerFunction;

    // Not all initial syncers have a crash recovery function.
    if (crashRecoveryFunction) {
        _crashRecoveryFunctions.emplace_back(crashRecoveryFunction);
    }
}
}  // namespace repl
}  // namespace mongo
