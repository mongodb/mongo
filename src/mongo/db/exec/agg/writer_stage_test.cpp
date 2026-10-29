// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/writer_stage.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/writer_util.h"
#include "mongo/unittest/unittest.h"

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

/**
 * Minimal WriterStage subclass that records flushed documents instead of writing them anywhere.
 */
class TestWriterStage final : public exec::agg::WriterStage<BSONObj> {
public:
    TestWriterStage(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : WriterStage<BSONObj>("$testWriter", expCtx, kOutputNs) {}

    using WriterStage<BSONObj>::setEffectiveQuota;

    size_t numFlushedDocs() const {
        return _numFlushedDocs;
    }

    static inline const NamespaceString kOutputNs =
        NamespaceString::createNamespaceString_forTest("test", "writer_stage_out");

protected:
    void flush(BatchedCommandRequest bcr, BatchedObjects batch) override {
        _numFlushedDocs += batch.size();
    }

    BatchedCommandRequest makeBatchedWriteRequest() const override {
        return makeInsertCommand(_outputNs, false /* bypassDocumentValidation */);
    }

    std::pair<BSONObj, int> makeBatchObject(Document doc) const override {
        auto obj = doc.toBson();
        auto size = obj.objsize();
        return {std::move(obj), size};
    }

private:
    size_t _numFlushedDocs{0};
};

/**
 * Returns a document whose BSON size is exactly 'sizeBytes'.
 */
Document makeDocOfSize(int sizeBytes) {
    // An empty object is 5 bytes; a string field "s" adds 1 (type) + 2 ("s\0") + 4 (length) + 1
    // (terminating NUL) = 8 bytes plus the string content.
    constexpr int kOverhead = 5 + 8;
    invariant(sizeBytes >= kOverhead);
    auto obj = BSON("s" << std::string(sizeBytes - kOverhead, 'x'));
    invariant(obj.objsize() == sizeBytes);
    return Document(obj);
}

class WriterStageTest : public AggregationContextFixture {
protected:
    boost::intrusive_ptr<TestWriterStage> makeStage(std::deque<exec::agg::GetNextResult> inputs) {
        _source = exec::agg::MockStage::createForTest(std::move(inputs), getExpCtx());
        auto stage = make_intrusive<TestWriterStage>(getExpCtx());
        exec::agg::MockStage::setSource_forTest(stage, _source.get());
        return stage;
    }

private:
    boost::intrusive_ptr<exec::agg::MockStage> _source;  // must outlive the stage
};

constexpr int kDocSize = 100;
constexpr ErrorCodes::Error kQuotaExceededCode{13624400};

TEST_F(WriterStageTest, QuotaExceededInSingleCallThrows) {
    auto stage = makeStage({makeDocOfSize(kDocSize), makeDocOfSize(kDocSize)});
    stage->setEffectiveQuota(kDocSize + kDocSize / 2);

    ASSERT_THROWS_CODE(stage->getNext(), AssertionException, kQuotaExceededCode);
}

TEST_F(WriterStageTest, SingleDocumentLargerThanQuotaThrows) {
    auto stage = makeStage({makeDocOfSize(kDocSize)});
    stage->setEffectiveQuota(kDocSize - 1);

    ASSERT_THROWS_CODE(stage->getNext(), AssertionException, kQuotaExceededCode);
}

TEST_F(WriterStageTest, QuotaExceededAcrossSeveralCallsThrows) {
    // Pauses between documents force doGetNext() to return after each document, so the quota
    // usage must accumulate across calls.
    auto stage = makeStage({makeDocOfSize(kDocSize),
                            exec::agg::GetNextResult::makePauseExecution(),
                            makeDocOfSize(kDocSize),
                            exec::agg::GetNextResult::makePauseExecution(),
                            makeDocOfSize(kDocSize)});
    stage->setEffectiveQuota(2 * kDocSize + kDocSize / 2);

    ASSERT_TRUE(stage->getNext().isPaused());
    ASSERT_EQ(stage->numFlushedDocs(), 1u);
    ASSERT_TRUE(stage->getNext().isPaused());
    ASSERT_EQ(stage->numFlushedDocs(), 2u);
    ASSERT_THROWS_CODE(stage->getNext(), AssertionException, kQuotaExceededCode);
}

TEST_F(WriterStageTest, QuotaExactlyReachedDoesNotThrow) {
    auto stage = makeStage({makeDocOfSize(kDocSize),
                            exec::agg::GetNextResult::makePauseExecution(),
                            makeDocOfSize(kDocSize)});
    stage->setEffectiveQuota(2 * kDocSize);

    ASSERT_TRUE(stage->getNext().isPaused());
    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_EQ(stage->numFlushedDocs(), 2u);
}

TEST_F(WriterStageTest, NoQuotaSetDoesNotThrow) {
    std::deque<exec::agg::GetNextResult> inputs;
    for (int i = 0; i < 10; ++i) {
        inputs.emplace_back(makeDocOfSize(kDocSize));
        inputs.emplace_back(exec::agg::GetNextResult::makePauseExecution());
    }
    auto stage = makeStage(std::move(inputs));

    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(stage->getNext().isPaused());
    }
    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_EQ(stage->numFlushedDocs(), 10u);
}

}  // namespace
}  // namespace mongo
