// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/match_processor.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/matchable.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

namespace mongo {
namespace {

MatchProcessor makeProcessor(boost::intrusive_ptr<ExpressionContext> expCtx,
                             const BSONObj& predicate) {
    auto expr = uassertStatusOK(
        MatchExpressionParser::parse(predicate,
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));
    expr = optimizeMatchExpression(std::move(expr));
    // Compute field dependencies exactly as production does, so the projection (slow) path
    // serializes the fields the predicate actually needs. Without this the slow-path tests would
    // project an empty document.
    DepsTracker deps;
    dependency_analysis::addDependencies(expr.get(), &deps);
    return MatchProcessor(std::move(expr), std::move(deps), predicate.getOwned());
}

// Builds a processor whose dependencies demand the whole document, as $where and $text do.
MatchProcessor makeWholeDocumentProcessor(boost::intrusive_ptr<ExpressionContext> expCtx,
                                          const BSONObj& predicate) {
    auto expr = uassertStatusOK(
        MatchExpressionParser::parse(predicate,
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));
    expr = optimizeMatchExpression(std::move(expr));
    DepsTracker deps;
    dependency_analysis::addDependencies(expr.get(), &deps);
    deps.needWholeDocument = true;
    return MatchProcessor(std::move(expr), std::move(deps), predicate.getOwned());
}

// Constructs a Document from a BSONObj and verifies it is trivially convertible (i.e. the
// backing BSONObj fast path in MatchProcessor::process will be taken).
Document trivialDoc(const BSONObj& bson) {
    Document doc(bson);
    ASSERT(doc.toBsonIfTriviallyConvertible().has_value());
    return doc;
}

// Constructs a Document that has been mutated so it is NOT trivially convertible, forcing the
// serialization slow path in MatchProcessor::process.
Document modifiedDoc(const BSONObj& base, std::string_view addedField, int addedValue) {
    MutableDocument mut{Document{base}};
    mut.setField(addedField, Value(addedValue));
    Document doc = mut.freeze();
    ASSERT_FALSE(doc.toBsonIfTriviallyConvertible().has_value());
    return doc;
}

class MatchProcessorTest : public unittest::Test {
public:
    MatchProcessorTest() : _expCtx(new ExpressionContextForTest()) {}

protected:
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};

TEST_F(MatchProcessorTest, TrivialDocMatchesEqualityPredicate) {
    auto processor = makeProcessor(_expCtx, fromjson("{a: 1}"));
    ASSERT_TRUE(processor.process(trivialDoc(fromjson("{a: 1, b: 2}"))));
    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{a: 2, b: 2}"))));
}

TEST_F(MatchProcessorTest, ModifiedDocMatchesEqualityPredicate) {
    auto processor = makeProcessor(_expCtx, fromjson("{a: 1}"));
    ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{a: 1}"), "extra"sv, 99)));
    ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{a: 2}"), "extra"sv, 99)));
}

TEST_F(MatchProcessorTest, TrivialDocMatchesDottedPathPredicate) {
    auto processor = makeProcessor(_expCtx, fromjson("{'x.y': 5}"));
    ASSERT_TRUE(processor.process(trivialDoc(fromjson("{x: {y: 5}, z: 1}"))));
    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{x: {y: 6}, z: 1}"))));
}

TEST_F(MatchProcessorTest, TrivialDocMatchesArrayElementPredicate) {
    auto processor = makeProcessor(_expCtx, fromjson("{a: 2}"));
    ASSERT_TRUE(processor.process(trivialDoc(fromjson("{a: [1, 2, 3]}"))));
    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{a: [4, 5, 6]}"))));
}

TEST_F(MatchProcessorTest, TrivialDocMatchesRangePredicate) {
    auto processor = makeProcessor(_expCtx, fromjson("{price: {$gte: 50, $lt: 100}}"));
    ASSERT_TRUE(processor.process(trivialDoc(fromjson("{price: 75, name: 'widget'}"))));
    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{price: 25, name: 'widget'}"))));
    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{price: 100, name: 'widget'}"))));
}

TEST_F(MatchProcessorTest, TrivialDocMatchesExprPredicate) {
    auto processor = makeProcessor(_expCtx, fromjson("{$expr: {$eq: ['$a', '$b']}}"));
    ASSERT_TRUE(processor.process(trivialDoc(fromjson("{a: 3, b: 3}"))));
    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{a: 3, b: 4}"))));
}

TEST_F(MatchProcessorTest, TrivialDocMissingFieldDoesNotMatch) {
    auto processor = makeProcessor(_expCtx, fromjson("{a: 1}"));
    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{b: 1}"))));
}

TEST_F(MatchProcessorTest, TrivialDocExtraFieldsIgnored) {
    // Verifies that the fast path correctly matches against the full backing BSON even when there
    // are many fields beyond the ones referenced by the predicate.
    auto processor = makeProcessor(_expCtx, fromjson("{target: 42}"));
    auto bson = fromjson("{a: 1, b: 2, c: 3, d: 4, e: 5, target: 42, f: 6}");
    ASSERT_TRUE(processor.process(trivialDoc(bson)));
    auto bsonNoMatch = fromjson("{a: 1, b: 2, c: 3, d: 4, e: 5, target: 0, f: 6}");
    ASSERT_FALSE(processor.process(trivialDoc(bsonNoMatch)));
}

// --- Slow path (DocumentMatchableDocument) tests ---
// These all use modifiedDoc() to force the slow path.

TEST_F(MatchProcessorTest, SlowPathMatchesOriginalField) {
    // Field being matched is in the backing BSON (zero-allocation path within
    // DocumentMatchableDocument), not the inserted field.
    auto processor = makeProcessor(_expCtx, fromjson("{price: {$gte: 50}}"));
    ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{price: 75}"), "extra"sv, 1)));
    ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{price: 25}"), "extra"sv, 1)));
}

TEST_F(MatchProcessorTest, SlowPathMatchesInsertedField) {
    // The field being matched is the one added to the Document cache (kInserted kind),
    // exercising the serialization path within DocumentMatchableDocument.
    auto processor = makeProcessor(_expCtx, fromjson("{extra: 99}"));
    ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{a: 1}"), "extra"sv, 99)));
    ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{a: 1}"), "extra"sv, 0)));
}

TEST_F(MatchProcessorTest, SlowPathMatchesDottedPath) {
    // Dotted-path match on a field that lives in the backing BSON of a modified document.
    auto processor = makeProcessor(_expCtx, fromjson("{'x.y': 7}"));
    ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{x: {y: 7}}"), "z"sv, 1)));
    ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{x: {y: 8}}"), "z"sv, 1)));
}

TEST_F(MatchProcessorTest, SlowPathMatchesArrayElement) {
    auto processor = makeProcessor(_expCtx, fromjson("{vals: 3}"));
    ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{vals: [1, 2, 3]}"), "extra"sv, 0)));
    ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{vals: [4, 5, 6]}"), "extra"sv, 0)));
}

TEST_F(MatchProcessorTest, SlowPathMissingFieldDoesNotMatch) {
    auto processor = makeProcessor(_expCtx, fromjson("{a: 1}"));
    ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{b: 1}"), "extra"sv, 0)));
}

TEST_F(MatchProcessorTest, SlowPathExprPredicate) {
    auto processor = makeProcessor(_expCtx, fromjson("{$expr: {$eq: ['$a', '$b']}}"));
    ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{a: 5, b: 5}"), "extra"sv, 0)));
    ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{a: 5, b: 6}"), "extra"sv, 0)));
}

TEST_F(MatchProcessorTest, SlowPathAndShortCircuit) {
    // Both predicates reference fields in backing BSON. When the first fails, the second is
    // never evaluated. Verify correctness regardless of which predicate fails first.
    auto processor = makeProcessor(_expCtx, fromjson("{a: 1, b: 2}"));
    ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{a: 1, b: 2}"), "extra"sv, 0)));
    ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{a: 9, b: 2}"), "extra"sv, 0)));
    ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{a: 1, b: 9}"), "extra"sv, 0)));
    ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{a: 9, b: 9}"), "extra"sv, 0)));
}

// A trivially-convertible document larger than the whole-document match size gate must fall back to
// the projection path and still match correctly.
TEST_F(MatchProcessorTest, LargeTrivialDocFallsBackToProjectionPath) {
    auto processor = makeProcessor(_expCtx, fromjson("{target: 7}"));
    const std::string big(32 * 1024, 'x');  // exceeds the 16KB whole-document match gate
    ASSERT_TRUE(processor.process(trivialDoc(BSON("pad" << big << "target" << 7))));
    ASSERT_FALSE(processor.process(trivialDoc(BSON("pad" << big << "target" << 8))));
}

// The core correctness guarantee of the whole-document fast path: it must produce the same result
// as the projection (slow) path for the same logical document, across a range of predicate shapes
// and values (including dotted paths, arrays, $expr, and missing/null fields). 'modifiedDoc' adds a
// field the predicates never reference, so it only changes the document's backing (forcing the slow
// path), not the logical match result.
TEST_F(MatchProcessorTest, FastPathAgreesWithSlowPath) {
    struct Case {
        std::string predicate;
        std::string doc;
    };
    const std::vector<Case> cases = {
        {"{a: 1}", "{a: 1, b: 2}"},
        {"{a: 1}", "{a: 2}"},
        {"{a: 1}", "{b: 2}"},
        {"{'x.y': 5}", "{x: {y: 5}, z: 1}"},
        {"{'x.y': 5}", "{x: {y: 6}}"},
        {"{a: 2}", "{a: [1, 2, 3]}"},
        {"{a: 2}", "{a: [4, 5, 6]}"},
        {"{price: {$gte: 50, $lt: 100}}", "{price: 75}"},
        {"{price: {$gte: 50, $lt: 100}}", "{price: 25}"},
        {"{$expr: {$eq: ['$a', '$b']}}", "{a: 3, b: 3}"},
        {"{$expr: {$eq: ['$a', '$b']}}", "{a: 3, b: 4}"},
        {"{$expr: {$eq: ['$a', '$b']}}", "{c: 1}"},
        {"{$expr: {$eq: ['$a', '$b']}}", "{a: null, b: null}"},
        {"{$expr: {$eq: ['$a', '$b']}}", "{a: 1}"},
    };
    for (const auto& c : cases) {
        auto processor = makeProcessor(_expCtx, fromjson(c.predicate));
        const BSONObj doc = fromjson(c.doc);
        const bool fastResult = processor.process(trivialDoc(doc));
        const bool slowResult =
            processor.process(modifiedDoc(doc, "unreferencedExtraField"sv, 12345));
        ASSERT_EQ(fastResult, slowResult)
            << "fast/slow path disagree; predicate=" << c.predicate << " doc=" << c.doc;
    }
}

// --- BSONMatchableDocument::reset tests ---
// SERVER-131797: MatchProcessor reuses a BSONMatchableDocument across process() calls. These
// tests verify the reset semantics that the reuse relies on.

TEST(MatchableDocumentTest, ResetRebindsToNewBsonObj) {
    BSONMatchableDocument doc(BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(doc.toBSON(), BSON("a" << 1));
    doc.reset(BSON("b" << 2));
    ASSERT_BSONOBJ_EQ(doc.toBSON(), BSON("b" << 2));
}

TEST(MatchableDocumentTest, ResetMoveRebindsToNewBsonObj) {
    BSONMatchableDocument doc(BSON("a" << 1));
    BSONObj moved = BSON("c" << 3).getOwned();
    doc.reset(std::move(moved));
    ASSERT_BSONOBJ_EQ(doc.toBSON(), BSON("c" << 3));
}

TEST(MatchableDocumentTest, ResetManyTimesReflectsLatestBsonObj) {
    BSONMatchableDocument doc(BSONObj{});
    for (int i = 0; i < 100; i++) {
        doc.reset(BSON("x" << i));
        ASSERT_EQUALS(i, doc.toBSON().firstElement().numberInt());
    }
}

TEST(MatchableDocumentTest, SourceDocumentIsOptional) {
    Document doc{BSON("a" << 1 << "b" << 2)};
    BSONMatchableDocument withSource(BSON("a" << 1), &doc);
    ASSERT_BSONOBJ_EQ(withSource.toBSON(), BSON("a" << 1));
    ASSERT_TRUE(withSource.getSourceDocument().has_value());
    ASSERT_BSONOBJ_EQ(withSource.getSourceDocument()->toBson(), BSON("a" << 1 << "b" << 2));

    BSONMatchableDocument withoutSource(BSON("a" << 1));
    ASSERT_FALSE(withoutSource.getSourceDocument().has_value());

    // reset() rebinds the source along with the BSON.
    withSource.reset(BSON("c" << 3));
    ASSERT_FALSE(withSource.getSourceDocument().has_value());
    withSource.reset(BSON("c" << 3), &doc);
    ASSERT_TRUE(withSource.getSourceDocument().has_value());
}

// --- Reuse staleness tests ---
// SERVER-131797: The reused BSONMatchableDocument must not go stale across repeated process()
// calls with varying document shapes.

TEST_F(MatchProcessorTest, ProcessRepeatedCallsSameShapeDoesNotGoStale) {
    auto processor = makeProcessor(_expCtx, fromjson("{array: 5}"));
    for (int i = 0; i < 1000; i++) {
        ASSERT_TRUE(processor.process(trivialDoc(fromjson("{array: 5}"))));
        ASSERT_FALSE(processor.process(trivialDoc(fromjson("{array: 6}"))));
    }
}

TEST_F(MatchProcessorTest, ProcessVaryingDocShapesDoesNotGoStale) {
    auto processor = makeProcessor(_expCtx, fromjson("{array: 5}"));

    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{}"))));
    ASSERT_TRUE(processor.process(trivialDoc(fromjson("{array: 5}"))));
    ASSERT_TRUE(processor.process(trivialDoc(fromjson("{array: [1, 2, 5, 6]}"))));
    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{array: [1, 2, 6]}"))));
    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{array: 6}"))));
    ASSERT_TRUE(processor.process(trivialDoc(fromjson("{array: 5, other: 999}"))));
    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{other: 999}"))));
    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{nested: {array: 5}}"))));
    ASSERT_TRUE(processor.process(trivialDoc(fromjson("{array: 5}"))));
}

TEST_F(MatchProcessorTest, ProcessLongerThanShorterDocDoesNotGoStale) {
    auto processor = makeProcessor(_expCtx, fromjson("{x: 1}"));
    ASSERT_TRUE(processor.process(trivialDoc(
        fromjson("{a: {b: {c: {d: {e: 1}}}}, x: 1, longField: 'aaaaaaaaaaaaaaaaaaaa'}"))));
    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{x: 2}"))));
    ASSERT_TRUE(processor.process(trivialDoc(fromjson("{x: 1}"))));
    ASSERT_FALSE(processor.process(trivialDoc(fromjson("{}"))));
    ASSERT_TRUE(processor.process(trivialDoc(
        fromjson("{a: {b: {c: {d: {e: 1}}}}, x: 1, longField: 'aaaaaaaaaaaaaaaaaaaa'}"))));
}

// --- Buffer reuse tests ---

// 'modifiedDoc' forces the serialization path, so these alternate document sizes and match results
// against one reused buffer.
TEST_F(MatchProcessorTest, BufferReuseAcrossVaryingDocSizes) {
    auto processor = makeProcessor(_expCtx, fromjson("{a: 5, b: {$gt: 10}}"));
    const BSONObj big = BSON("pad" << std::string(4096, 'x') << "a" << 5 << "b" << 20);
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{a: 5, b: 20}"), "z"sv, i)));
        ASSERT_TRUE(processor.process(modifiedDoc(big, "z"sv, i)));
        ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{a: 5, b: 5}"), "z"sv, i)));
    }
}

TEST_F(MatchProcessorTest, BufferReuseWithDependenciesOnSeparateFields) {
    auto processor = makeProcessor(_expCtx, fromjson("{$expr: {$eq: ['$a', '$b']}}"));
    const BSONObj big = BSON("pad" << std::string(4096, 'x') << "a" << 7 << "b" << 7);
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{a: 7, b: 7}"), "extra"sv, i)));
        ASSERT_TRUE(processor.process(modifiedDoc(big, "extra"sv, i)));
        ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{a: 7, b: 8}"), "extra"sv, i)));
    }
}

// A whole-document dependency serializes the entire input into the buffer rather than projecting
// the predicate's fields, so the reused buffer must also hold fields the predicate never reads.
TEST_F(MatchProcessorTest, BufferReuseWithNeedWholeDocument) {
    auto processor = makeWholeDocumentProcessor(_expCtx, fromjson("{a: 7}"));
    const BSONObj big = BSON("pad" << std::string(4096, 'x') << "a" << 7);
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{a: 7, b: 1}"), "extra"sv, i)));
        ASSERT_TRUE(processor.process(modifiedDoc(big, "extra"sv, i)));
        ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{a: 8, b: 1}"), "extra"sv, i)));
    }
}

