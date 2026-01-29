/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/update/update_tree_executor.h"

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_factory_icu.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_domain_global.h"

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

static const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.bm");
static const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> kArrayFilters = {};


void configureLogging(bool disable) {
    auto& lv2Manager = logv2::LogManager::global();
    logv2::LogDomainGlobal::ConfigurationOptions lv2Config;
    if (disable)
        lv2Config.makeDisabled();
    uassertStatusOK(lv2Manager.getGlobalDomainInternal().configure(lv2Config));
}

class UpdateTreeExecutorBenchmark : public benchmark::Fixture {
private:
    static constexpr int32_t kSeed = 1;

protected:
    void benchmarkUpdate(UpdateTreeExecutor executor, BSONObj input, benchmark::State& state) {
        for (auto _ : state) {
            mutablebson::Document document{input};
            UpdateExecutor::ApplyParams params{document.root(), FieldRefSet{}};
            benchmark::DoNotOptimize(executor.applyUpdate(std::move(params)));
            benchmark::ClobberMemory();
        }
    }

    void benchmarkAddToSet(BSONObj inputDoc,
                           BSONObj addToSetSpec,
                           std::shared_ptr<CollatorInterface> collator,
                           benchmark::State& state) {
        _expCtx->setCollator(std::move(collator));
        std::set<std::string> foundIdentifiers;
        auto updateTree = std::make_unique<UpdateObjectNode>();
        tassert(11788501,
                "Failed to parse $addToSet",
                UpdateObjectNode::parseAndMerge(updateTree.get(),
                                                modifiertable::ModifierType::MOD_ADD_TO_SET,
                                                addToSetSpec.firstElement(),
                                                _expCtx,
                                                kArrayFilters,
                                                foundIdentifiers)
                    .isOK());

        UpdateTreeExecutor executor(std::move(updateTree));
        benchmarkUpdate(std::move(executor), inputDoc, state);
    }

    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;

    std::mt19937 _random{kSeed};

private:
    void SetUp(benchmark::State& state) final {
        QueryFCVEnvironmentForTest::setUp();
        // Temporarily disable logging because we don't want logs from service context
        // initialization to clutter the benchmark results table.
        configureLogging(true);
        ON_BLOCK_EXIT([&] { configureLogging(false); });
        _scopedGlobalServiceContext = std::make_unique<QueryTestScopedGlobalServiceContext>();

        _opCtx = _scopedGlobalServiceContext->makeOperationContext();
        _expCtx = make_intrusive<ExpressionContextForTest>(_opCtx.get(), kNss);
    }

    void TearDown(benchmark::State& state) final {
        _opCtx.reset();
        _scopedGlobalServiceContext.reset();
    }

    std::unique_ptr<QueryTestScopedGlobalServiceContext> _scopedGlobalServiceContext;
};

template <typename T>
BSONArray toBsonArray(std::vector<T> array) {
    BSONArrayBuilder builder;
    for (auto& v : array) {
        builder.append(std::move(v));
    }
    return builder.arr();
}

template <typename Generator>
BSONArray generateArray(
    std::mt19937& random, int64_t from, int64_t to, int64_t step, Generator generator) {
    std::vector<std::invoke_result_t<Generator, int64_t>> array;
    for (int64_t i = from; i < to; i += step) {
        array.push_back(generator(i));
    }
    std::shuffle(array.begin(), array.end(), random);
    return toBsonArray(std::move(array));
}

BSONArray generateNumberArray(std::mt19937& random, int64_t from, int64_t to, int64_t step = 1) {
    return generateArray(random, from, to, step, [](auto i) { return i; });
}

BENCHMARK_DEFINE_F(UpdateTreeExecutorBenchmark, AddToSetNumbers)(benchmark::State& state) {
    int64_t inputSize = state.range(0);
    int64_t updateSize = state.range(1);
    BSONObj inputDoc = BSON("a" << generateNumberArray(_random, 0, inputSize));
    // Create array with step 2 to have some overlap between input and update
    BSONObj addToSetSpec =
        BSON("a" << BSON("$each" << generateNumberArray(
                             _random, inputSize - updateSize, inputSize + updateSize, 2)));
    benchmarkAddToSet(inputDoc, addToSetSpec, nullptr /*collator*/, state);
}

BSONArray generateObjectWithStringArray(std::mt19937& random,
                                        int64_t from,
                                        int64_t to,
                                        int64_t step = 1) {
    static constexpr size_t kStringPrefixSize = 64;
    std::string prefix{kStringPrefixSize, '0'};
    return generateArray(random, from, to, step, [&](int64_t index) {
        BSONObjBuilder builder;
        std::stringstream payload;
        payload << prefix << std::hex << index;
        builder.append("payload", payload.str());
        return builder.obj();
    });
}

BENCHMARK_DEFINE_F(UpdateTreeExecutorBenchmark,
                   AddToSetObjectWithStrings)(benchmark::State& state) {
    int64_t inputSize = state.range(0);
    int64_t updateSize = state.range(1);
    BSONObj inputDoc = BSON("a" << generateObjectWithStringArray(_random, 0, inputSize));
    // Create array with step 2 to have some overlap between input and update
    BSONObj addToSetSpec =
        BSON("a" << BSON("$each" << generateObjectWithStringArray(
                             _random, inputSize - updateSize, inputSize + updateSize, 2)));
    benchmarkAddToSet(inputDoc, addToSetSpec, nullptr /*collator*/, state);
}

BENCHMARK_DEFINE_F(UpdateTreeExecutorBenchmark,
                   AddToSetObjectWithStringsCollator)(benchmark::State& state) {
    int64_t inputSize = state.range(0);
    int64_t updateSize = state.range(1);
    BSONObj inputDoc = BSON("a" << generateObjectWithStringArray(_random, 0, inputSize));
    // Create array with step 2 to have some overlap between input and update
    BSONObj addToSetSpec =
        BSON("a" << BSON("$each" << generateObjectWithStringArray(
                             _random, inputSize - updateSize, inputSize + updateSize, 2)));

    CollatorFactoryICU _collatorFactory;
    auto statusWithCollator =
        _collatorFactory.makeFromBSON(BSON("locale" << "en" << "strength" << 2));
    tassert(11788500, "Could not create collator", statusWithCollator.isOK());
    benchmarkAddToSet(inputDoc, addToSetSpec, statusWithCollator.getValue()->cloneShared(), state);
}

static void configureAddToSetBenchmark(benchmark::internal::Benchmark* bm) {
    static const std::vector<int64_t> kSizes = {10, 1000};
    bm->ArgsProduct({kSizes, kSizes});
}

BENCHMARK_REGISTER_F(UpdateTreeExecutorBenchmark, AddToSetNumbers)
    ->Apply(configureAddToSetBenchmark);

BENCHMARK_REGISTER_F(UpdateTreeExecutorBenchmark, AddToSetObjectWithStrings)
    ->Apply(configureAddToSetBenchmark);

BENCHMARK_REGISTER_F(UpdateTreeExecutorBenchmark, AddToSetObjectWithStringsCollator)
    ->Apply(configureAddToSetBenchmark);

}  // namespace
}  // namespace mongo
