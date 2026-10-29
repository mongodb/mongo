// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/service_entry_point_shard_role.h"

#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/rss/attached_storage/attached_persistence_provider.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_bm_fixture.h"

namespace mongo {

class ServiceEntryPointShardRoleBenchmarkFixture : public ServiceEntryPointBenchmarkFixture {
public:
    void setUpServiceContext(ServiceContext* sc) override {
        auto replCoordMock = std::make_unique<repl::ReplicationCoordinatorMock>(sc);
        // Transition to primary so that the server can accept writes.
        invariant(replCoordMock->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(sc, std::move(replCoordMock));
        sc->getService()->setServiceEntryPoint(std::make_unique<ServiceEntryPointShardRole>());

        auto persistenceProvider = std::make_unique<rss::AttachedPersistenceProvider>();
        rss::ReplicatedStorageService::get(getGlobalServiceContext())
            .setPersistenceProvider(std::move(persistenceProvider));
    }

    ClusterRole getClusterRole() const override {
        return ClusterRole::ShardServer;
    }
};

BENCHMARK_DEFINE_F(ServiceEntryPointShardRoleBenchmarkFixture, BM_SEP_PING)
(benchmark::State& state) {
    runBenchmark(state, [] { return makePingCommand(); });
}

BENCHMARK_REGISTER_F(ServiceEntryPointShardRoleBenchmarkFixture, BM_SEP_PING)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);

}  // namespace mongo
