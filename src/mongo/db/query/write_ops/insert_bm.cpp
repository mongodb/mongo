// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/write_ops/insert.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/query/query_test_service_context.h"

#include <memory>
#include <string>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

/**
 * Generates a flat document with the given number of fields.
 * If 'idFirst' is true the _id is the first field; if 'includeId' is false no _id is added.
 */
BSONObj makeFlatDocument(int numFields, bool includeId, bool idFirst) {
    BSONObjBuilder builder;
    if (includeId && idFirst) {
        builder.append("_id", 1);
    }
    for (int i = 0; i < numFields; ++i) {
        builder.append(std::to_string(i), i);
    }
    if (includeId && !idFirst) {
        builder.append("_id", 1);
    }
    return builder.obj();
}

/**
 * Generates a nested document of the given depth, where depth 1 is {a: 1}.
 */
BSONObj makeNestedDocument(int depth) {
    BSONObj obj = BSON("a" << 1);
    for (int i = 1; i < depth; ++i) {
        obj = BSON("a" << obj);
    }
    return obj;
}

/**
 * Generates a flat document that contains a dollar-prefixed field near the end.
 */
BSONObj makeDollarFieldDocument(int numFields) {
    BSONObjBuilder builder;
    for (int i = 0; i < numFields; ++i) {
        builder.append(std::to_string(i), i);
    }
    builder.append("$x", 1);
    return builder.obj();
}

/**
 * Fixture that creates and tears down the ServiceContext and OperationContext once per benchmark.
 */
class FixDocumentForInsert : public benchmark::Fixture {
public:
    void SetUp(benchmark::State&) override {
        _sc = std::make_unique<QueryTestServiceContext>();
        _opCtx = _sc->makeOperationContext();
    }

    void TearDown(benchmark::State&) override {
        _opCtx.reset();
        _sc.reset();
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

private:
    std::unique_ptr<QueryTestServiceContext> _sc;
    ServiceContext::UniqueOperationContext _opCtx;
};

BENCHMARK_DEFINE_F(FixDocumentForInsert, Empty)(benchmark::State& state) {
    BSONObj doc;

    for (auto _ : state) {
        benchmark::DoNotOptimize(fixDocumentForInsert(opCtx(), doc));
    }
}

BENCHMARK_DEFINE_F(FixDocumentForInsert, IdFirst_Small)(benchmark::State& state) {
    auto doc = BSON("_id" << 1 << "x" << 2 << "y" << 3);

    for (auto _ : state) {
        benchmark::DoNotOptimize(fixDocumentForInsert(opCtx(), doc));
    }
}

BENCHMARK_DEFINE_F(FixDocumentForInsert, IdNotFirst)(benchmark::State& state) {
    auto doc = BSON("x" << 1 << "_id" << 2 << "y" << 3);

    for (auto _ : state) {
        benchmark::DoNotOptimize(fixDocumentForInsert(opCtx(), doc));
    }
}

BENCHMARK_DEFINE_F(FixDocumentForInsert, NoId)(benchmark::State& state) {
    auto doc = BSON("x" << 1 << "y" << 2);

    for (auto _ : state) {
        benchmark::DoNotOptimize(fixDocumentForInsert(opCtx(), doc));
    }
}

BENCHMARK_DEFINE_F(FixDocumentForInsert, DollarField)(benchmark::State& state) {
    bool containsDotsAndDollarsField = false;
    auto doc = makeDollarFieldDocument(10);

    for (auto _ : state) {
        benchmark::DoNotOptimize(fixDocumentForInsert(
            opCtx(), doc, /*bypassEmptyTsReplacement=*/false, &containsDotsAndDollarsField));
    }
}

BENCHMARK_DEFINE_F(FixDocumentForInsert, LargeFlat)(benchmark::State& state) {
    auto doc = makeFlatDocument(static_cast<int>(state.range(0)), true, false);

    for (auto _ : state) {
        benchmark::DoNotOptimize(fixDocumentForInsert(opCtx(), doc));
    }
}

BENCHMARK_DEFINE_F(FixDocumentForInsert, DeeplyNested)(benchmark::State& state) {
    auto doc = makeNestedDocument(static_cast<int>(state.range(0)));

    for (auto _ : state) {
        benchmark::DoNotOptimize(fixDocumentForInsert(opCtx(), doc));
    }
}

BENCHMARK_DEFINE_F(FixDocumentForInsert, TimestampReplacement)(benchmark::State& state) {
    auto doc = BSON("x" << 1 << "ts" << Timestamp(0, 0) << "_id" << 1);

    for (auto _ : state) {
        benchmark::DoNotOptimize(fixDocumentForInsert(opCtx(), doc));
    }
}

BENCHMARK_REGISTER_F(FixDocumentForInsert, Empty);
BENCHMARK_REGISTER_F(FixDocumentForInsert, IdFirst_Small);
BENCHMARK_REGISTER_F(FixDocumentForInsert, IdNotFirst);
BENCHMARK_REGISTER_F(FixDocumentForInsert, NoId);
BENCHMARK_REGISTER_F(FixDocumentForInsert, DollarField);
BENCHMARK_REGISTER_F(FixDocumentForInsert, LargeFlat)
    ->ArgName("numFields")
    ->Args({10})
    ->Args({100})
    ->Args({1000});
BENCHMARK_REGISTER_F(FixDocumentForInsert, DeeplyNested)
    ->ArgName("depth")
    ->Args({5})
    ->Args({50})
    ->Args({100});
BENCHMARK_REGISTER_F(FixDocumentForInsert, TimestampReplacement);

}  // namespace
}  // namespace mongo
