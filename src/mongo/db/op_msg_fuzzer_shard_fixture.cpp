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

#include "mongo/db/op_msg_fuzzer_shard_fixture.h"

#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/auth/authorization_client_handle_shard.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_factory_mock.h"
#include "mongo/db/auth/authorization_router_impl.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/collection_impl.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/database_holder_impl.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state_factory_shard.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state_factory_shard.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_entry_point_shard_role.h"
#include "mongo/db/session_manager_mongod.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/version/releases.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {
constexpr auto kTempDirStem = "op_msg_fuzzer_fixture"_sd;
}

// This must be called before creating any new threads that may access `AuthorizationManager` to
// avoid a data-race.
void OpMsgFuzzerShardFixture::_setAuthorizationManager() {
    auto globalAuthzManagerFactory = std::make_unique<AuthorizationManagerFactoryMock>();
    AuthorizationManager::set(
        _serviceContext->getService(),
        globalAuthzManagerFactory->createShard(_serviceContext->getService()));
    AuthorizationManager::get(_serviceContext->getService())->setAuthEnabled(true);
}

OpMsgFuzzerShardFixture::OpMsgFuzzerShardFixture(bool skipGlobalInitializers)
    : _dir(std::string{kTempDirStem}) {
    if (!skipGlobalInitializers) {
        auto ret = runGlobalInitializers(std::vector<std::string>{});
        invariant(ret);
    }

    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    setGlobalServiceContext(ServiceContext::make());
    _session = _transportLayer.createSession();

    _serviceContext = getGlobalServiceContext();
    _setAuthorizationManager();
    _serviceContext->getService(ClusterRole::ShardServer)
        ->setServiceEntryPoint(std::make_unique<ServiceEntryPointShardRole>());

    auto observerRegistry = std::make_unique<OpObserverRegistry>();
    _serviceContext->setOpObserver(std::move(observerRegistry));

    _serviceContext->setPeriodicRunner(makePeriodicRunner(_serviceContext));

    _shardStrand = ClientStrand::make(
        _serviceContext->getService(ClusterRole::ShardServer)->makeClient("shardTest", _session));

    auto clientGuard = _shardStrand->bind();

    storageGlobalParams.dbpath = _dir.path();
    storageGlobalParams.engine = "wiredTiger";
    storageGlobalParams.engineSetByUser = true;
    storageGlobalParams.repair = false;
    // (Generic FCV reference): Initialize FCV.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);

    catalog::startUpStorageEngineAndCollectionCatalog(
        _serviceContext,
        clientGuard.get(),
        StorageEngineInitFlags::kAllowNoLockFile | StorageEngineInitFlags::kSkipMetadataFile);
    StorageControl::startStorageControls(_serviceContext, true /*forTestOnly*/);

    ShardingState::create(_serviceContext);
    CollectionShardingStateFactory::set(
        _serviceContext, std::make_unique<CollectionShardingStateFactoryShard>(_serviceContext));
    DatabaseShardingStateFactory::set(_serviceContext,
                                      std::make_unique<DatabaseShardingStateFactoryShard>());
    DatabaseHolder::set(_serviceContext, std::make_unique<DatabaseHolderImpl>());
    Collection::Factory::set(_serviceContext, std::make_unique<CollectionImpl::FactoryImpl>());

    // Setup the repl coordinator in standalone mode so we don't need an oplog etc.
    repl::ReplicationCoordinator::set(
        _serviceContext,
        std::make_unique<repl::ReplicationCoordinatorMock>(_serviceContext, repl::ReplSettings()));

    _serviceContext->getStorageEngine()->notifyStorageStartupRecoveryComplete();
}

OpMsgFuzzerShardFixture::~OpMsgFuzzerShardFixture() {
    CollectionShardingStateFactory::clear(_serviceContext);
    DatabaseShardingStateFactory::clear(_serviceContext);

    {
        auto clientGuard = _shardStrand->bind();
        auto opCtx = _serviceContext->makeOperationContext(clientGuard.get());
        Lock::GlobalLock glk(opCtx.get(), MODE_X);
        auto databaseHolder = DatabaseHolder::get(opCtx.get());
        databaseHolder->closeAll(opCtx.get());
    }

    catalog::shutDownCollectionCatalogAndGlobalStorageEngineCleanly(_serviceContext,
                                                                    true /* memLeakAllowed */);
}

int OpMsgFuzzerShardFixture::testOneInput(const char* Data, size_t Size) {
    if (Size < sizeof(MSGHEADER::Value)) {
        return 0;
    }

    ClientStrand::Guard clientGuard = _shardStrand->bind();

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
            ->handleRequest(opCtx.get(), msg, _serviceContext->getFastClockSource()->now())
            .get();
    } catch (const AssertionException&) {
        // We need to catch exceptions caused by invalid inputs
    }
    return 0;
}

}  // namespace mongo
