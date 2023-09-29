/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/database_holder_impl.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/op_msg_fuzzer_fixture.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/collection_sharding_state_factory_standalone.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_entry_point_mongod.h"
#include "mongo/db/session_manager_mongod.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/version/releases.h"

namespace mongo {

namespace {
constexpr auto kTempDirStem = "op_msg_fuzzer_fixture"_sd;
}

// This must be called before creating any new threads that may access `AuthorizationManager` to
// avoid a data-race.
void OpMsgFuzzerFixture::_setAuthorizationManager() {
    auto localExternalState = std::make_unique<AuthzManagerExternalStateMock>();
    _externalState = localExternalState.get();

    auto localAuthzManager =
        std::make_unique<AuthorizationManagerImpl>(_serviceContext, std::move(localExternalState));
    _authzManager = localAuthzManager.get();
    _externalState->setAuthorizationManager(_authzManager);
    _authzManager->setAuthEnabled(true);

    AuthorizationManager::set(_serviceContext, std::move(localAuthzManager));
}

OpMsgFuzzerFixture::OpMsgFuzzerFixture(bool skipGlobalInitializers)
    : _dir(kTempDirStem.toString()) {
    if (!skipGlobalInitializers) {
        auto ret = runGlobalInitializers(std::vector<std::string>{});
        invariant(ret.isOK());
    }

    setGlobalServiceContext(ServiceContext::make());
    _session = _transportLayer.createSession();

    _serviceContext = getGlobalServiceContext();
    _setAuthorizationManager();
    _serviceContext->getService()->setServiceEntryPoint(
        std::make_unique<ServiceEntryPointMongod>());
    _serviceContext->setSessionManager(std::make_unique<SessionManagerMongod>(_serviceContext));

    auto observerRegistry = std::make_unique<OpObserverRegistry>();
    _serviceContext->setOpObserver(std::move(observerRegistry));

    _serviceContext->setPeriodicRunner(makePeriodicRunner(_serviceContext));

    _clientStrand = ClientStrand::make(_serviceContext->makeClient("test", _session));
    auto clientGuard = _clientStrand->bind();
    auto opCtx = _serviceContext->makeOperationContext(clientGuard.get());

    storageGlobalParams.dbpath = _dir.path();
    storageGlobalParams.engine = "wiredTiger";
    storageGlobalParams.engineSetByUser = true;
    storageGlobalParams.repair = false;
    serverGlobalParams.enableMajorityReadConcern = false;
    // (Generic FCV reference): Initialize FCV.
    serverGlobalParams.mutableFeatureCompatibility.setVersion(multiversion::GenericFCV::kLatest);

    initializeStorageEngine(opCtx.get(),
                            StorageEngineInitFlags::kAllowNoLockFile |
                                StorageEngineInitFlags::kSkipMetadataFile);
    StorageControl::startStorageControls(_serviceContext, true /*forTestOnly*/);

    CollectionShardingStateFactory::set(
        _serviceContext,
        std::make_unique<CollectionShardingStateFactoryStandalone>(_serviceContext));
    DatabaseHolder::set(_serviceContext, std::make_unique<DatabaseHolderImpl>());
    Collection::Factory::set(_serviceContext, std::make_unique<CollectionImpl::FactoryImpl>());

    // Setup the repl coordinator in standalone mode so we don't need an oplog etc.
    repl::ReplicationCoordinator::set(
        _serviceContext,
        std::make_unique<repl::ReplicationCoordinatorMock>(_serviceContext, repl::ReplSettings()));

    _serviceContext->getStorageEngine()->notifyStartupComplete();
}

OpMsgFuzzerFixture::~OpMsgFuzzerFixture() {
    CollectionShardingStateFactory::clear(_serviceContext);

    {
        auto clientGuard = _clientStrand->bind();
        auto opCtx = _serviceContext->makeOperationContext(clientGuard.get());
        Lock::GlobalLock glk(opCtx.get(), MODE_X);
        auto databaseHolder = DatabaseHolder::get(opCtx.get());
        databaseHolder->closeAll(opCtx.get());
    }

    shutdownGlobalStorageEngineCleanly(_serviceContext);
}

int OpMsgFuzzerFixture::testOneInput(const char* Data, size_t Size) {
    if (Size < sizeof(MSGHEADER::Value)) {
        return 0;
    }

    auto clientGuard = _clientStrand->bind();
    auto opCtx = _serviceContext->makeOperationContext(clientGuard.get());
    VectorClockMutable::get(_serviceContext)->tickClusterTimeTo(kInMemoryLogicalTime);

    int new_size = Size + sizeof(int);
    auto sb = SharedBuffer::allocate(new_size);
    memcpy(sb.get(), &new_size, sizeof(int));
    memcpy(sb.get() + sizeof(int), Data, Size);
    Message msg(std::move(sb));

    try {
        _serviceContext->getService()
            ->getServiceEntryPoint()
            ->handleRequest(opCtx.get(), msg)
            .get();
    } catch (const AssertionException&) {
        // We need to catch exceptions caused by invalid inputs
    }

    return 0;
}

}  // namespace mongo
