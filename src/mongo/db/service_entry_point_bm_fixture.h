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

#pragma once

#include "mongo/base/init.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/unittest/benchmark_util.h"
#include "mongo/util/processinfo.h"

#include <memory>

#include <benchmark/benchmark.h>

namespace mongo {
class ServiceEntryPointBenchmarkFixture : public unittest::BenchmarkWithProfiler {
public:
    void setUpSharedResources(benchmark::State& state) override {
        BenchmarkWithProfiler::setUpSharedResources(state);
        serverGlobalParams.clusterRole = getClusterRole();

        setGlobalServiceContext(ServiceContext::make());

        // Minimal set up necessary for ServiceEntryPoint.
        auto sc = getGlobalServiceContext();
        auto service = sc->getService(getClusterRole());

        ReadWriteConcernDefaults::create(service, _lookupMock.getFetchDefaultsFn());
        _lookupMock.setLookupCallReturnValue({});
        setUpServiceContext(sc);
    }

    void tearDownSharedResources(benchmark::State& state) override {
        tearDownServiceContext(getGlobalServiceContext());
        setGlobalServiceContext({});
        BenchmarkWithProfiler::tearDownSharedResources(state);
    }

    /** Any custom service context setup and teardown, such as attaching a ServiceEntryPoint. */
    virtual void setUpServiceContext(ServiceContext*) {}
    virtual void tearDownServiceContext(ServiceContext*) {}

    virtual ClusterRole getClusterRole() const = 0;

    void doRequest(ServiceEntryPoint* sep, Client* client, Message& msg) {
        auto newOpCtx = client->makeOperationContext();
        iassert(sep->handleRequest(newOpCtx.get(), msg, newOpCtx.get()->fastClockSource().now())
                    .getNoThrow());
    }

    void runBenchmark(benchmark::State& state, BSONObj obj) {
        auto strand = ClientStrand::make(
            getGlobalServiceContext()
                ->getService(getClusterRole())
                ->makeClient(fmt::format("conn{}", _nextClientId.fetchAndAdd(1)), nullptr));
        OpMsgRequest request;
        request.body = obj;
        auto msg = request.serialize();
        strand->run([&] {
            auto client = strand->getClientPointer();
            auto sep = client->getService()->getServiceEntryPoint();
            runBenchmarkWithProfiler([&]() { doRequest(sep, client, msg); }, state);
        });
    }

    static auto makePingCommand() {
        return BSON("ping" << 1 << "$db"
                           << "admin");
    }

private:
    Atomic<uint64_t> _nextClientId{0};

    ReadWriteConcernDefaultsLookupMock _lookupMock;
};

/**
 * ASAN can't handle the # of threads the benchmark creates.
 * With sanitizers, run this in a diminished "correctness check" mode.
 */
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
const auto kCommandBMMaxThreads = 1;
#else
/** 2x to benchmark the case of more threads than cores for curiosity's sake. */
const auto kCommandBMMaxThreads = 2 * ProcessInfo::getNumLogicalCores();
#endif


/**
 * Required initializers, but this is a benchmark so nothing needs to be done.
 *
 * These should not be in a header file, but it works because it is only included in 2 files and
 * they are never included into the same binary. If that changes, these should find a home in a new
 * cpp file.
 */
MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))  // NOLINT
(InitializerContext* context) {}
MONGO_INITIALIZER(ServerLogRedirection)(mongo::InitializerContext*) {}  // NOLINT

}  // namespace mongo
