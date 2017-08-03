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
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

using boost::intrusive_ptr;

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
    void addDocument(Timestamp ts, std::string id, StringData ns = kTestNs) {
        _mock->queue.push_back(
            Document({{"_id", Document({{"ts", ts}, {"ns", ns}, {"_id", id}})}}));
    }

    void addPause() {
        _mock->queue.push_back(DocumentSource::GetNextResult::makePauseExecution());
    }

    /**
     * Convenience method to create the class under test with a given timestamp, id, and namespace.
     */
    intrusive_ptr<DocumentSourceCheckResumeToken> createCheckResumeToken(Timestamp ts,
                                                                         StringData id,
                                                                         StringData ns = kTestNs) {
        auto token = ResumeToken::parse(BSON("ts" << ts << "_id" << id << "ns" << ns));
        DocumentSourceCheckResumeTokenSpec spec;
        spec.setResumeToken(token);
        auto checkResumeToken = DocumentSourceCheckResumeToken::create(getExpCtx(), spec);
        checkResumeToken->setSource(_mock.get());
        return checkResumeToken;
    }

private:
    intrusive_ptr<DocumentSourceMock> _mock;
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
    ASSERT_VALUE_EQ(Value(doc1Timestamp), doc1["_id"].getDocument()["ts"]);
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
    ASSERT_VALUE_EQ(Value(doc1Timestamp), doc1["_id"].getDocument()["ts"]);
    auto result2 = checkResumeToken->getNext();
    ASSERT_TRUE(result2.isAdvanced());
    auto& doc2 = result2.getDocument();
    ASSERT_VALUE_EQ(Value(doc2Timestamp), doc2["_id"].getDocument()["ts"]);
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

TEST_F(CheckResumeTokenTest, ShouldFailIfFirstDocHasWrongResumeToken) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createCheckResumeToken(resumeTimestamp, "1");

    Timestamp doc1Timestamp(100, 2);
    Timestamp doc2Timestamp(101, 1);
    addDocument(doc1Timestamp, "1");
    addDocument(doc2Timestamp, "2");
    ASSERT_THROWS_CODE(checkResumeToken->getNext(), UserException, 40585);
}

TEST_F(CheckResumeTokenTest, ShouldFailIfTokenHasWrongDocumentId) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createCheckResumeToken(resumeTimestamp, "0");
    addDocument(resumeTimestamp, "1");
    ASSERT_THROWS_CODE(checkResumeToken->getNext(), UserException, 40585);
}

TEST_F(CheckResumeTokenTest, ShouldFailIfTokenHasWrongNamespace) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createCheckResumeToken(resumeTimestamp, "1", "test1.ns");
    addDocument(resumeTimestamp, "1", "test2.ns");
    ASSERT_THROWS_CODE(checkResumeToken->getNext(), UserException, 40585);
}

/**
 * We should _error_ on the no-document case, because that means the resume token was not found.
 */
TEST_F(CheckResumeTokenTest, ShouldFailWithNoDocuments) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createCheckResumeToken(resumeTimestamp, "0");
    ASSERT_THROWS_CODE(checkResumeToken->getNext(), UserException, 40584);
}

}  // namespace
}  // namespace mongo
