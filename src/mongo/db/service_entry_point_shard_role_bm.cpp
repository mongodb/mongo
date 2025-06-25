/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/service_entry_point_shard_role.h"

#include "mongo/db/repl/replication_coordinator_mock.h"
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
        sc->getService(getClusterRole())
            ->setServiceEntryPoint(std::make_unique<ServiceEntryPointShardRole>());
    }

    ClusterRole getClusterRole() const override {
        return ClusterRole::ShardServer;
    }
};

BENCHMARK_DEFINE_F(ServiceEntryPointShardRoleBenchmarkFixture, BM_SEP_PING)
(benchmark::State& state) {
    runBenchmark(state, makePingCommand());
}

BENCHMARK_REGISTER_F(ServiceEntryPointShardRoleBenchmarkFixture, BM_SEP_PING)
    ->ThreadRange(1, kCommandBMMaxThreads);

}  // namespace mongo
