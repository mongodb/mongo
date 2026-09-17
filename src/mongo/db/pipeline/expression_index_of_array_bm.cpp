// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/expression_bm_fixture.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/util/intrusive_counter.h"

#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>
#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
#include <tcmalloc/malloc_extension.h>
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
#include <gperftools/malloc_extension.h>
#endif

namespace mongo {
namespace {

// Returns the number of bytes currently allocated to the application by the global allocator, or
// boost::none if the build's allocator does not expose it (e.g. the system allocator). Reading the
// delta of this across a call gives the real heap growth of the actual production code, with no
// manual accounting.
boost::optional<int64_t> currentAllocatedBytes() {
#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
    if (auto res =
            tcmalloc::MallocExtension::GetNumericProperty("generic.current_allocated_bytes")) {
        return static_cast<int64_t>(*res);
    }
    return boost::none;
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
    size_t value = 0;
    if (MallocExtension::instance()->GetNumericProperty("generic.current_allocated_bytes",
                                                        &value)) {
        return static_cast<int64_t>(value);
    }
    return boost::none;
#else
    return boost::none;
#endif
}

class IndexOfArrayBenchmarkFixture : public ExpressionBenchmarkFixture {
public:
    void benchmarkExpression(BSONObj, benchmark::State&, const std::vector<Document>&) final {}

protected:
    // The single value repeated to fill the all-duplicates arrays used by the "SameValue"
    // benchmarks. It is also the value those benchmarks search for, so the search always hits.
    static constexpr int kRepeatedValue = 0;

    // Builds the spec:
    //   {"$indexOfArray": [{"$range": [0, arraySize]}, "$nonConstLookupValue"]}
    static BSONObj makeRangeSpec(int arraySize) {
        return BSON("$indexOfArray" << BSON_ARRAY(BSON("$range" << BSON_ARRAY(0 << arraySize))
                                                  << "$nonConstLookupValue"));
    }

    // Builds the spec:
    //   {"$indexOfArray": [[0, 1, ..., arraySize - 1], "$nonConstLookupValue"]}
    // Uses a _literal_ array not a $range.
    static BSONObj makeLiteralArraySpec(int arraySize) {
        BSONArrayBuilder builder;
        for (int i = 0; i < arraySize; ++i) {
            builder.append(i);
        }
        return BSON("$indexOfArray" << BSON_ARRAY(builder.arr() << "$nonConstLookupValue"));
    }

    // Builds a literal array holding 'arraySize' copies of kRepeatedValue.
    static BSONArray makeRepeatedValueArray(int arraySize) {
        BSONArrayBuilder builder;
        for (int i = 0; i < arraySize; ++i) {
            builder.append(kRepeatedValue);
        }
        return builder.arr();
    }

    // Builds the spec:
    //   {"$indexOfArray": [[0, 0, ..., 0], "$nonConstLookupValue"]}
    static BSONObj makeSameValueSpec(int arraySize) {
        return BSON("$indexOfArray"
                    << BSON_ARRAY(makeRepeatedValueArray(arraySize) << "$nonConstLookupValue"));
    }

    // Builds the spec:
    //   {"$indexOfArray": [[0, 0, ..., 0], "$nonConstLookupValue", arraySize - 2, arraySize]}
    // The start/end window covers only the final two positions, so a lookup that iterates the
    // duplicate matches in array order must skip almost the whole array before landing in range.
    static BSONObj makeSameValueRangeSpec(int arraySize) {
        return BSON("$indexOfArray"
                    << BSON_ARRAY(makeRepeatedValueArray(arraySize)
                                  << "$nonConstLookupValue" << (arraySize - 2) << arraySize));
    }

