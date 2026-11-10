// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_convert_errors_stage.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_change_stream_convert_errors.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using namespace std::literals::string_view_literals;

// An event identity as the transform records it: the resume token is both '_id' and the
// single-element sort key.
Document recordedIdentity(Value token) {
    MutableDocument md(Document{{"_id"sv, token}});
    md.metadata().setSortKey(token, true /* isSingleElementKey */);
    return md.freeze();
}

/**
 * A stage that always fails with the given error code, modelling a failure raised by a stage below
 * the top-level conversion stage.
 */
class ThrowingStage final : public exec::agg::Stage {
public:
    ThrowingStage(const boost::intrusive_ptr<ExpressionContext>& expCtx, ErrorCodes::Error code)
        : Stage("$throwing", expCtx), _code(code) {}

    exec::agg::GetNextResult doGetNext() final {
        uasserted(_code, "stage failure");
        MONGO_UNREACHABLE;
    }

private:
    ErrorCodes::Error _code;
};

class ChangeStreamConvertErrorsStageTest : public unittest::Test {
protected:
    boost::intrusive_ptr<exec::agg::ChangeStreamConvertErrorsStage> makeConverter() {
        return make_intrusive<exec::agg::ChangeStreamConvertErrorsStage>(
            "$_internalChangeStreamConvertErrors", _expCtx);
    }

    void enableErrorEvents() {
        DocumentSourceChangeStreamSpec spec;
        spec.setShowErrorsAsEvents(true);
        _expCtx->setChangeStreamSpec(spec);
    }

    boost::intrusive_ptr<ExpressionContext> _expCtx = make_intrusive<ExpressionContextForTest>();
};

TEST_F(ChangeStreamConvertErrorsStageTest, ConvertsEligibleFailureWithRecordedIdentity) {
    enableErrorEvents();
    // The identity the transform would have recorded for the event in flight.
    _expCtx->setActiveChangeStreamEvent(recordedIdentity(Value(Document{{"_id"sv, 42}})));

    auto throwing = make_intrusive<ThrowingStage>(_expCtx, ErrorCodes::TypeMismatch);
    auto converter = makeConverter();
    exec::agg::MockStage::setSource_forTest(converter, throwing.get());

    auto result = converter->getNext();
    ASSERT_TRUE(result.isAdvanced());
    const auto& doc = result.getDocument();
    ASSERT_EQ(doc["operationType"].getString(), "error");
    ASSERT_EQ(doc["error"]["code"].getInt(), static_cast<int>(ErrorCodes::TypeMismatch));
    ASSERT_EQ(doc["error"]["source"].getString(), "user-pipeline");
    // The identity is cleared after delivery, so a later failure is not blamed on this event.
    ASSERT_FALSE(_expCtx->getActiveChangeStreamEvent());
}

