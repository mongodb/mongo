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

#include <benchmark/benchmark.h>

#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role.h"
#include "mongo/logv2/log_domain_global.h"

namespace mongo {
namespace {
using namespace mongo::repl;
MONGO_INITIALIZER_GENERAL(CoreOptions_Store, (), ())
(InitializerContext* context) {
    // Dummy initializer to fill in the initializer graph
}

MONGO_INITIALIZER_GENERAL(DisableLogging, (), ())
(InitializerContext*) {
    auto& lv2Manager = logv2::LogManager::global();
    logv2::LogDomainGlobal::ConfigurationOptions lv2Config;
    lv2Config.makeDisabled();
    uassertStatusOK(lv2Manager.getGlobalDomainInternal().configure(lv2Config));
}


class CollectionAcquisitionBenchmark : public ServiceContextMongoDTest {
public:
    CollectionAcquisitionBenchmark() : ServiceContextMongoDTest() {
        setUp();
    }

    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

    ~CollectionAcquisitionBenchmark() override {
        tearDown();
    }

private:
    void _doTest() override{};

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _opCtx = cc().makeOperationContext();
        auto replCoord = std::make_unique<ReplicationCoordinatorMock>(getServiceContext());
        ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));
    }

    void tearDown() override {
        _opCtx.reset(nullptr);
        ServiceContextMongoDTest::tearDown();
    }

    ServiceContext::UniqueOperationContext _opCtx;
};

void BM_acquireCollectionLockFree(benchmark::State& state) {
    CollectionAcquisitionBenchmark fixture;
    auto opCtx = fixture.getOperationContext();

    const auto nss = NamespaceString::createNamespaceString_forTest("test.test");
    const auto readConcern = repl::ReadConcernArgs::fromBSONThrows(repl::ReadConcernArgs::kLocal);
    for (auto _ : state) {
        // Building the request is part of the work required to acquire a collection, so we include
        // this in the benchmark.
        CollectionAcquisitionRequest request{
            nss,
            PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
            readConcern,
            AcquisitionPrerequisites::kRead};
        auto acquisition = acquireCollectionMaybeLockFree(opCtx, request);
        benchmark::DoNotOptimize(acquisition);
    }
}

void BM_acquireCollection(benchmark::State& state) {
    CollectionAcquisitionBenchmark fixture;
    auto opCtx = fixture.getOperationContext();

    const auto nss = NamespaceString::createNamespaceString_forTest("test.test");
    const auto readConcern = repl::ReadConcernArgs::fromBSONThrows(repl::ReadConcernArgs::kLocal);
    for (auto _ : state) {
        // Building the request is part of the work required to acquire a collection, so we include
        // this in the benchmark.
        CollectionAcquisitionRequest request{
            nss,
            PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
            readConcern,
            AcquisitionPrerequisites::kRead};
        auto acquisition = acquireCollection(opCtx, request, MODE_IS);
        benchmark::DoNotOptimize(acquisition);
    }
}

void BM_acquireMultiCollection(benchmark::State& state) {
    CollectionAcquisitionBenchmark fixture;
    auto opCtx = fixture.getOperationContext();

    const auto nss1 = NamespaceString::createNamespaceString_forTest("test.test1");
    const auto nss2 = NamespaceString::createNamespaceString_forTest("test.test2");
    const auto readConcern = repl::ReadConcernArgs::fromBSONThrows(repl::ReadConcernArgs::kLocal);
    for (auto _ : state) {
        // Building the request is part of the work required to acquire a collection, so we include
        // this in the benchmark.
        auto requests = {
            CollectionAcquisitionRequest{nss1,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         readConcern,
                                         AcquisitionPrerequisites::kRead},
            CollectionAcquisitionRequest{nss2,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         readConcern,
                                         AcquisitionPrerequisites::kRead}};
        auto acquisitions = acquireCollections(opCtx, requests, MODE_IX);
        benchmark::DoNotOptimize(acquisitions);
    }
}

BENCHMARK(BM_acquireCollectionLockFree);
BENCHMARK(BM_acquireCollection);
BENCHMARK(BM_acquireMultiCollection);
}  // namespace
}  // namespace mongo
