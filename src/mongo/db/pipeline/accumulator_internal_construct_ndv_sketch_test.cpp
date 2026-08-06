// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/accumulator_internal_construct_ndv_sketch.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/ce/ndv/hyperloglog.h"
#include "mongo/db/query/compiler/ce/ndv/ndv_sketch_gen.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace mongo {
namespace {

class InternalConstructNdvSketchTest : public unittest::Test {
protected:
    boost::intrusive_ptr<AccumulatorInternalConstructNdvSketch> makeAccumulator(
        std::vector<std::string> fields = {"a"}) {
        InternalConstructNdvSketchAccumulatorParams params;
        params.setVal("$$ROOT");
        params.setFields(std::move(fields));
        return make_intrusive<AccumulatorInternalConstructNdvSketch>(_expCtx.get(), params);
    }

    // Feeds 'value' as field "a" of a document, matching the {val: <document>} input the
    // accumulator receives from $group. A missing 'value' yields a document without the field.
    void add(AccumulatorInternalConstructNdvSketch& accumulator, const Value& value) {
        MutableDocument doc;
        if (!value.missing()) {
            doc.addField("a", value);
        }
        accumulator.process(Value(Document{{"val", doc.freezeToValue()}}), false /* merging */);
    }

    Value sketches(AccumulatorInternalConstructNdvSketch& accumulator) {
        return accumulator.getValue(false /* toBeMerged */);
    }

    long long ndv(AccumulatorInternalConstructNdvSketch& accumulator) {
        return sketches(accumulator)[0][NdvSketch::kNdvFieldName].coerceToLong();
    }

