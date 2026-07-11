// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/service_entry_point_router_role.h"

#include "mongo/db/service_entry_point_bm_fixture.h"

namespace mongo {

class ServiceEntryPointRouterRoleBenchmarkFixture : public ServiceEntryPointBenchmarkFixture {
public:
    void setUpServiceContext(ServiceContext* sc) override {
        sc->getService()->setServiceEntryPoint(std::make_unique<ServiceEntryPointRouterRole>());
    }

    ClusterRole getClusterRole() const override {
        return ClusterRole::RouterServer;
    }
};

BENCHMARK_DEFINE_F(ServiceEntryPointRouterRoleBenchmarkFixture, BM_SEP_PING)
(benchmark::State& state) {
    runBenchmark(state, [] { return makePingCommand(); });
}

BENCHMARK_REGISTER_F(ServiceEntryPointRouterRoleBenchmarkFixture, BM_SEP_PING)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);

// Needed in the initializers chain, but we don't need its behavior. Make it no-op.
MONGO_INITIALIZER_GENERAL(CoreOptions_Store,
                          ("BeginStartupOptionStorage"),
                          ("EndStartupOptionStorage"))
(InitializerContext* context) {}

}  // namespace mongo