    boost::intrusive_ptr<Expression> parse(ExpressionContext* exprContext, const BSONObj& spec) {
        return Expression::parseExpression(exprContext, spec, exprContext->variablesParseState);
    }
};

BENCHMARK_DEFINE_F(IndexOfArrayBenchmarkFixture, OptimizeIndexOfArrayConst)
(benchmark::State& state) {
    const int arraySize = state.range(0);
    const BSONObj spec = makeRangeSpec(arraySize);

    auto opContext = getServiceContext()->makeOperationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.bm");

    for (auto _ : state) {
        auto exprContext = make_intrusive<ExpressionContextForTest>(opContext.get(), nss);
        auto expression = parse(exprContext.get(), spec);
        benchmark::DoNotOptimize(expression->optimize());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * arraySize);
}

BENCHMARK_DEFINE_F(IndexOfArrayBenchmarkFixture, EvaluateIndexOfArrayConst)
(benchmark::State& state) {
    const int arraySize = state.range(0);
    const BSONObj spec = makeRangeSpec(arraySize);

    auto opContext = getServiceContext()->makeOperationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.bm");
    auto exprContext = make_intrusive<ExpressionContextForTest>(opContext.get(), nss);
    auto expression = parse(exprContext.get(), spec)->optimize();
    auto* variables = &(exprContext->variables);

    const Document doc{{"nonConstLookupValue", Value(arraySize - 1)}};

    for (auto _ : state) {
        benchmark::DoNotOptimize(expression->evaluate(doc, variables));
        benchmark::ClobberMemory();
    }
}

// Optimize timing for an all-duplicates array. The helper is built the same way as for a $range
// array; only the array contents differ, so this isolates any cost the lookup helper pays when
// every element collides on a single value.
BENCHMARK_DEFINE_F(IndexOfArrayBenchmarkFixture, OptimizeIndexOfArraySameValue)
(benchmark::State& state) {
    const int arraySize = state.range(0);
    const BSONObj spec = makeSameValueSpec(arraySize);

    auto opContext = getServiceContext()->makeOperationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.bm");

    for (auto _ : state) {
        auto exprContext = make_intrusive<ExpressionContextForTest>(opContext.get(), nss);
        auto expression = parse(exprContext.get(), spec);
        benchmark::DoNotOptimize(expression->optimize());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * arraySize);
}

// Evaluate timing for an all-duplicates array, searching for the repeated value across the whole
// array. Every element matches, so the search resolves as early as possible.
BENCHMARK_DEFINE_F(IndexOfArrayBenchmarkFixture, EvaluateIndexOfArraySameValue)
(benchmark::State& state) {
    const int arraySize = state.range(0);
    const BSONObj spec = makeSameValueSpec(arraySize);

    auto opContext = getServiceContext()->makeOperationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.bm");
    auto exprContext = make_intrusive<ExpressionContextForTest>(opContext.get(), nss);
    auto expression = parse(exprContext.get(), spec)->optimize();
    auto* variables = &(exprContext->variables);

    const Document doc{{"nonConstLookupValue", Value(kRepeatedValue)}};

    for (auto _ : state) {
        benchmark::DoNotOptimize(expression->evaluate(doc, variables));
        benchmark::ClobberMemory();
    }
}

// Optimize timing for an all-duplicates array queried with a start/end window near the end of the
// array. Optimize is unaffected by the window, so this should track OptimizeIndexOfArraySameValue.
BENCHMARK_DEFINE_F(IndexOfArrayBenchmarkFixture, OptimizeIndexOfArraySameValueRange)
(benchmark::State& state) {
    const int arraySize = state.range(0);
    const BSONObj spec = makeSameValueRangeSpec(arraySize);

    auto opContext = getServiceContext()->makeOperationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.bm");

    for (auto _ : state) {
        auto exprContext = make_intrusive<ExpressionContextForTest>(opContext.get(), nss);
        auto expression = parse(exprContext.get(), spec);
        benchmark::DoNotOptimize(expression->optimize());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * arraySize);
}

// Evaluate timing for an all-duplicates array with a start/end window covering only the final two
// positions. The lookup must walk the duplicate matches in array order until it reaches one inside
// the window, so this exercises the deepest search over duplicates.
BENCHMARK_DEFINE_F(IndexOfArrayBenchmarkFixture, EvaluateIndexOfArraySameValueRange)
(benchmark::State& state) {
    const int arraySize = state.range(0);
    const BSONObj spec = makeSameValueRangeSpec(arraySize);

    auto opContext = getServiceContext()->makeOperationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.bm");
    auto exprContext = make_intrusive<ExpressionContextForTest>(opContext.get(), nss);
    auto expression = parse(exprContext.get(), spec)->optimize();
    auto* variables = &(exprContext->variables);

    const Document doc{{"nonConstLookupValue", Value(kRepeatedValue)}};

    for (auto _ : state) {
        benchmark::DoNotOptimize(expression->evaluate(doc, variables));
        benchmark::ClobberMemory();
    }
}

// Evaluate timing for the $range array searching for a value that is absent from the array. The
// optimize cost is identical to EvaluateIndexOfArrayConst (the helper is unaffected by the search
// value), so only the miss-path evaluate cost is measured here.
BENCHMARK_DEFINE_F(IndexOfArrayBenchmarkFixture, EvaluateIndexOfArrayMissing)
(benchmark::State& state) {
    const int arraySize = state.range(0);
    const BSONObj spec = makeRangeSpec(arraySize);

    auto opContext = getServiceContext()->makeOperationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.bm");
    auto exprContext = make_intrusive<ExpressionContextForTest>(opContext.get(), nss);
    auto expression = parse(exprContext.get(), spec)->optimize();
    auto* variables = &(exprContext->variables);

    // -1 never appears in the $range array [0, arraySize).
    const Document doc{{"nonConstLookupValue", Value(-1)}};

    for (auto _ : state) {
        benchmark::DoNotOptimize(expression->evaluate(doc, variables));
        benchmark::ClobberMemory();
    }
}

BENCHMARK_DEFINE_F(IndexOfArrayBenchmarkFixture, IndexOfArrayConstMemory)
(benchmark::State& state) {
    const int arraySize = state.range(0);
    const BSONObj spec = makeRangeSpec(arraySize);

    auto opContext = getServiceContext()->makeOperationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.bm");

    int64_t allocatedBytes = 0;
    for (auto _ : state) {
        auto exprContext = make_intrusive<ExpressionContextForTest>(opContext.get(), nss);
        auto expression = parse(exprContext.get(), spec);

        const auto before = currentAllocatedBytes();
        auto optimized = expression->optimize();
        const auto after = currentAllocatedBytes();
        if (before && after) {
            allocatedBytes = *after - *before;
        }

        benchmark::DoNotOptimize(optimized);
        benchmark::ClobberMemory();
    }

    state.counters["allocatedBytes"] = benchmark::Counter(static_cast<double>(allocatedBytes),
                                                          benchmark::Counter::kDefaults,
                                                          benchmark::Counter::OneK::kIs1024);
    state.counters["bytesPerElem"] =
        benchmark::Counter(static_cast<double>(allocatedBytes) / static_cast<double>(arraySize));
}

BENCHMARK_DEFINE_F(IndexOfArrayBenchmarkFixture, IndexOfArrayLiteralConstMemory)
(benchmark::State& state) {
    const int arraySize = state.range(0);
    const BSONObj spec = makeLiteralArraySpec(arraySize);

    auto opContext = getServiceContext()->makeOperationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.bm");

    int64_t allocatedBytes = 0;
    for (auto _ : state) {
        auto exprContext = make_intrusive<ExpressionContextForTest>(opContext.get(), nss);
        auto expression = parse(exprContext.get(), spec);

        const auto before = currentAllocatedBytes();
        auto optimized = expression->optimize();
        const auto after = currentAllocatedBytes();
        if (before && after) {
            allocatedBytes = *after - *before;
        }

        benchmark::DoNotOptimize(optimized);
        benchmark::ClobberMemory();
    }

    state.counters["allocatedBytes"] = benchmark::Counter(static_cast<double>(allocatedBytes),
                                                          benchmark::Counter::kDefaults,
                                                          benchmark::Counter::OneK::kIs1024);
    state.counters["bytesPerElem"] =
        benchmark::Counter(static_cast<double>(allocatedBytes) / static_cast<double>(arraySize));
}

void configureArgs(benchmark::internal::Benchmark* bm) {
    bm->ArgName("arraySize");
    // The largest size approximates the internalQueryMaxRangeBytes cap on a single $range array.
    for (int arraySize : {1'000, 10'000, 100'000, 1'000'000, 6'000'000}) {
        bm->Arg(arraySize);
    }
}

// The "SameValue" benchmarks embed a literal array in the spec rather than generating it with
// $range, so the array must fit within the 16MB BSONObj limit. Six million int elements exceed it,
// so those benchmarks stop at one million.
void configureLiteralArrayArgs(benchmark::internal::Benchmark* bm) {
    bm->ArgName("arraySize");
    for (int arraySize : {1'000, 10'000, 100'000, 1'000'000}) {
        bm->Arg(arraySize);
    }
}

// Registered first so it reads the global allocator's live-bytes counter before the other
// benchmarks have churned or fragmented the heap, keeping the measured allocation delta clean.
BENCHMARK_REGISTER_F(IndexOfArrayBenchmarkFixture, IndexOfArrayConstMemory)->Apply(configureArgs);
BENCHMARK_REGISTER_F(IndexOfArrayBenchmarkFixture, IndexOfArrayLiteralConstMemory)
    ->Apply(configureLiteralArrayArgs);
BENCHMARK_REGISTER_F(IndexOfArrayBenchmarkFixture, OptimizeIndexOfArrayConst)->Apply(configureArgs);
BENCHMARK_REGISTER_F(IndexOfArrayBenchmarkFixture, EvaluateIndexOfArrayConst)->Apply(configureArgs);

BENCHMARK_REGISTER_F(IndexOfArrayBenchmarkFixture, OptimizeIndexOfArraySameValue)
    ->Apply(configureLiteralArrayArgs);
BENCHMARK_REGISTER_F(IndexOfArrayBenchmarkFixture, EvaluateIndexOfArraySameValue)
    ->Apply(configureLiteralArrayArgs);
BENCHMARK_REGISTER_F(IndexOfArrayBenchmarkFixture, OptimizeIndexOfArraySameValueRange)
    ->Apply(configureLiteralArrayArgs);
BENCHMARK_REGISTER_F(IndexOfArrayBenchmarkFixture, EvaluateIndexOfArraySameValueRange)
    ->Apply(configureLiteralArrayArgs);
BENCHMARK_REGISTER_F(IndexOfArrayBenchmarkFixture, EvaluateIndexOfArrayMissing)
    ->Apply(configureArgs);

}  // namespace
}  // namespace mongo