    QueryTestServiceContext _serviceContext;
    ServiceContext::UniqueOperationContext _opCtx = _serviceContext.makeOperationContext();
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx =
        make_intrusive<ExpressionContextForTest>(_opCtx.get());
};

TEST_F(InternalConstructNdvSketchTest, ParsesHistogramStyleSyntax) {
    // Same shape as $_internalConstructStats: 'val' evaluates to the document, 'fields' names the
    // paths the accumulator extracts itself, so absent fields arrive as missing.
    const BSONObj spec =
        BSON("sketch" << BSON("$_internalConstructNdvSketch"
                              << BSON("val" << "$$ROOT" << "fields" << BSON_ARRAY("a"))));
    auto statement = AccumulationStatement::parseAccumulationStatement(
        _expCtx.get(), spec.firstElement(), _expCtx->variablesParseState);
    auto accumulator = statement.makeAccumulator();
    ASSERT_EQ(std::string(accumulator->getOpName()), "$_internalConstructNdvSketch");

    // Parsing must force the classic engine.
    ASSERT(_expCtx->getSbeCompatibility() == SbeCompatibility::notCompatible);

    const auto& argument = statement.expr.argument;
    accumulator->process(argument->evaluate(Document{{"a", 5}}, &_expCtx->variables), false);
    accumulator->process(argument->evaluate(Document{}, &_expCtx->variables), false);
    accumulator->process(argument->evaluate(Document{}, &_expCtx->variables), false);
    // Two distinct values: 5 and missing.
    ASSERT_EQ(
        accumulator->getValue(false /* toBeMerged */)[0][NdvSketch::kNdvFieldName].coerceToLong(),
        2);
}

TEST_F(InternalConstructNdvSketchTest, CountsDistinctValuesNearExactly) {
    auto accumulator = makeAccumulator();
    // Small cardinalities are near-exact. Repeats must not change the count.
    constexpr int kDistinct = 1000;
    for (int repetition = 0; repetition < 3; ++repetition) {
        for (int i = 0; i < kDistinct; ++i) {
            add(*accumulator, Value(i));
        }
    }
    ASSERT_APPROX_EQUAL(static_cast<double>(ndv(*accumulator)), kDistinct, 0.02 * kDistinct);
}

TEST_F(InternalConstructNdvSketchTest, DistinctnessFollowsWoCompareSemantics) {
    auto accumulator = makeAccumulator();
    // All numeric representations of 1 count as a single distinct value.
    add(*accumulator, Value(1));
    add(*accumulator, Value(1.0));
    add(*accumulator, Value(1LL));
    ASSERT_EQ(ndv(*accumulator), 1);
}

TEST_F(InternalConstructNdvSketchTest, ExtractsDottedPaths) {
    auto accumulator = makeAccumulator({"a.b"});
    accumulator->process(
        Value(Document{{"val", Value(Document{{"a", Value(Document{{"b", 1}})}})}}), false);
    accumulator->process(
        Value(Document{{"val", Value(Document{{"a", Value(Document{{"b", 2}})}})}}), false);
    // A document without the nested field counts as missing.
    accumulator->process(Value(Document{{"val", Value(Document{})}}), false);
    ASSERT_EQ(ndv(*accumulator), 3);
}

TEST_F(InternalConstructNdvSketchTest, NullMissingAndUndefinedBucketing) {
    auto accumulator = makeAccumulator();
    // Null counts once; missing and Undefined count as one value together (woCompare-equal).
    add(*accumulator, Value(BSONNULL));
    add(*accumulator, Value());  // Missing.
    add(*accumulator, Value(BSONUndefined));
    ASSERT_EQ(ndv(*accumulator), 2);
}

TEST_F(InternalConstructNdvSketchTest, RejectsArrayValues) {
    auto accumulator = makeAccumulator();
    const Value array{std::vector<Value>{Value(1), Value(2)}};
    ASSERT_THROWS_CODE(add(*accumulator, array), DBException, 13175701);
}

TEST_F(InternalConstructNdvSketchTest, RejectsArraysAlongDottedPaths) {
    auto accumulator = makeAccumulator({"a.b"});
    const Value doc{Document{
        {"val", Value(Document{{"a", Value(std::vector<Value>{Value(Document{{"b", 1}})})}})}}};
    ASSERT_THROWS_CODE(accumulator->process(doc, false /* merging */), DBException, 13175701);
}

TEST_F(InternalConstructNdvSketchTest, RejectsMerging) {
    auto accumulator = makeAccumulator();
    ASSERT_THROWS_CODE(accumulator->process(Value(1), true /* merging */), DBException, 13175700);
    ASSERT_THROWS_CODE(accumulator->getValue(true /* toBeMerged */), DBException, 13175702);
}

TEST_F(InternalConstructNdvSketchTest, NonDocumentValDegradesToMissing) {
    auto accumulator = makeAccumulator();
    // A spec whose 'val' evaluates to a scalar must not crash; the path walk yields missing.
    accumulator->process(Value(Document{{"val", Value(7)}}), false /* merging */);
    add(*accumulator, Value());  // Missing.
    ASSERT_EQ(ndv(*accumulator), 1);
}

TEST_F(InternalConstructNdvSketchTest, OutputRoundTripsIntoSketch) {
    auto accumulator = makeAccumulator();
    constexpr int kDistinct = 5000;
    for (int i = 0; i < kDistinct; ++i) {
        add(*accumulator, Value(i));
    }
    const auto result = sketches(*accumulator);

    // Pin the persisted schema: an array of sketch documents, one per folding variant (a single
    // field has exactly one). The strict IDL parse rejects any drift.
    ASSERT_EQ(result.getType(), BSONType::array);
    ASSERT_EQ(result.getArrayLength(), 1);
    ASSERT_EQ(result[0][NdvSketch::kNdvFieldName].getType(), BSONType::numberLong);
    const auto sketch =
        NdvSketch::parse(result[0].getDocument().toBson(), IDLParserContext("NdvSketch"));
    ASSERT_EQ(sketch.getPrecision(), 14);
    const auto registers = sketch.getRegisters();
    ASSERT_EQ(registers.length(), 1 << 14);
    auto swSketch = ce::HyperLogLog::create(
        static_cast<size_t>(sketch.getPrecision()),
        ce::HyperLogLog::Registers(reinterpret_cast<const uint8_t*>(registers.data()),
                                   registers.length()));
    ASSERT_OK(swSketch.getStatus());
    ASSERT_EQ(static_cast<long long>(std::llround(swSketch.getValue().estimate())),
              sketch.getNdv());
}

TEST_F(InternalConstructNdvSketchTest, TracksConstantSketchMemory) {
    auto accumulator = makeAccumulator();
    // The sketch owns 2^14 single-byte registers.
    const auto initial = accumulator->getMemUsage();
    ASSERT_GTE(initial, 16384);

    // Memory must not grow with input volume.
    for (int i = 0; i < 100'000; ++i) {
        add(*accumulator, Value(i));
    }
    ASSERT_EQ(accumulator->getMemUsage(), initial);
}

TEST_F(InternalConstructNdvSketchTest, ResetClearsState) {
    auto accumulator = makeAccumulator();
    add(*accumulator, Value(1));
    accumulator->reset();
    ASSERT_EQ(ndv(*accumulator), 0);
}

}  // namespace
}  // namespace mongo
