// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/change_stream_add_pre_image_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

/**
 * A stub MongoProcessInterface whose local single-document lookup (used to fetch pre-images from
 * the pre-images collection) returns a configurable result and counts the number of lookups.
 */
class MockPreImageProcessInterface final : public StubMongoProcessInterface {
public:
    explicit MockPreImageProcessInterface(boost::optional<Document> preImageDoc)
        : _preImageDoc(std::move(preImageDoc)) {}

    boost::optional<Document> lookupSingleDocumentLocally(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const Document& documentKey) override {
        ++_lookupCount;
        return _preImageDoc;
    }

    int lookupCount() const {
        return _lookupCount;
    }

private:
    boost::optional<Document> _preImageDoc;
    int _lookupCount = 0;
};

class DocumentSourceChangeStreamAddPreImageTest : public AggregationContextFixture {
public:
    static const UUID& testUuid() {
        static const UUID* uuid_gen = new UUID(UUID::gen());
        return *uuid_gen;
    }

    static ResumeTokenData makeToken(Timestamp clusterTime, int idValue, size_t txnOpIndex = 0) {
        return ResumeTokenData(clusterTime,
                               ResumeTokenData::kDefaultTokenVersion,
                               txnOpIndex,
                               testUuid(),
                               /* eventIdentifier */ Value(Document{{"_id", idValue}}));
    }

    /**
     * Builds an update change event carrying 'token' as its resume token (sort key).
     */
    static Document makeUpdateEvent(const ResumeTokenData& token) {
        MutableDocument doc;
        doc.metadata().setSortKey(Value(ResumeToken(token).toDocument()), true);
        doc.addField(std::string{DocumentSourceChangeStream::kOperationTypeField},
                     Value("update"sv));
        doc.addField(std::string{DocumentSourceChangeStreamAddPreImage::kPreImageIdFieldName},
                     Value(Document{{"nsUUID", testUuid()}, {"ts", Timestamp(100, 1)}}));
        doc.addField(std::string{DocumentSourceChangeStream::kNamespaceField},
                     Value(Document{{"db", "test"sv}, {"coll", "coll"sv}}));
        doc.addField(std::string{DocumentSourceChangeStream::kClusterTimeField},
                     Value(token.clusterTime));
        doc.addField("documentKey", Value(Document{{"_id", 0}}));
        return doc.freeze();
    }

    void setChangeStreamSpec(boost::optional<ResumeToken> resumeAfter,
                             boost::optional<Timestamp> startAtOperationTime,
                             FullDocumentBeforeChangeModeEnum mode) {
        auto spec = DocumentSourceChangeStreamSpec();
        spec.setFullDocumentBeforeChange(mode);
        if (resumeAfter) {
            spec.setResumeAfter(*resumeAfter);
        }
        if (startAtOperationTime) {
            spec.setStartAtOperationTime(*startAtOperationTime);
        }
        getExpCtx()->setChangeStreamSpec(spec);
    }

    boost::intrusive_ptr<exec::agg::Stage> buildStage(const std::vector<Document>& events,
                                                      FullDocumentBeforeChangeModeEnum mode,
                                                      boost::optional<Document> preImageDoc) {
        auto processInterface =
            std::make_unique<MockPreImageProcessInterface>(std::move(preImageDoc));
        _processInterface = processInterface.get();
        getExpCtx()->setMongoProcessInterface(std::move(processInterface));
        // Keep the mock source alive for the lifetime of the fixture; the stage holds only a raw
        // pointer to it.
        _mockSource = exec::agg::MockStage::createForTest(events, getExpCtx());
        auto stage = make_intrusive<exec::agg::ChangeStreamAddPreImageStage>(
            DocumentSourceChangeStreamAddPreImage::kStageName, getExpCtx(), mode);
        exec::agg::stitchStage(*stage, _mockSource.get());
        return stage;
    }

    int lookupCount() const {
        return _processInterface->lookupCount();
    }

private:
    boost::intrusive_ptr<exec::agg::Stage> _mockSource;
    MockPreImageProcessInterface* _processInterface = nullptr;
};

TEST_F(DocumentSourceChangeStreamAddPreImageTest, ResumeTokenEventIsEnrichedButExemptFromRequired) {
    const Timestamp clusterTime(100, 1);
    const auto token = makeToken(clusterTime, 0);

    setChangeStreamSpec(
        ResumeToken(token), boost::none, FullDocumentBeforeChangeModeEnum::kRequired);

    auto stage = buildStage({makeUpdateEvent(token)},
                            FullDocumentBeforeChangeModeEnum::kRequired,
                            /* preImageDoc */ boost::none);

    auto next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    // The resume-token event is still enriched, so that an image-dependent $match placed before
    // token verification sees the same pre-image the original stream would have produced, but its
    // missing pre-image is not an error.
    ASSERT_VALUE_EQ(next.releaseDocument()
                        [DocumentSourceChangeStreamAddPreImage::kFullDocumentBeforeChangeFieldName],
                    Value(BSONNULL));
    ASSERT_EQ(lookupCount(), 1);
}

