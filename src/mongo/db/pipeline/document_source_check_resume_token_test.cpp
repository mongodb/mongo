/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <memory>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_check_resume_token.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/pipeline/stub_mongo_process_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

using boost::intrusive_ptr;
using std::deque;

namespace mongo {
namespace {
static constexpr StringData kTestNs = "test.ns"_sd;

class CheckResumeTokenTest : public AggregationContextFixture {
public:
    CheckResumeTokenTest() : _mock(DocumentSourceMock::create()) {}

protected:
    /**
     * Puts an arbitrary document with resume token corresponding to the given timestamp, id, and
     * namespace in the mock queue.
     */
    void addDocument(Timestamp ts, std::string id, UUID uuid = testUuid()) {
        _mock->queue.push_back(Document{
            {"_id",
             ResumeToken(ResumeTokenData(ts, Value(Document{{"_id", id}}), uuid)).toDocument()}});
    }

    void addPause() {
        _mock->queue.push_back(DocumentSource::GetNextResult::makePauseExecution());
    }

    /**
     * Convenience method to create the class under test with a given timestamp, id, and namespace.
     */
    intrusive_ptr<DocumentSourceEnsureResumeTokenPresent> createCheckResumeToken(
        Timestamp ts, StringData id, UUID uuid = testUuid()) {
        ResumeToken token(ResumeTokenData(ts, Value(Document{{"_id", id}}), uuid));
        auto checkResumeToken = DocumentSourceEnsureResumeTokenPresent::create(getExpCtx(), token);
        checkResumeToken->setSource(_mock.get());
        return checkResumeToken;
    }

    /**
     * This method is required to avoid a static initialization fiasco resulting from calling
     * UUID::gen() in file or class static scope.
     */
    static const UUID& testUuid() {
        static const UUID* uuid_gen = new UUID(UUID::gen());
        return *uuid_gen;
    }

    intrusive_ptr<DocumentSourceMock> _mock;
};

class ShardCheckResumabilityTest : public CheckResumeTokenTest {
protected:
    intrusive_ptr<DocumentSourceShardCheckResumability> createShardCheckResumability(
        Timestamp ts, StringData id, UUID uuid = testUuid()) {
        ResumeToken token(ResumeTokenData(ts, Value(Document{{"_id", id}}), uuid));
        auto shardCheckResumability =
            DocumentSourceShardCheckResumability::create(getExpCtx(), token);
        shardCheckResumability->setSource(_mock.get());
        return shardCheckResumability;
    }
};

TEST_F(CheckResumeTokenTest, ShouldSucceedWithOnlyResumeToken) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createCheckResumeToken(resumeTimestamp, "1");
    addDocument(resumeTimestamp, "1");
    // We should not see the resume token.
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

TEST_F(CheckResumeTokenTest, ShouldSucceedWithPausesBeforeResumeToken) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createCheckResumeToken(resumeTimestamp, "1");
    addPause();
    addDocument(resumeTimestamp, "1");

