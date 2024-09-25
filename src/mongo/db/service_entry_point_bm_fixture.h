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

#include <benchmark/benchmark.h>

#include "mongo/base/init.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/profiler_bm_fixture.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/processinfo.h"

namespace mongo {
namespace {

class ServiceEntryPointBenchmarkFixture : public BenchmarkWithProfiler {
public:
    void SetUp(benchmark::State& state) override {
        stdx::lock_guard lk(_setupMutex);
        if (_configuredThreads++)
            return;

        serverGlobalParams.clusterRole = getClusterRole();
        BenchmarkWithProfiler::SetUp(state);

        setGlobalServiceContext(ServiceContext::make());

        // Minimal set up necessary for ServiceEntryPoint.
        auto service = getGlobalServiceContext();

        ReadWriteConcernDefaults::create(service, _lookupMock.getFetchDefaultsFn());
        _lookupMock.setLookupCallReturnValue({});

        setupImpl(service);
        setServiceEntryPoint(service);
    }

    void TearDown(benchmark::State& state) override {
        stdx::lock_guard lk(_setupMutex);
        if (--_configuredThreads)
            return;
        setGlobalServiceContext({});

        BenchmarkWithProfiler::TearDown(state);
    }

    virtual void setServiceEntryPoint(ServiceContext* service) const = 0;

    virtual void setupImpl(ServiceContext* service){};

    virtual ClusterRole getClusterRole() const = 0;

    void doRequest(ServiceEntryPoint* sep, Client* client, Message& msg) {
        auto newOpCtx = client->makeOperationContext();
        iassert(sep->handleRequest(newOpCtx.get(), msg).getNoThrow());
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

    Mutex _setupMutex;
    ReadWriteConcernDefaultsLookupMock _lookupMock;
    size_t _configuredThreads = 0;
};

/**
 * ASAN can't handle the # of threads the benchmark creates.
 * With sanitizers, run this in a diminished "correctness check" mode.
 */
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
const auto kSEPBMMaxThreads = 1;
#else
/** 2x to benchmark the case of more threads than cores for curiosity's sake. */
const auto kSEPBMMaxThreads = 2 * ProcessInfo::getNumLogicalCores();
#endif


/**
 * Required initializers, but this is a benchmark so nothing needs to be done.
 */
MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {}
MONGO_INITIALIZER(ServerLogRedirection)(mongo::InitializerContext*) {}

}  // namespace
}  // namespace mongo