TEST_F(DocumentSourceChangeStreamAddPreImageTest, ResumeTokenEventIsEnrichedWithAvailablePreImage) {
    const Timestamp clusterTime(100, 1);
    const auto token = makeToken(clusterTime, 0);

    setChangeStreamSpec(
        ResumeToken(token), boost::none, FullDocumentBeforeChangeModeEnum::kRequired);

    const Document preImage{{"_id", 0}, {"v", 5}};
    auto stage = buildStage({makeUpdateEvent(token)},
                            FullDocumentBeforeChangeModeEnum::kRequired,
                            Document{{"preImage", preImage}});

    auto next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_VALUE_EQ(next.releaseDocument()
                        [DocumentSourceChangeStreamAddPreImage::kFullDocumentBeforeChangeFieldName],
                    Value(preImage));
    ASSERT_EQ(lookupCount(), 1);
}

TEST_F(DocumentSourceChangeStreamAddPreImageTest, EventAfterResumePointPerformsPreImageLookup) {
    const auto resumeToken = makeToken(Timestamp(100, 1), 0);
    const auto eventToken = makeToken(Timestamp(100, 2), 0);

    setChangeStreamSpec(
        ResumeToken(resumeToken), boost::none, FullDocumentBeforeChangeModeEnum::kRequired);

    auto stage = buildStage({makeUpdateEvent(eventToken)},
                            FullDocumentBeforeChangeModeEnum::kRequired,
                            /* preImageDoc */ boost::none);

    ASSERT_THROWS_CODE(stage->getNext(), AssertionException, ErrorCodes::NoMatchingDocument);
    ASSERT_EQ(lookupCount(), 1);
}

TEST_F(DocumentSourceChangeStreamAddPreImageTest,
       RequiredPreImageMissingWithHighWaterMarkResumePointThrows) {
    const auto eventToken = makeToken(Timestamp(100, 1), 0);

    // A high water mark token never matches an actual event, so no comparison is performed and the
    // "required" check still applies.
    setChangeStreamSpec(
        boost::none, Timestamp(100, 1), FullDocumentBeforeChangeModeEnum::kRequired);

    auto stage = buildStage({makeUpdateEvent(eventToken)},
                            FullDocumentBeforeChangeModeEnum::kRequired,
                            /* preImageDoc */ boost::none);

    ASSERT_THROWS_CODE(stage->getNext(), AssertionException, ErrorCodes::NoMatchingDocument);
    ASSERT_EQ(lookupCount(), 1);
}

TEST_F(DocumentSourceChangeStreamAddPreImageTest,
       WhenAvailablePreImageMissingForResumeTokenEventIsEnriched) {
    const Timestamp clusterTime(100, 1);
    const auto token = makeToken(clusterTime, 0);

    setChangeStreamSpec(
        ResumeToken(token), boost::none, FullDocumentBeforeChangeModeEnum::kWhenAvailable);

    auto stage = buildStage({makeUpdateEvent(token)},
                            FullDocumentBeforeChangeModeEnum::kWhenAvailable,
                            /* preImageDoc */ boost::none);

    auto next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_VALUE_EQ(next.releaseDocument()
                        [DocumentSourceChangeStreamAddPreImage::kFullDocumentBeforeChangeFieldName],
                    Value(BSONNULL));
    ASSERT_EQ(lookupCount(), 1);
}

TEST_F(DocumentSourceChangeStreamAddPreImageTest, StopsComparingAfterResumePointIsPassed) {
    const auto resumeToken = makeToken(Timestamp(100, 1), 0);
    const auto afterToken = makeToken(Timestamp(100, 2), 0);

    setChangeStreamSpec(
        ResumeToken(resumeToken), boost::none, FullDocumentBeforeChangeModeEnum::kRequired);

    // The first event is the resume-token event: it is enriched but exempt from the "required"
    // check. The second event is past the resume point and must be looked up, failing the
    // "required" check because no pre-image exists.
    auto stage = buildStage({makeUpdateEvent(resumeToken), makeUpdateEvent(afterToken)},
                            FullDocumentBeforeChangeModeEnum::kRequired,
                            /* preImageDoc */ boost::none);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());
    ASSERT_EQ(lookupCount(), 1);

    ASSERT_THROWS_CODE(stage->getNext(), AssertionException, ErrorCodes::NoMatchingDocument);
    ASSERT_EQ(lookupCount(), 2);
}

}  // namespace
}  // namespace mongo