    // We see the pause we inserted, but not the resume token.
    ASSERT_TRUE(checkResumeToken->getNext().isPaused());
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

TEST_F(CheckResumeTokenTest, ShouldSucceedWithPausesAfterResumeToken) {
    Timestamp resumeTimestamp(100, 1);
    Timestamp doc1Timestamp(100, 2);

    auto checkResumeToken = createCheckResumeToken(resumeTimestamp, "1");
    addDocument(resumeTimestamp, "1");
    addPause();
    addDocument(doc1Timestamp, "2");

    // Pause added explicitly.
    ASSERT_TRUE(checkResumeToken->getNext().isPaused());
    // The document after the resume token should be the first.
    auto result1 = checkResumeToken->getNext();
    ASSERT_TRUE(result1.isAdvanced());
    auto& doc1 = result1.getDocument();
    ASSERT_EQ(doc1Timestamp, ResumeToken::parse(doc1["_id"].getDocument()).getData().clusterTime);
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

TEST_F(CheckResumeTokenTest, ShouldSucceedWithMultipleDocumentsAfterResumeToken) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createCheckResumeToken(resumeTimestamp, "0");
    addDocument(resumeTimestamp, "0");

    Timestamp doc1Timestamp(100, 2);
    Timestamp doc2Timestamp(101, 1);
    addDocument(doc1Timestamp, "1");
    addDocument(doc2Timestamp, "2");

    auto result1 = checkResumeToken->getNext();
    ASSERT_TRUE(result1.isAdvanced());
    auto& doc1 = result1.getDocument();
    ASSERT_EQ(doc1Timestamp, ResumeToken::parse(doc1["_id"].getDocument()).getData().clusterTime);
    auto result2 = checkResumeToken->getNext();
    ASSERT_TRUE(result2.isAdvanced());
    auto& doc2 = result2.getDocument();
    ASSERT_EQ(doc2Timestamp, ResumeToken::parse(doc2["_id"].getDocument()).getData().clusterTime);
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

TEST_F(CheckResumeTokenTest, ShouldFailIfFirstDocHasWrongResumeToken) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createCheckResumeToken(resumeTimestamp, "1");

    Timestamp doc1Timestamp(100, 2);
    Timestamp doc2Timestamp(101, 1);
    addDocument(doc1Timestamp, "1");
    addDocument(doc2Timestamp, "2");
    ASSERT_THROWS_CODE(checkResumeToken->getNext(), AssertionException, 40585);
}

TEST_F(CheckResumeTokenTest, ShouldIgnoreChangeWithEarlierTimestamp) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createCheckResumeToken(resumeTimestamp, "1");
    addDocument(resumeTimestamp, "0");
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

TEST_F(CheckResumeTokenTest, ShouldFailIfTokenHasWrongNamespace) {
    Timestamp resumeTimestamp(100, 1);

    auto resumeTokenUUID = UUID::gen();
    auto checkResumeToken = createCheckResumeToken(resumeTimestamp, "1", resumeTokenUUID);
    auto otherUUID = UUID::gen();
    addDocument(resumeTimestamp, "1", otherUUID);
    ASSERT_THROWS_CODE(checkResumeToken->getNext(), AssertionException, 40585);
}

TEST_F(CheckResumeTokenTest, ShouldSucceedWithBinaryCollation) {
    CollatorInterfaceMock collatorCompareLower(CollatorInterfaceMock::MockType::kToLowerString);
    getExpCtx()->setCollator(&collatorCompareLower);

    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createCheckResumeToken(resumeTimestamp, "abc");
    // We must not see the following document.
    addDocument(resumeTimestamp, "ABC");
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

/**
 * We should _error_ on the no-document case, because that means the resume token was not found.
 */
TEST_F(CheckResumeTokenTest, ShouldSucceedWithNoDocuments) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createCheckResumeToken(resumeTimestamp, "0");
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

/**
 * A mock MongoProcessInterface which allows mocking a foreign pipeline.
 */
class MockMongoProcessInterface final : public StubMongoProcessInterface {
public:
    MockMongoProcessInterface(deque<DocumentSource::GetNextResult> mockResults)
        : _mockResults(std::move(mockResults)) {}

    bool isSharded(const NamespaceString& ns) final {
        return false;
    }

    StatusWith<std::unique_ptr<Pipeline, Pipeline::Deleter>> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const MakePipelineOptions opts) final {
        auto pipeline = Pipeline::parse(rawPipeline, expCtx);
        if (!pipeline.isOK()) {
            return pipeline.getStatus();
        }

        if (opts.optimize) {
            pipeline.getValue()->optimizePipeline();
        }

        if (opts.attachCursorSource) {
            uassertStatusOK(attachCursorSourceToPipeline(expCtx, pipeline.getValue().get()));
        }

        return pipeline;
    }

    Status attachCursorSourceToPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        Pipeline* pipeline) final {
        pipeline->addInitialSource(DocumentSourceMock::create(_mockResults));
        return Status::OK();
    }

private:
    deque<DocumentSource::GetNextResult> _mockResults;
};

TEST_F(ShardCheckResumabilityTest,
       ShouldSucceedIfResumeTokenIsPresentAndEarliestOplogEntryBeforeToken) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 2);

    auto shardCheckResumability = createShardCheckResumability(resumeTimestamp, "ID");
    deque<DocumentSource::GetNextResult> mockOplog({Document{{"ts", oplogTimestamp}}});
    shardCheckResumability->injectMongoProcessInterface(
        std::make_shared<MockMongoProcessInterface>(mockOplog));
    addDocument(resumeTimestamp, "ID");
    // We should see the resume token.
    auto result = shardCheckResumability->getNext();
    ASSERT_TRUE(result.isAdvanced());
    auto& doc = result.getDocument();
    ASSERT_EQ(resumeTimestamp, ResumeToken::parse(doc["_id"].getDocument()).getData().clusterTime);
}