TEST_F(ChangeStreamConvertErrorsStageTest, RethrowsEligibleFailureWithoutIdentity) {
    enableErrorEvents();
    // No identity recorded: the failure cannot be attributed, so it must propagate (fail closed).

    auto throwing = make_intrusive<ThrowingStage>(_expCtx, ErrorCodes::TypeMismatch);
    auto converter = makeConverter();
    exec::agg::MockStage::setSource_forTest(converter, throwing.get());

    ASSERT_THROWS_CODE(converter->getNext(), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(ChangeStreamConvertErrorsStageTest, MaterializesPendingResumeGap) {
    enableErrorEvents();
    _expCtx->setPendingChangeStreamResumeGap(Document{{"gap"sv, true}});

    auto throwing = make_intrusive<ThrowingStage>(_expCtx, ErrorCodes::ChangeStreamHistoryLost);
    auto converter = makeConverter();
    exec::agg::MockStage::setSource_forTest(converter, throwing.get());

    auto result = converter->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_TRUE(result.getDocument()["gap"].getBool());
}

TEST_F(ChangeStreamConvertErrorsStageTest, ReleasesPendingRouterErrorEventOnBypass) {
    enableErrorEvents();
    _expCtx->setPendingChangeStreamRouterErrorEvent(Document{{"bypass"sv, true}});

    auto throwing =
        make_intrusive<ThrowingStage>(_expCtx, ErrorCodes::ChangeStreamErrorEventBypass);
    auto converter = makeConverter();
    exec::agg::MockStage::setSource_forTest(converter, throwing.get());

    auto result = converter->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_TRUE(result.getDocument()["bypass"].getBool());
}

// The bypass delivers an operationType:"error" event, which means the recorded identity and error
// source for the in-flight event have been consumed. They must be cleared so a later failure is
// never attributed to this (already-delivered) event.
TEST_F(ChangeStreamConvertErrorsStageTest, BypassClearsRecordedIdentity) {
    enableErrorEvents();
    _expCtx->setActiveChangeStreamEvent(Document{{"_id"sv, 42}});
    _expCtx->setActiveChangeStreamErrorSource(ChangeStreamErrorSource::kUserPipeline);
    _expCtx->setPendingChangeStreamRouterErrorEvent(Document{{"bypass"sv, true}});

    auto throwing =
        make_intrusive<ThrowingStage>(_expCtx, ErrorCodes::ChangeStreamErrorEventBypass);
    auto converter = makeConverter();
    exec::agg::MockStage::setSource_forTest(converter, throwing.get());

    auto result = converter->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_TRUE(result.getDocument()["bypass"].getBool());
    ASSERT_FALSE(_expCtx->getActiveChangeStreamEvent());
    ASSERT_FALSE(_expCtx->getActiveChangeStreamErrorSource());
}

// Same invariant for the resume-gap path: once the gap event is delivered, the recorded identity
// and error source must not survive to mislabel a later failure.
TEST_F(ChangeStreamConvertErrorsStageTest, ResumeGapClearsRecordedIdentity) {
    enableErrorEvents();
    _expCtx->setActiveChangeStreamEvent(Document{{"_id"sv, 42}});
    _expCtx->setActiveChangeStreamErrorSource(ChangeStreamErrorSource::kUserPipeline);
    _expCtx->setPendingChangeStreamResumeGap(Document{{"gap"sv, true}});

    auto throwing = make_intrusive<ThrowingStage>(_expCtx, ErrorCodes::ChangeStreamHistoryLost);
    auto converter = makeConverter();
    exec::agg::MockStage::setSource_forTest(converter, throwing.get());

    auto result = converter->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_TRUE(result.getDocument()["gap"].getBool());
    ASSERT_FALSE(_expCtx->getActiveChangeStreamEvent());
    ASSERT_FALSE(_expCtx->getActiveChangeStreamErrorSource());
}

TEST_F(ChangeStreamConvertErrorsStageTest, DoesNotConvertNonConvertibleError) {
    enableErrorEvents();
    _expCtx->setActiveChangeStreamEvent(recordedIdentity(Value(Document{{"_id"sv, 42}})));

    // An interruption is never converted, even with a recorded identity.
    auto throwing = make_intrusive<ThrowingStage>(_expCtx, ErrorCodes::Interrupted);
    auto converter = makeConverter();
    exec::agg::MockStage::setSource_forTest(converter, throwing.get());

    ASSERT_THROWS_CODE(converter->getNext(), AssertionException, ErrorCodes::Interrupted);
}

TEST_F(ChangeStreamConvertErrorsStageTest, DoesNotConvertTransientLookupFailure) {
    enableErrorEvents();
    // A lookup (pre-image, post-image or updateLookup) failed transiently on the event in flight:
    // the identity and the internal-pipeline source are set, as the lookup stage leaves them.
    _expCtx->setActiveChangeStreamEvent(recordedIdentity(Value(Document{{"_id"sv, 42}})));
    _expCtx->setActiveChangeStreamErrorSource(ChangeStreamErrorSource::kInternalPipeline);

    // The failure propagates so that a resume delivers the event, rather than being converted into
    // an error event that skips it.
    auto throwing = make_intrusive<ThrowingStage>(_expCtx, ErrorCodes::WriteConflict);
    auto converter = makeConverter();
    exec::agg::MockStage::setSource_forTest(converter, throwing.get());

    ASSERT_THROWS_CODE(converter->getNext(), DBException, ErrorCodes::WriteConflict);
}

TEST(ChangeStreamConvertErrorsDocumentSourceTest, SerializesForExplain) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto ds = DocumentSourceChangeStreamConvertErrors::create(expCtx);

    auto value = ds->doSerialize(
        query_shape::SerializationOptions{.verbosity = ExplainOptions::Verbosity::kQueryPlanner});

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$changeStream": {"stage": "$_internalChangeStreamConvertErrors"}})",
        value.getDocument().toBson());
}

TEST(ChangeStreamConvertErrorsDocumentSourceTest, SerializesForWire) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto ds = DocumentSourceChangeStreamConvertErrors::create(expCtx);

    ASSERT_BSONOBJ_EQ(ds->doSerialize().getDocument().toBson(),
                      BSON("$_internalChangeStreamConvertErrors" << BSONObj()));
}

// The stage must be the final stage of the pipeline: an error event is built there, above every
// user stage, and must never be observed by a user stage below it.
TEST(ChangeStreamConvertErrorsConstraintTest, MustBeFinalStage) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto converter = DocumentSourceChangeStreamConvertErrors::create(expCtx);
    auto mock = DocumentSourceMock::createForTest({Document{}}, expCtx);

    // The stage is the final source: valid.
    ASSERT_DOES_NOT_THROW(Pipeline::create({mock, converter}, expCtx));

    // The stage placed below another stage: invalid.
    ASSERT_THROWS_CODE(Pipeline::create({converter, mock}, expCtx), AssertionException, 40601);
}

}  // namespace
}  // namespace mongo
