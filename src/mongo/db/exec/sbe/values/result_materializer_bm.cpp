// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/result_materializer.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"

#include <cstdint>
#include <string>
#include <utility>

#include <benchmark/benchmark.h>

namespace mongo::sbe {
namespace {

enum class Output { kBson, kDocument };

/**
 * A record of 20 fields of mixed types: _id followed by f1 to f19.
 */
BSONObj makeRecord() {
    BSONObjBuilder bob;
    bob.append("_id", OID("65f1a2b3c4d5e6f708192a3b"));
    for (int i = 1; i < 20; ++i) {
        const std::string name = "f" + std::to_string(i);
        switch (i % 4) {
            case 0:
                bob.append(name, i);
                break;
            case 1:
                bob.append(name, i * 1.5);
                break;
            case 2:
                bob.append(name, "a string value " + std::to_string(i));
                break;
            case 3:
                bob.append(name, BSON("x" << i << "y" << BSON_ARRAY(1 << 2 << 3)));
                break;
        }
    }
    return bob.obj();
}

value::TagValueView viewOf(const BSONObj& obj) {
    return {value::TypeTags::bsonObject, value::bitcastFrom<const char*>(obj.objdata())};
}

value::TagValueOwned makeOwnedBson(const BSONObj& obj) {
    return value::TagValueOwned::fromRaw(value::copyValue(
        value::TypeTags::bsonObject, value::bitcastFrom<const char*>(obj.objdata())));
}

value::TagValueOwned makeSbeObject(const BSONObj& obj) {
    auto owned = value::TagValueOwned::fromRaw(value::makeNewObject());
    auto* sbeObj = value::getObjectView(owned.value());
    for (auto&& elem : obj) {
        sbeObj->push_back(elem.fieldName(), bson::convertToOwned(elem));
    }
    return owned;
}

value::OwnedValueAccessor makeAccessor(value::TagValueOwned value) {
    value::OwnedValueAccessor accessor;
    accessor.reset(std::move(value));
    return accessor;
}

value::OwnedValueAccessor makeAccessor(const BSONElement& elem) {
    return makeAccessor(bson::convertToOwned(elem));
}

/**
 * Builds one result per iteration. 'nextRow' runs first, as the plan advancing to its next row,
 * so that record slots expose a new record every time.
 */
template <typename NextRow>
void materialize(benchmark::State& state,
                 ResultMaterializer& materializer,
                 Output output,
                 NextRow nextRow) {
    for (auto _ : state) {
        nextRow();
        if (output == Output::kBson) {
            BSONObj result = materializer.toBson(true /* mustOwn */);
            benchmark::DoNotOptimize(result);
        } else {
            Document result = materializer.toDocument();
            benchmark::DoNotOptimize(result);
        }
    }
}

void materialize(benchmark::State& state, ResultMaterializer& materializer, Output output) {
    materialize(state, materializer, output, [] {});
}

// A scan or fetch result returned as is.
void BM_RecordNoChanges(benchmark::State& state, Output output) {
    const BSONObj record = makeRecord();
    value::BSONObjValueAccessor base;
    ResultMaterializer materializer(&base, false, {});
    materialize(state, materializer, output, [&] { base.reset(viewOf(record)); });
}

// A blocking stage's result, such as a sort's, returned as is.
void BM_OwnedBsonNoChanges(benchmark::State& state, Output output) {
    auto base = makeAccessor(makeOwnedBson(makeRecord()));
    ResultMaterializer materializer(&base, false, {});
    materialize(state, materializer, output);
}

// A result built by makeObj with a computed field set on top.
void BM_SbeObjectSetNewField(benchmark::State& state, Output output) {
    auto base = makeAccessor(makeSbeObject(makeRecord()));
    const BSONObj values = BSON("computed" << 42.5);
    auto computed = makeAccessor(values["computed"]);
    ResultMaterializer materializer(
        &base, false, {{"computed", ResultFieldAction::kSet, &computed}});
    materialize(state, materializer, output);
}

// An inclusion projection of fields spread across the record with a computed field.
void BM_RecordClosedKeepSet(benchmark::State& state, Output output) {
    const BSONObj record = makeRecord();
    value::BSONObjValueAccessor base;
    const BSONObj values = BSON("total" << 1234.5);
    auto total = makeAccessor(values["total"]);
    ResultMaterializer materializer(&base,
                                    true,
                                    {{"_id", ResultFieldAction::kKeep},
                                     {"f9", ResultFieldAction::kKeep},
                                     {"f18", ResultFieldAction::kKeep},
                                     {"total", ResultFieldAction::kSet, &total}});
    materialize(state, materializer, output, [&] { base.reset(viewOf(record)); });
}

// An exclusion projection of fields spread across the record.
void BM_RecordOpenDrop(benchmark::State& state, Output output) {
    const BSONObj record = makeRecord();
    value::BSONObjValueAccessor base;
    ResultMaterializer materializer(&base,
                                    false,
                                    {{"f3", ResultFieldAction::kDrop},
                                     {"f9", ResultFieldAction::kDrop},
                                     {"f18", ResultFieldAction::kDrop}});
    materialize(state, materializer, output, [&] { base.reset(viewOf(record)); });
}

// An $addFields that replaces two fields of the record in place and appends a new one.
void BM_RecordOpenSet(benchmark::State& state, Output output) {
    const BSONObj record = makeRecord();
    value::BSONObjValueAccessor base;
    const BSONObj values =
        BSON("f5" << 500 << "f14" << "a replaced string value" << "total" << 1234.5);
    auto f5 = makeAccessor(values["f5"]);
    auto f14 = makeAccessor(values["f14"]);
    auto total = makeAccessor(values["total"]);
    ResultMaterializer materializer(&base,
                                    false,
                                    {{"f5", ResultFieldAction::kSet, &f5},
                                     {"f14", ResultFieldAction::kSet, &f14},
                                     {"total", ResultFieldAction::kSet, &total}});
    materialize(state, materializer, output, [&] { base.reset(viewOf(record)); });
}

// A $lookup-$unwind result: the local record with the foreign record embedded as a new field.
void BM_RecordSetRecordField(benchmark::State& state, Output output) {
    const BSONObj local = makeRecord();
    const BSONObj foreign = makeRecord();
    value::BSONObjValueAccessor base;
    value::BSONObjValueAccessor joined;
    ResultMaterializer materializer(&base, false, {{"joined", ResultFieldAction::kSet, &joined}});
    materialize(state, materializer, output, [&] {
        base.reset(viewOf(local));
        joined.reset(viewOf(foreign));
    });
}

// A $group result: the group key and its accumulators, including a $push array.
void BM_GroupAdd(benchmark::State& state, Output output) {
    value::OwnedValueAccessor base;
    const BSONObj values =
        BSON("_id" << "a group key" << "count" << 1000LL << "sum" << 12345.5 << "avg" << 12.3455);
    auto id = makeAccessor(values["_id"]);
    auto count = makeAccessor(values["count"]);
    auto sum = makeAccessor(values["sum"]);
    auto avg = makeAccessor(values["avg"]);

    auto pushed = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto* arr = value::getArrayView(pushed.value());
    for (int32_t i = 0; i < 10; ++i) {
        arr->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(i));
    }
    auto items = makeAccessor(std::move(pushed));