TEST_F(ShardCheckResumabilityTest,
       ShouldSucceedIfResumeTokenIsPresentAndEarliestOplogEntryAfterToken) {
    Timestamp resumeTimestamp(100, 1);
    Timestamp oplogTimestamp(100, 2);

    auto shardCheckResumability = createShardCheckResumability(resumeTimestamp, "ID");
    deque<DocumentSource::GetNextResult> mockOplog({Document{{"ts", oplogTimestamp}}});
    shardCheckResumability->injectMongoProcessInterface(
        std::make_shared<MockMongoProcessInterface>(mockOplog));
    addDocument(resumeTimestamp, "ID");
    // We should see the resume token.
    auto result = shardCheckResumability->getNext();
    ASSERT_TRUE(result.isAdvanced());
    auto& doc = result.getDocument();
    ASSERT_EQ(resumeTimestamp, ResumeToken::parse(doc["_id"].getDocument()).getData().clusterTime);
}

TEST_F(ShardCheckResumabilityTest, ShouldSucceedIfResumeTokenIsPresentAndOplogIsEmpty) {
    Timestamp resumeTimestamp(100, 1);

    auto shardCheckResumability = createShardCheckResumability(resumeTimestamp, "ID");
    deque<DocumentSource::GetNextResult> mockOplog;
    shardCheckResumability->injectMongoProcessInterface(
        std::make_shared<MockMongoProcessInterface>(mockOplog));
    addDocument(resumeTimestamp, "ID");
    // We should see the resume token.
    auto result = shardCheckResumability->getNext();
    ASSERT_TRUE(result.isAdvanced());
    auto& doc = result.getDocument();
    ASSERT_EQ(resumeTimestamp, ResumeToken::parse(doc["_id"].getDocument()).getData().clusterTime);
}

TEST_F(ShardCheckResumabilityTest,
       ShouldSucceedWithNoDocumentsInPipelineAndEarliestOplogEntryBeforeToken) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 2);

    auto shardCheckResumability = createShardCheckResumability(resumeTimestamp, "0");
    deque<DocumentSource::GetNextResult> mockOplog({Document{{"ts", oplogTimestamp}}});
    shardCheckResumability->injectMongoProcessInterface(
        std::make_shared<MockMongoProcessInterface>(mockOplog));
    auto result = shardCheckResumability->getNext();
    ASSERT_TRUE(result.isEOF());
}

TEST_F(ShardCheckResumabilityTest,
       ShouldFailWithNoDocumentsInPipelineAndEarliestOplogEntryAfterToken) {
    Timestamp resumeTimestamp(100, 1);
    Timestamp oplogTimestamp(100, 2);

    auto shardCheckResumability = createShardCheckResumability(resumeTimestamp, "0");
    deque<DocumentSource::GetNextResult> mockOplog({Document{{"ts", oplogTimestamp}}});
    shardCheckResumability->injectMongoProcessInterface(
        std::make_shared<MockMongoProcessInterface>(mockOplog));
    ASSERT_THROWS_CODE(shardCheckResumability->getNext(), AssertionException, 40576);
}

TEST_F(ShardCheckResumabilityTest, ShouldSucceedWithNoDocumentsInPipelineAndOplogIsEmpty) {
    Timestamp resumeTimestamp(100, 2);

    auto shardCheckResumability = createShardCheckResumability(resumeTimestamp, "0");
    deque<DocumentSource::GetNextResult> mockOplog;
    shardCheckResumability->injectMongoProcessInterface(
        std::make_shared<MockMongoProcessInterface>(mockOplog));
    auto result = shardCheckResumability->getNext();
    ASSERT_TRUE(result.isEOF());
}

TEST_F(ShardCheckResumabilityTest,
       ShouldSucceedWithLaterDocumentsInPipelineAndEarliestOplogEntryBeforeToken) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 2);
    Timestamp docTimestamp(100, 3);

    auto shardCheckResumability = createShardCheckResumability(resumeTimestamp, "0");
    addDocument(docTimestamp, "ID");
    deque<DocumentSource::GetNextResult> mockOplog({Document{{"ts", oplogTimestamp}}});
    shardCheckResumability->injectMongoProcessInterface(
        std::make_shared<MockMongoProcessInterface>(mockOplog));
    auto result = shardCheckResumability->getNext();
    ASSERT_TRUE(result.isAdvanced());
    auto& doc = result.getDocument();
    ASSERT_EQ(docTimestamp, ResumeToken::parse(doc["_id"].getDocument()).getData().clusterTime);
}

