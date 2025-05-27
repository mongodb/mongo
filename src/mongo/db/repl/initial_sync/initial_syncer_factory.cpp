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
