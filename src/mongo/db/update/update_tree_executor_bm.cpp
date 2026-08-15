// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/update_tree_executor.h"

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_factory_icu.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_domain_global.h"

#include <string_view>

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.bm");
const std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> kArrayFilters = {};


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
        // ApplyParams holds 'immutablePaths' by reference, so it has to outlive the ApplyParams.
        const FieldRefSet immutablePaths;
        for (auto _ : state) {
            mutablebson::Document document{input};
            UpdateExecutor::ApplyParams params{document.root(), immutablePaths};
            benchmark::DoNotOptimize(executor.applyUpdate(std::move(params)));
            benchmark::ClobberMemory();
        }
    }

    // As benchmarkUpdate, but also serializes the mutated document back to BSON. Applying a
    // modifier and serializing the result are separately interesting: the apply cost is dominated
    // by locating the target element, while the serialize cost is dominated by how much of the
    // original buffer writeChildren() can copy in bulk rather than element by element. Pairing this
    // with the apply-only benchmark on the same inputs isolates the serialization.
    void benchmarkUpdateAndSerialize(UpdateTreeExecutor executor,
                                     BSONObj input,
                                     benchmark::State& state) {
        const FieldRefSet immutablePaths;
        for (auto _ : state) {
            mutablebson::Document document{input};
            UpdateExecutor::ApplyParams params{document.root(), immutablePaths};
            benchmark::DoNotOptimize(executor.applyUpdate(std::move(params)));
            benchmark::DoNotOptimize(document.getObject());
            benchmark::ClobberMemory();
        }
    }

    UpdateTreeExecutor makeExecutor(modifiertable::ModifierType modifierType, BSONObj spec) {
        std::set<std::string> foundIdentifiers;
        auto updateTree = std::make_unique<UpdateObjectNode>();
        tassert(11788501,
                "Failed to parse update modifier",
                UpdateObjectNode::parseAndMerge(updateTree.get(),
                                                modifierType,
                                                spec.firstElement(),
                                                _expCtx,
                                                kArrayFilters,
                                                foundIdentifiers)
                    .isOK());
        return UpdateTreeExecutor(std::move(updateTree));
    }

    // As makeExecutor, but merges every field of 'spec' into the tree, the way a multi-field
    // modifier such as {$set: {a: 1, b: 2}} is parsed.
    UpdateTreeExecutor makeMultiFieldExecutor(modifiertable::ModifierType modifierType,
                                              BSONObj spec) {
        std::set<std::string> foundIdentifiers;
        auto updateTree = std::make_unique<UpdateObjectNode>();
        for (auto&& elem : spec) {
            tassert(
                11788502,
                "Failed to parse update modifier",
                UpdateObjectNode::parseAndMerge(
                    updateTree.get(), modifierType, elem, _expCtx, kArrayFilters, foundIdentifiers)
                    .isOK());
        }
        return UpdateTreeExecutor(std::move(updateTree));
    }

    void benchmarkAddToSet(BSONObj inputDoc,
                           BSONObj addToSetSpec,
                           std::shared_ptr<CollatorInterface> collator,
                           benchmark::State& state) {
        _expCtx->setCollator(std::move(collator));
        benchmarkUpdate(makeExecutor(modifiertable::ModifierType::MOD_ADD_TO_SET, addToSetSpec),
                        inputDoc,
                        state);
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

// $set of a single field in a wide, flat document, modelled on the mongo-perf
// 'Update.FieldAtOffset' and 'Update.SingleDocFieldAtOffset' cases.
//
// The interesting parameter is where the modified field sits, because locating it materializes an
// ElementRep for every field to its left. That splits the document into a materialized prefix and a
// still-opaque tail, and those two halves are serialized by different code paths in
// writeChildren(): the tail by the long-standing "copy the opaque tail" shortcut, the prefix by the
// bulk copy of unmodified runs. Sweeping the offset therefore covers both.
//
//   offset 0%   - nothing materialized to the left, so serialization is almost entirely the
//                 opaque-tail copy (the pre-existing path).
//   offset 50%  - half prefix, half tail; both paths carry real work.
//   offset 100% - everything is materialized, so the whole document is one unmodified run plus the
//                 changed field, and the run copy does essentially all of the work.
//
// The narrow (8 field) configurations guard against the run-length check costing anything on
// documents too small to have a run worth copying.
BSONObj generateWideFlatObject(int64_t fieldCount) {
    BSONObjBuilder builder;
    for (int64_t i = 0; i < fieldCount; ++i) {
        builder.append("f" + std::to_string(i), "v");
    }
    return builder.obj();
}

// Returns the index of the field to modify, given a percentage along the document.
int64_t targetFieldIndex(int64_t fieldCount, int64_t offsetPercent) {
    return (fieldCount - 1) * offsetPercent / 100;
}

BENCHMARK_DEFINE_F(UpdateTreeExecutorBenchmark, SetFieldAtOffset)(benchmark::State& state) {
    const int64_t fieldCount = state.range(0);
    const int64_t target = targetFieldIndex(fieldCount, state.range(1));
    BSONObj inputDoc = generateWideFlatObject(fieldCount);
    // A longer replacement value, so the change cannot be applied in place.
    BSONObj setSpec = BSON("f" + std::to_string(target) << "vv");
    benchmarkUpdate(makeExecutor(modifiertable::ModifierType::MOD_SET, setSpec), inputDoc, state);
}

BENCHMARK_DEFINE_F(UpdateTreeExecutorBenchmark,
                   SetFieldAtOffsetAndSerialize)(benchmark::State& state) {
    const int64_t fieldCount = state.range(0);
    const int64_t target = targetFieldIndex(fieldCount, state.range(1));
    BSONObj inputDoc = generateWideFlatObject(fieldCount);
    BSONObj setSpec = BSON("f" + std::to_string(target) << "vv");
    benchmarkUpdateAndSerialize(
        makeExecutor(modifiertable::ModifierType::MOD_SET, setSpec), inputDoc, state);
}

static void configureSetAtOffsetBenchmark(benchmark::internal::Benchmark* bm) {
    static const std::vector<int64_t> kFieldCounts = {8, 512};
    static const std::vector<int64_t> kOffsetPercents = {0, 50, 100};
    bm->ArgsProduct({kFieldCounts, kOffsetPercents});
}

BENCHMARK_REGISTER_F(UpdateTreeExecutorBenchmark, SetFieldAtOffset)
    ->Apply(configureSetAtOffsetBenchmark);

BENCHMARK_REGISTER_F(UpdateTreeExecutorBenchmark, SetFieldAtOffsetAndSerialize)
    ->Apply(configureSetAtOffsetBenchmark);

// $set of an early field to a new value, plus a $set of a later field to the value it already has.
// The second $set is a noop, but locating it still materializes an ElementRep for every field in
// between. That leaves a bulk-copyable run of unmodified fields whose right sibling is still
// opaque, which is the only shape in which the run copy is followed directly by the opaque tail
// copy. The plain SetFieldAtOffset cases never produce it, because there the run always ends at the
// modified field.
BENCHMARK_DEFINE_F(UpdateTreeExecutorBenchmark,
                   SetFieldThenNoopSetAtOffsetAndSerialize)(benchmark::State& state) {
    const int64_t fieldCount = state.range(0);
    // At least 1, because 'f0' is the field that actually changes and a modifier cannot name the
    // same path twice.
    const int64_t noopTarget = std::max<int64_t>(1, targetFieldIndex(fieldCount, state.range(1)));
    BSONObj inputDoc = generateWideFlatObject(fieldCount);
    // "f0" gets a longer value so the document must be reserialized; the noop target is set to the
    // "v" it already holds.
    BSONObj setSpec = BSON("f0" << "vv" << "f" + std::to_string(noopTarget) << "v");
    benchmarkUpdateAndSerialize(
        makeMultiFieldExecutor(modifiertable::ModifierType::MOD_SET, setSpec), inputDoc, state);
}

BENCHMARK_REGISTER_F(UpdateTreeExecutorBenchmark, SetFieldThenNoopSetAtOffsetAndSerialize)
    ->Apply(configureSetAtOffsetBenchmark);

// $set of every 'stride'th field of a wide, flat document. This sweeps the run-length distribution
// the bulk copy sees, from "no runs at all" to "one run covering the whole document", which is what
// decides whether the extra walk in bulkCopyContiguousRegion() pays for itself:
//
//   stride 1   - every field is modified, so no two adjacent children are unmodified. The run walk
//                stops immediately at each child, so this measures the cost of the check alone
//                against a document that can never benefit from it.
//   stride 2   - the documented worst case: every other field is modified, leaving runs of exactly
//                one unmodified field. Every one of those runs is walked, found to be too short,
//                and then rejected, so the walk is paid in full and the element is still written
//                one at a time by writeElement(). This is the most double-walking possible for
//                zero benefit.
//   stride 3   - the shortest run the bulk copy will actually take (two fields), so the first
//                configuration where the walk buys anything. Roughly the break-even point.
//   stride 8   - runs of seven; a modest win per run.
//   stride 64  - runs of 63; comfortably in the profitable range.
//   stride 512 - only "f0" is modified, so the rest of the document is a single run. This is the
//                best case, and the one the optimization was written for.
//
// Every field is modified to a longer value, so no update can be applied in place and the whole
// document must be reserialized. Both an apply-only and an apply-plus-serialize variant are
// registered: the bulk copy only runs during serialization, so the difference between the two is
// what the change actually affects. The apply-only numbers are otherwise dominated by parsing and
// walking the update tree, which grows with the number of modified fields and would swamp the
// effect being measured.
BENCHMARK_DEFINE_F(UpdateTreeExecutorBenchmark, SetEveryNthField)(benchmark::State& state) {
    const int64_t fieldCount = state.range(0);
    const int64_t stride = state.range(1);
    BSONObj inputDoc = generateWideFlatObject(fieldCount);
    BSONObjBuilder specBuilder;
    for (int64_t i = 0; i < fieldCount; i += stride) {
        specBuilder.append("f" + std::to_string(i), "vv");
    }
    BSONObj setSpec = specBuilder.obj();
    benchmarkUpdate(
        makeMultiFieldExecutor(modifiertable::ModifierType::MOD_SET, setSpec), inputDoc, state);
}

BENCHMARK_DEFINE_F(UpdateTreeExecutorBenchmark,
                   SetEveryNthFieldAndSerialize)(benchmark::State& state) {
    const int64_t fieldCount = state.range(0);
    const int64_t stride = state.range(1);
    BSONObj inputDoc = generateWideFlatObject(fieldCount);
    BSONObjBuilder specBuilder;
    for (int64_t i = 0; i < fieldCount; i += stride) {
        specBuilder.append("f" + std::to_string(i), "vv");
    }
    BSONObj setSpec = specBuilder.obj();
    benchmarkUpdateAndSerialize(
        makeMultiFieldExecutor(modifiertable::ModifierType::MOD_SET, setSpec), inputDoc, state);
}

static void configureSetEveryNthFieldBenchmark(benchmark::internal::Benchmark* bm) {
    static const std::vector<int64_t> kStrides = {1, 2, 3, 8, 64, 512};
    for (int64_t stride : kStrides) {
        bm->Args({512, stride});
    }
}

BENCHMARK_REGISTER_F(UpdateTreeExecutorBenchmark, SetEveryNthField)
    ->Apply(configureSetEveryNthFieldBenchmark);

BENCHMARK_REGISTER_F(UpdateTreeExecutorBenchmark, SetEveryNthFieldAndSerialize)
    ->Apply(configureSetEveryNthFieldBenchmark);

}  // namespace
}  // namespace mongo
