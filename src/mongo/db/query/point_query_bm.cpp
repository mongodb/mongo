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
#include <vector>

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/profiler_bm_fixture.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/platform/random.h"
#include "mongo/transport/service_entry_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("testDb", "testColl");

class PointQueryBenchmark : public BenchmarkWithProfiler {
public:
    PointQueryBenchmark() {
        _setupDone.emplace();
    }

    void SetUp(benchmark::State& state) override {
        // Only one thread need to do global setup
        if (state.thread_index == 0) {
            BenchmarkWithProfiler::SetUp(state);
            _fixture.emplace(CatalogScopedGlobalServiceContextForTest::Options{}, false);

            ReadWriteConcernDefaults::create(getGlobalServiceContext(),
                                             _lookupMock.getFetchDefaultsFn());
            _lookupMock.setLookupCallReturnValue({});

            populateCollection(state.range(0), state.range(1));
            _setupDone->set();
        } else {
            _setupDone->get();
        }
    }

    void TearDown(benchmark::State& state) override {
        // Only one thread need to do global teardown
        if (state.thread_index == 0) {
            _fixture.reset();
            _docs.clear();
            BenchmarkWithProfiler::TearDown(state);
            _setupDone.emplace();
        }
    }

    void runBenchmark(BSONObj filter, benchmark::State& state) {
        BSONObj command =
            BSON("find" << kNss.coll() << "$db" << kNss.db_forTest() << "filter" << filter);
        OpMsgRequest request;
        request.body = command;
        auto msg = request.serialize();

        ThreadClient threadClient{getGlobalServiceContext()->getService()};
        runBenchmarkWithProfiler(
            [&]() {
                Client& client = cc();
                auto opCtx = client.makeOperationContext();
                auto statusWithResponse = client.getService()
                                              ->getServiceEntryPoint()
                                              ->handleRequest(opCtx.get(), msg)
                                              .getNoThrow();
                iassert(statusWithResponse);
                LOGV2_DEBUG(9278700,
                            1,
                            "db response",
                            "request"_attr = msg.opMsgDebugString(),
                            "response"_attr =
                                statusWithResponse.getValue().response.opMsgDebugString());
            },
            state);
    }

protected:
    const std::vector<BSONObj>& docs() const {
        return _docs;
    }

private:
    char randomLowercaseAlpha() {
        return 'a' + _random.nextInt64(24);
    }

    BSONObj generateDocument(size_t index, size_t approximateSize) {
        std::string str;
        str.reserve(approximateSize);
        for (size_t i = 0; i < approximateSize; ++i) {
            str.push_back(randomLowercaseAlpha());
        }
        auto uniqueField = static_cast<long long>(index);
        auto nonUniqueField = static_cast<long long>(index / 2);
        return BSONObjBuilder{}
            .append("_id", OID::gen())
            .append("uniqueField", uniqueField)
            .append("nonUniqueField", nonUniqueField)
            .append("arrayField", std::vector<long long>{uniqueField, nonUniqueField})
            .append("str", str)
            .obj();
    }

    BSONObj buildIndexSpec(StringData fieldName, bool unique) {
        return BSONObjBuilder{}
            .append("v", IndexDescriptor::kLatestIndexVersion)
            .append("key", BSON(fieldName << 1))
            .append("name", fieldName + "_1")
            .append("unique", unique)
            .obj();
    }

    void createIndexes(OperationContext* opCtx) {
        const std::vector<BSONObj> kIndexes = {buildIndexSpec("uniqueField", true),
                                               buildIndexSpec("nonUniqueField", false),
                                               buildIndexSpec("arrayField", false)};

        for (const auto& indexSpec : kIndexes) {
            auto acquisition = acquireCollection(opCtx,
                                                 CollectionAcquisitionRequest::fromOpCtx(
                                                     opCtx, kNss, AcquisitionPrerequisites::kWrite),
                                                 MODE_X);
            CollectionWriter coll(opCtx, &acquisition);

            WriteUnitOfWork wunit(opCtx);
            uassertStatusOK(coll.getWritableCollection(opCtx)
                                ->getIndexCatalog()
                                ->createIndexOnEmptyCollection(
                                    opCtx, coll.getWritableCollection(opCtx), indexSpec)
                                .getStatus());
            wunit.commit();
        }
    }

    void populateCollection(size_t size, size_t approximateSize) {
        std::vector<InsertStatement> inserts;
        for (size_t i = 0; i < size; ++i) {
            _docs.push_back(generateDocument(i, approximateSize));
            inserts.emplace_back(_docs.back());
        }

        ThreadClient threadClient{getGlobalServiceContext()->getService()};
        auto opCtx = cc().makeOperationContext();
        auto storage = repl::StorageInterface::get(opCtx.get());

        uassertStatusOK(storage->createCollection(opCtx.get(), kNss, CollectionOptions{}));
        createIndexes(opCtx.get());
        uassertStatusOK(storage->insertDocuments(opCtx.get(), kNss, inserts));
    }

    static constexpr int32_t kSeed = 1;
    PseudoRandom _random{kSeed};

    boost::optional<CatalogScopedGlobalServiceContextForTest> _fixture;
    ReadWriteConcernDefaultsLookupMock _lookupMock;
    std::vector<BSONObj> _docs;

    boost::optional<Notification<void>> _setupDone;
};

BENCHMARK_DEFINE_F(PointQueryBenchmark, IdPointQuery)
(benchmark::State& state) {
    auto id = docs()[docs().size() / 2].getField("_id").OID();
    runBenchmark(BSON("_id" << id), state);
}

BENCHMARK_DEFINE_F(PointQueryBenchmark, UniqueFieldPointQuery)
(benchmark::State& state) {
    int64_t fieldValue = docs().size() / 2;
    runBenchmark(BSON("uniqueField" << fieldValue), state);
}

BENCHMARK_DEFINE_F(PointQueryBenchmark, NonUniqueFieldPointQuery)
(benchmark::State& state) {
    int64_t fieldValue = docs().size() / 3;
    runBenchmark(BSON("nonUniqueField" << fieldValue), state);
}

BENCHMARK_DEFINE_F(PointQueryBenchmark, ArrayFieldPointQuery)
(benchmark::State& state) {
    int64_t fieldValue = docs().size() / 3;
    runBenchmark(BSON("arrayField" << fieldValue), state);
}

/**
 * ASAN can't handle the # of threads the benchmark creates. With sanitizers, run this in a
 * diminished "correctness check" mode. See SERVER-73168.
 */
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
const auto kMaxThreads = 1;
#else
/** 2x to benchmark the case of more threads than cores for curiosity's sake. */
const auto kMaxThreads = 2 * ProcessInfo::getNumLogicalCores();
#endif

static void configureBenchmarks(benchmark::internal::Benchmark* bm) {
    bm->ThreadRange(1, kMaxThreads)->Args({10, 1024});
}

BENCHMARK_REGISTER_F(PointQueryBenchmark, IdPointQuery)->Apply(configureBenchmarks);
BENCHMARK_REGISTER_F(PointQueryBenchmark, UniqueFieldPointQuery)->Apply(configureBenchmarks);
BENCHMARK_REGISTER_F(PointQueryBenchmark, NonUniqueFieldPointQuery)->Apply(configureBenchmarks);
BENCHMARK_REGISTER_F(PointQueryBenchmark, ArrayFieldPointQuery)->Apply(configureBenchmarks);
}  // namespace
}  // namespace mongo
