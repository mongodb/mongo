// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/internode_validation_hash_utils.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/util/version/releases.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include <benchmark/benchmark.h>

namespace mongo::repl {
namespace {

// The auth initializers pulled in transitively by the replication coordinator mock expect
// startup option storage to have run. This benchmark parses no options, so stub the node out
// to keep the initializer graph satisfiable.
MONGO_INITIALIZER_GENERAL(CoreOptions_Store, (), ())
(InitializerContext*) {}

// Document sizes swept by the hashing benchmarks (64B up to 2MB in powers of two). The hash is
// calculated over the raw BSON bytes, so cost depends only on objsize() and not on the document's
// shape.
constexpr int64_t kDocSizeSweepStart = 1LL << 6;  // 64B
constexpr int64_t kDocSizeSweepEnd = 1LL << 21;   // 2MB
constexpr int kDocSizeSweepMultiplier = 2;

// Builds a document whose objsize() is targetSize.
BSONObj makeDocOfSize(int targetSize, char fill = 'x') {
    static const int overhead = BSON("_id" << 1 << "d" << "").objsize();
    const int payload = std::max(0, targetSize - overhead);
    return BSON("_id" << 1 << "d" << std::string(payload, fill));
}

// Reports both bytes/sec (per-byte SHA-256 throughput) and items/sec (per-document cost).
void setThroughput(benchmark::State& state, int64_t bytesPerIteration) {
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * bytesPerIteration);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

void BM_ComputeDocValidationHash(benchmark::State& state) {
    const BSONObj doc = makeDocOfSize(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        benchmark::DoNotOptimize(computeDocValidationHash(doc));
    }
    setThroughput(state, doc.objsize());
}

void BM_ComputeUpdateValidationHash(benchmark::State& state) {
    const int size = static_cast<int>(state.range(0));
    // Differ in content but not length, so both images cost the same to hash and the XOR is not
    // 0 as it would be for two identical images.
    const BSONObj preImage = makeDocOfSize(size, 'x');
    const BSONObj postImage = makeDocOfSize(size, 'y');
    for (auto _ : state) {
        benchmark::DoNotOptimize(computeUpdateValidationHash(preImage, postImage));
    }
    setThroughput(state, preImage.objsize() + postImage.objsize());
}

class ServiceContextFixture {
public:
    ServiceContextFixture() {
        // (Generic FCV reference): This reference is needed for the feature flag check API.
        serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
        setGlobalServiceContext(ServiceContext::make());
        auto* service = getGlobalServiceContext();

        // The enablement check reads the replication settings, so a replication coordinator has to
        // exist. Default-constructed settings are not a replica set, which is enough for the check
        // to return without consulting the persistence provider this fixture does not set.
        ReplicationCoordinator::set(
            service, std::make_unique<ReplicationCoordinatorMock>(service, ReplSettings{}));

        _client = service->getService()->makeClient("internodeValidationHashBm");
    }

    ServiceContext::UniqueOperationContext makeOpCtx() {
        return _client->makeOperationContext();
    }

private:
    ServiceContext::UniqueClient _client;
};

void BM_IsEnabledCheck(benchmark::State& state) {
    ServiceContextFixture fixture;
    auto opCtx = fixture.makeOpCtx();
    for (auto _ : state) {
        benchmark::DoNotOptimize(isContinuousInternodeValidationPerDocumentEnabled(opCtx.get()));
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

BENCHMARK(BM_ComputeDocValidationHash)
    ->RangeMultiplier(kDocSizeSweepMultiplier)
    ->Range(kDocSizeSweepStart, kDocSizeSweepEnd);
BENCHMARK(BM_ComputeUpdateValidationHash)
    ->RangeMultiplier(kDocSizeSweepMultiplier)
    ->Range(kDocSizeSweepStart, kDocSizeSweepEnd);
BENCHMARK(BM_IsEnabledCheck);

}  // namespace
}  // namespace mongo::repl