// Paths sharing a first field defeat the uniqueness optimization, selecting the slower
// serialization that checks for a repeated first field.
TEST_F(MatchProcessorTest, BufferReuseWithSharedFirstFieldDependencies) {
    auto processor = makeProcessor(_expCtx, fromjson("{'a.b': 1, 'a.c': 2}"));
    const BSONObj big = BSON("pad" << std::string(4096, 'x') << "a" << BSON("b" << 1 << "c" << 2));
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{a: {b: 1, c: 2}}"), "extra"sv, i)));
        ASSERT_TRUE(processor.process(modifiedDoc(big, "extra"sv, i)));
        ASSERT_FALSE(processor.process(modifiedDoc(fromjson("{a: {b: 1, c: 3}}"), "extra"sv, i)));
    }
}

TEST(BSONObjBuilderTest, AsTempObjUsesLargeSizeTrait) {
    const std::string bigStr(20 * 1024 * 1024, 'x');
    BSONObjBuilder bob;
    bob.append("pad", bigStr);
    BSONObj temp = bob.asTempObj();
    ASSERT_GT(temp.objsize(), BSONObjMaxInternalSize);
    ASSERT_EQ(temp.firstElement().str(), bigStr);
}

// process() must handle a serialized doc past BSONObjMaxInternalSize without crashing.
TEST_F(MatchProcessorTest, ProcessMatchesDocExceedingBSONObjMaxInternalSize) {
    auto processor = makeWholeDocumentProcessor(_expCtx, fromjson("{target: 7}"));
    const std::string bigStr(20 * 1024 * 1024, 'x');

    auto matchingDoc = modifiedDoc(BSON("pad" << bigStr << "target" << 7), "extra"sv, 0);
    ASSERT_TRUE(processor.process(matchingDoc));
    ASSERT_GT(processor.bufferCapacity(), BSONObjMaxInternalSize);

    auto nonMatchingDoc = modifiedDoc(BSON("pad" << bigStr << "target" << 8), "extra"sv, 0);
    ASSERT_FALSE(processor.process(nonMatchingDoc));
}