TEST_F(ShardCheckResumabilityTest,
       ShouldFailWithLaterDocumentsInPipelineAndEarliestOplogEntryAfterToken) {
    Timestamp resumeTimestamp(100, 1);
    Timestamp oplogTimestamp(100, 2);
    Timestamp docTimestamp(100, 3);

    auto shardCheckResumability = createShardCheckResumability(resumeTimestamp, "0");
    addDocument(docTimestamp, "ID");
    deque<DocumentSource::GetNextResult> mockOplog({Document{{"ts", oplogTimestamp}}});
    shardCheckResumability->injectMongoProcessInterface(
        std::make_shared<MockMongoProcessInterface>(mockOplog));
    ASSERT_THROWS_CODE(shardCheckResumability->getNext(), AssertionException, 40576);
}

TEST_F(ShardCheckResumabilityTest, ShouldIgnoreOplogAfterFirstDoc) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 2);
    Timestamp oplogFutureTimestamp(100, 3);
    Timestamp docTimestamp(100, 4);

    auto shardCheckResumability = createShardCheckResumability(resumeTimestamp, "0");
    addDocument(docTimestamp, "ID");
    deque<DocumentSource::GetNextResult> mockOplog({Document{{"ts", oplogTimestamp}}});
    shardCheckResumability->injectMongoProcessInterface(
        std::make_shared<MockMongoProcessInterface>(mockOplog));
    auto result1 = shardCheckResumability->getNext();
    ASSERT_TRUE(result1.isAdvanced());
    auto& doc1 = result1.getDocument();
    ASSERT_EQ(docTimestamp, ResumeToken::parse(doc1["_id"].getDocument()).getData().clusterTime);

    mockOplog = {Document{{"ts", oplogFutureTimestamp}}};
    shardCheckResumability->injectMongoProcessInterface(
        std::make_shared<MockMongoProcessInterface>(mockOplog));
    auto result2 = shardCheckResumability->getNext();
    ASSERT_TRUE(result2.isEOF());
}

TEST_F(ShardCheckResumabilityTest, ShouldSucceedWhenOplogEntriesExistBeforeAndAfterResumeToken) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 2);
    Timestamp oplogFutureTimestamp(100, 3);
    Timestamp docTimestamp(100, 4);

    auto shardCheckResumability = createShardCheckResumability(resumeTimestamp, "0");
    addDocument(docTimestamp, "ID");
    deque<DocumentSource::GetNextResult> mockOplog(
        {{Document{{"ts", oplogTimestamp}}}, {Document{{"ts", oplogFutureTimestamp}}}});
    shardCheckResumability->injectMongoProcessInterface(
        std::make_shared<MockMongoProcessInterface>(mockOplog));
    auto result1 = shardCheckResumability->getNext();
    ASSERT_TRUE(result1.isAdvanced());
    auto& doc1 = result1.getDocument();
    ASSERT_EQ(docTimestamp, ResumeToken::parse(doc1["_id"].getDocument()).getData().clusterTime);
    auto result2 = shardCheckResumability->getNext();
    ASSERT_TRUE(result2.isEOF());
}

TEST_F(ShardCheckResumabilityTest, ShouldIgnoreOplogAfterFirstEOF) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 2);
    Timestamp oplogFutureTimestamp(100, 3);

    auto shardCheckResumability = createShardCheckResumability(resumeTimestamp, "0");
    deque<DocumentSource::GetNextResult> mockOplog({Document{{"ts", oplogTimestamp}}});
    shardCheckResumability->injectMongoProcessInterface(
        std::make_shared<MockMongoProcessInterface>(mockOplog));
    auto result1 = shardCheckResumability->getNext();
    ASSERT_TRUE(result1.isEOF());

    mockOplog = {Document{{"ts", oplogFutureTimestamp}}};
    shardCheckResumability->injectMongoProcessInterface(
        std::make_shared<MockMongoProcessInterface>(mockOplog));
    auto result2 = shardCheckResumability->getNext();
    ASSERT_TRUE(result2.isEOF());
}

}  // namespace
}  // namespace mongo
