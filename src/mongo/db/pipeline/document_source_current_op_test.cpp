/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_current_op.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/stub_mongod_interface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

const std::string kMockShardName = "testshard";

/**
 * Subclass AggregationContextFixture to set the ExpressionContext's namespace to 'admin' with
 * {aggregate: 1} by default, so that parsing tests other than those which validate the namespace do
 * not need to explicitly set it.
 */
class DocumentSourceCurrentOpTest : public AggregationContextFixture {
public:
    DocumentSourceCurrentOpTest()
        : AggregationContextFixture(NamespaceString::makeCollectionlessAggregateNSS("admin")) {}
};

/**
 * A MongodInterface used for testing which returns artificial currentOp entries.
 */
class MockMongodImplementation final : public StubMongodInterface {
public:
    MockMongodImplementation(std::vector<BSONObj> ops, bool hasShardName = true)
        : _ops(std::move(ops)), _hasShardName(hasShardName) {}

    MockMongodImplementation(bool hasShardName = true) : _hasShardName(hasShardName) {}

    std::vector<BSONObj> getCurrentOps(CurrentOpConnectionsMode connMode,
                                       CurrentOpUserMode userMode) const {
        return _ops;
    }

    std::string getShardName(OperationContext* opCtx) const {
        if (_hasShardName) {
            return kMockShardName;
        }

        return std::string();
    }

private:
    std::vector<BSONObj> _ops;
    bool _hasShardName;
};

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIfSpecIsNotObject) {
    const auto specObj = fromjson("{$currentOp:1}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       UserException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIfNotRunOnAdmin) {
    const auto specObj = fromjson("{$currentOp:{}}");
    getExpCtx()->ns = NamespaceString::makeCollectionlessAggregateNSS("foo");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       UserException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIfNotRunWithAggregateOne) {
    const auto specObj = fromjson("{$currentOp:{}}");
    getExpCtx()->ns = NamespaceString("admin.foo");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       UserException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIdleConnectionsIfNotBoolean) {
    const auto specObj = fromjson("{$currentOp:{idleConnections:1}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       UserException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseAllUsersIfNotBoolean) {
    const auto specObj = fromjson("{$currentOp:{allUsers:1}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       UserException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIfUnrecognisedParameterSpecified) {
    const auto specObj = fromjson("{$currentOp:{foo:true}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       UserException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldParseAndSerializeTrueOptionalArguments) {
    const auto specObj = fromjson("{$currentOp:{idleConnections:true, allUsers:true}}");

    const auto parsed =
        DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx());

    const auto currentOp = static_cast<DocumentSourceCurrentOp*>(parsed.get());

    const auto expectedOutput =
        Document{{"$currentOp", Document{{"idleConnections", true}, {"allUsers", true}}}};

    ASSERT_DOCUMENT_EQ(currentOp->serialize().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldParseAndSerializeFalseOptionalArguments) {
    const auto specObj = fromjson("{$currentOp:{idleConnections:false, allUsers:false}}");

    const auto parsed =
        DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx());

    const auto currentOp = static_cast<DocumentSourceCurrentOp*>(parsed.get());

    const auto expectedOutput =
        Document{{"$currentOp", Document{{"idleConnections", false}, {"allUsers", false}}}};

    ASSERT_DOCUMENT_EQ(currentOp->serialize().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldSerializeOmittedOptionalArgumentsAsDefaultValues) {
    const auto specObj = fromjson("{$currentOp:{}}");

    const auto parsed =
        DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx());

    const auto currentOp = static_cast<DocumentSourceCurrentOp*>(parsed.get());

    const auto expectedOutput =
        Document{{"$currentOp", Document{{"idleConnections", false}, {"allUsers", false}}}};

    ASSERT_DOCUMENT_EQ(currentOp->serialize().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldReturnEOFImmediatelyIfNoCurrentOps) {
    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());
    const auto mongod = std::make_shared<MockMongodImplementation>();
    currentOp->injectMongodInterface(mongod);

    ASSERT(currentOp->getNext().isEOF());
}

TEST_F(DocumentSourceCurrentOpTest, ShouldModifyOpIDAndClientFieldNameInShardedContext) {
    getExpCtx()->inShard = true;

    std::vector<BSONObj> ops{fromjson("{ client: '192.168.1.10:50844', opid: 430 }")};
    const auto mongod = std::make_shared<MockMongodImplementation>(ops);

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());
    currentOp->injectMongodInterface(mongod);

    const auto expectedOutput =
        Document{{"client_s", std::string("192.168.1.10:50844")},
                 {"opid", std::string(str::stream() << kMockShardName << ":430")}};

    ASSERT_DOCUMENT_EQ(currentOp->getNext().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceCurrentOpTest,
       ShouldReturnOpIDAndClientFieldNameUnmodifiedWhenNotInShardedContext) {
    getExpCtx()->inShard = false;

    std::vector<BSONObj> ops{fromjson("{ client: '192.168.1.10:50844', opid: 430 }")};
    const auto mongod = std::make_shared<MockMongodImplementation>(ops);

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());
    currentOp->injectMongodInterface(mongod);

    const auto expectedOutput =
        Document{{"client", std::string("192.168.1.10:50844")}, {"opid", 430}};

    ASSERT_DOCUMENT_EQ(currentOp->getNext().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailIfNoShardNameAvailableForShardedRequest) {
    getExpCtx()->inShard = true;

    const auto mongod = std::make_shared<MockMongodImplementation>(false);

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());
    currentOp->injectMongodInterface(mongod);

    ASSERT_THROWS_CODE(currentOp->getNext(), UserException, 40465);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailIfOpIDIsNonNumericWhenModifyingInShardedContext) {
    getExpCtx()->inShard = true;

    std::vector<BSONObj> ops{fromjson("{ client: '192.168.1.10:50844', opid: 'string' }")};
    const auto mongod = std::make_shared<MockMongodImplementation>(ops);

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());
    currentOp->injectMongodInterface(mongod);

    ASSERT_THROWS_CODE(currentOp->getNext(), UserException, ErrorCodes::TypeMismatch);
}

}  // namespace
}  // namespace mongo