// Same, but via the projection path instead of whole-document.
TEST_F(MatchProcessorTest, ProjectionPathMatchesDocExceedingBSONObjMaxInternalSize) {
    auto processor = makeProcessor(
        _expCtx, fromjson("{pad1: {$exists: true}, pad2: {$exists: true}, target: 7}"));
    const std::string bigStr(9 * 1024 * 1024, 'x');

    auto matchingDoc =
        modifiedDoc(BSON("pad1" << bigStr << "pad2" << bigStr << "target" << 7), "extra"sv, 0);
    ASSERT_TRUE(processor.process(matchingDoc));
    ASSERT_GT(processor.bufferCapacity(), BSONObjMaxInternalSize);

    auto nonMatchingDoc =
        modifiedDoc(BSON("pad1" << bigStr << "pad2" << bigStr << "target" << 8), "extra"sv, 0);
    ASSERT_FALSE(processor.process(nonMatchingDoc));
}

// process() throws code on doc larger than BufferMaxSize (125MB).
TEST_F(MatchProcessorTest, ProcessThrowsWhenDocExceedsBufferMaxSize) {
    auto processor = makeWholeDocumentProcessor(_expCtx, fromjson("{target: 7}"));
    const std::string bigStr(15 * 1024 * 1024, 'x');

    MutableDocument mut;
    mut.setField("target", Value(7));
    for (int i = 0; i < 9; ++i) {
        mut.setField(std::to_string(i), Value(bigStr));
    }
    Document hugeDoc = mut.freeze();
    ASSERT_FALSE(hugeDoc.toBsonIfTriviallyConvertible().has_value());

    ASSERT_THROWS_CODE(processor.process(hugeDoc), AssertionException, 13548);
}