    ResultMaterializer materializer(&base,
                                    true,
                                    {{"_id", ResultFieldAction::kAdd, &id},
                                     {"count", ResultFieldAction::kAdd, &count},
                                     {"sum", ResultFieldAction::kAdd, &sum},
                                     {"avg", ResultFieldAction::kAdd, &avg},
                                     {"items", ResultFieldAction::kAdd, &items}});
    materialize(state, materializer, output);
}

BENCHMARK_CAPTURE(BM_RecordNoChanges, Bson, Output::kBson);
BENCHMARK_CAPTURE(BM_RecordNoChanges, Document, Output::kDocument);
BENCHMARK_CAPTURE(BM_OwnedBsonNoChanges, Bson, Output::kBson);
BENCHMARK_CAPTURE(BM_OwnedBsonNoChanges, Document, Output::kDocument);
BENCHMARK_CAPTURE(BM_SbeObjectSetNewField, Bson, Output::kBson);
BENCHMARK_CAPTURE(BM_SbeObjectSetNewField, Document, Output::kDocument);
BENCHMARK_CAPTURE(BM_RecordClosedKeepSet, Bson, Output::kBson);
BENCHMARK_CAPTURE(BM_RecordClosedKeepSet, Document, Output::kDocument);
BENCHMARK_CAPTURE(BM_RecordOpenDrop, Bson, Output::kBson);
BENCHMARK_CAPTURE(BM_RecordOpenDrop, Document, Output::kDocument);
BENCHMARK_CAPTURE(BM_RecordOpenSet, Bson, Output::kBson);
BENCHMARK_CAPTURE(BM_RecordOpenSet, Document, Output::kDocument);
BENCHMARK_CAPTURE(BM_RecordSetRecordField, Bson, Output::kBson);
BENCHMARK_CAPTURE(BM_RecordSetRecordField, Document, Output::kDocument);
BENCHMARK_CAPTURE(BM_GroupAdd, Bson, Output::kBson);
BENCHMARK_CAPTURE(BM_GroupAdd, Document, Output::kDocument);

}  // namespace
}  // namespace mongo::sbe