// --- Buffer memory tracking tests ---

TEST_F(MatchProcessorTest, TrackerChargesBufferGrowthHighWaterMark) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{1024 * 1024}};
    EvaluationContext ctx;
    ctx.tracker = &tracker;

    auto processor = makeProcessor(_expCtx, fromjson("{pad: {$exists: true}, target: 1}"));

    // Small doc: the tracker charges the buffer's whole initial allocation, not the bytes written.
    processor.process(modifiedDoc(fromjson("{target: 1, pad: 'short'}"), "extra"sv, 0), ctx);
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), processor.bufferCapacity());
    auto afterSmall = tracker.inUseTrackedMemoryBytes();

    // Same-size doc: no growth, tracker should not change.
    processor.process(modifiedDoc(fromjson("{target: 1, pad: 'short'}"), "extra"sv, 0), ctx);
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), afterSmall);

    // Larger doc: buffer grows to fit the bigger projected field, tracker charges the delta.
    const std::string big(4096, 'x');
    processor.process(modifiedDoc(BSON("pad" << big << "target" << 1), "extra"sv, 0), ctx);
    ASSERT_GT(tracker.inUseTrackedMemoryBytes(), afterSmall);

    // Smaller doc after a large one: buffer retains capacity, no new charge.
    auto afterBig = tracker.inUseTrackedMemoryBytes();
    processor.process(modifiedDoc(fromjson("{target: 1, pad: 'short'}"), "extra"sv, 0), ctx);
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), afterBig);
}

// The buffer holds its grown allocation after a small document overwrites a large one, so the
// charge must stay at the capacity rather than falling back to the bytes written.
TEST_F(MatchProcessorTest, TrackerChargesCapacityNotBytesWritten) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{1024 * 1024}};
    EvaluationContext ctx;
    ctx.tracker = &tracker;

    auto processor = makeProcessor(_expCtx, fromjson("{pad: {$exists: true}, target: 1}"));

    const std::string big(4096, 'x');
    processor.process(modifiedDoc(BSON("pad" << big << "target" << 1), "extra"sv, 0), ctx);
    const int64_t capacity = processor.bufferCapacity();
    ASSERT_GT(capacity, 4096);

    processor.process(modifiedDoc(fromjson("{target: 1, pad: 'short'}"), "extra"sv, 0), ctx);
    ASSERT_EQ(processor.bufferCapacity(), capacity);
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), capacity);
}

TEST_F(MatchProcessorTest, TrackerNotUpdatedWhenNull) {
    auto processor = makeProcessor(_expCtx, fromjson("{target: 1}"));
    // No tracker in EvaluationContext — must not crash.
    ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{target: 1}"), "extra"sv, 0)));
    ASSERT_TRUE(processor.process(
        modifiedDoc(BSON("pad" << std::string(4096, 'x') << "target" << 1), "extra"sv, 0)));
}

TEST_F(MatchProcessorTest, ReleaseBufferDeductsTrackerAndFreesMemory) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{1024 * 1024}};
    EvaluationContext ctx;
    ctx.tracker = &tracker;

    auto processor = makeProcessor(_expCtx, fromjson("{pad: {$exists: true}, target: 1}"));

    const std::string big(4096, 'x');
    processor.process(modifiedDoc(BSON("pad" << big << "target" << 1), "extra"sv, 0), ctx);
    ASSERT_GT(tracker.inUseTrackedMemoryBytes(), 0);

    processor.releaseBuffer(&tracker);
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);

    // Buffer is recreated on next process() call.
    processor.process(modifiedDoc(fromjson("{target: 1, pad: 'short'}"), "extra"sv, 0), ctx);
    ASSERT_GT(tracker.inUseTrackedMemoryBytes(), 0);
}

// --- Buffer lifetime tests ---

TEST_F(MatchProcessorTest, BufferAllocatedOnlyForSerializationAndFreedOnRelease) {
    auto processor = makeProcessor(_expCtx, fromjson("{target: 1}"));
    ASSERT_EQ(processor.bufferCapacity(), 0);

    // The fast path never allocates the buffer.
    ASSERT_TRUE(processor.process(trivialDoc(fromjson("{target: 1}"))));
    ASSERT_EQ(processor.bufferCapacity(), 0);

    ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{target: 1}"), "extra"sv, 0)));
    ASSERT_GT(processor.bufferCapacity(), 0);

    // Taking the fast path must not drop the buffer; a workload alternating between the two paths
    // would otherwise allocate per document.
    ASSERT_TRUE(processor.process(trivialDoc(fromjson("{target: 1}"))));
    ASSERT_GT(processor.bufferCapacity(), 0);

    processor.releaseBuffer(nullptr);
    ASSERT_EQ(processor.bufferCapacity(), 0);

    ASSERT_TRUE(processor.process(modifiedDoc(fromjson("{target: 1}"), "extra"sv, 0)));
    ASSERT_GT(processor.bufferCapacity(), 0);
}

}  // namespace
}  // namespace mongo
