/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_current_op.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

const auto kQueryPlanner = ExplainOptions::Verbosity::kQueryPlanner;
const std::string kMockShardName = "testshard";

/**
 * Subclass AggregationContextFixture to set the ExpressionContext's namespace to 'admin' with
 * {aggregate: 1} by default, so that parsing tests other than those which validate the namespace do
 * not need to explicitly set it.
 */
class DocumentSourceCurrentOpTest : public AggregationContextFixture {
public:
    DocumentSourceCurrentOpTest()
        : AggregationContextFixture(
              NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin)) {}
};

/**
 * A MongoProcessInterface used for testing which returns artificial currentOp entries.
 */
class MockMongoInterface final : public StubMongoProcessInterface {
public:
    MockMongoInterface(std::vector<BSONObj> ops, bool hasShardName = true)
        : _ops(std::move(ops)), _hasShardName(hasShardName) {}

    MockMongoInterface(bool hasShardName = true) : _hasShardName(hasShardName) {}

    std::vector<BSONObj> getCurrentOps(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       CurrentOpConnectionsMode connMode,
                                       CurrentOpSessionsMode sessionMode,
                                       CurrentOpUserMode userMode,
                                       CurrentOpTruncateMode truncateMode,
                                       CurrentOpCursorMode cursorMode,
                                       CurrentOpBacktraceMode backtraceMode) const {
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
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIfNotRunOnAdmin) {
    const auto specObj = fromjson("{$currentOp:{}}");
    getExpCtx()->ns = NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "foo"));
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIfNotRunWithAggregateOne) {
    const auto specObj = fromjson("{$currentOp:{}}");
    getExpCtx()->ns = NamespaceString::createNamespaceString_forTest("admin.foo");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIdleConnectionsIfNotBoolean) {
    const auto specObj = fromjson("{$currentOp:{idleConnections:1}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIdleSessionsIfNotBoolean) {
    const auto specObj = fromjson("{$currentOp:{idleSessions:1}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseAllUsersIfNotBoolean) {
    const auto specObj = fromjson("{$currentOp:{allUsers:1}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseLocalOpsIfNotBoolean) {
    const auto specObj = fromjson("{$currentOp:{localOps:1}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseTruncateOpsIfNotBoolean) {
    const auto specObj = fromjson("{$currentOp:{truncateOps:1}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIfUnrecognisedParameterSpecified) {
    const auto specObj = fromjson("{$currentOp:{foo:true}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldParseAndSerializeAllExplicitlySpecifiedArguments) {
    const auto specObj = fromjson(
        "{$currentOp:{idleConnections:false, idleSessions:false, allUsers:true, localOps:true, "
        "truncateOps:false}}");

    const auto parsed =
        DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx());

    const auto currentOp = static_cast<DocumentSourceCurrentOp*>(parsed.get());

    const auto expectedOutput = Document{{"$currentOp",
                                          Document{{"idleConnections", false},
                                                   {"idleSessions", false},
                                                   {"allUsers", true},
                                                   {"localOps", true},
                                                   {"truncateOps", false}}}};

    ASSERT_DOCUMENT_EQ(currentOp->serialize().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceCurrentOpTest,
       ShouldParseAndSerializeAllExplicitlySpecifiedArgumentsWithRedaction) {
    auto spec = fromjson(
        R"({
            $currentOp: {
                idleConnections: true,
                allUsers: false,
                idleSessions: false,
                localOps: true
            }
        })");
    auto docSource = DocumentSourceCurrentOp::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$currentOp": {
                "idleConnections": "?",
                "idleSessions": "?",
                "allUsers": "?",
                "localOps": "?"
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceCurrentOpTest, ShouldNotSerializeOmittedOptionalArguments) {
    const auto specObj = fromjson("{$currentOp:{}}");

    const auto parsed =
        DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx());

    const auto currentOp = static_cast<DocumentSourceCurrentOp*>(parsed.get());

    const auto expectedOutput = Document{{"$currentOp", Document{}}};

    ASSERT_DOCUMENT_EQ(currentOp->serialize().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldNotSerializeOmittedOptionalArgumentsWithRedaction) {
    const auto specObj = fromjson("{$currentOp:{}}");

    const auto docSource =
        DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$currentOp": {}
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceCurrentOpTest, ShouldReturnEOFImmediatelyIfNoCurrentOps) {
    getExpCtx()->mongoProcessInterface = std::make_shared<MockMongoInterface>();

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());

    ASSERT(currentOp->getNext().isEOF());
}

TEST_F(DocumentSourceCurrentOpTest,
       ShouldAddShardNameModifyOpIDAndClientFieldNameInShardedContext) {
    getExpCtx()->fromMongos = true;

    std::vector<BSONObj> ops{fromjson("{ client: '192.168.1.10:50844', opid: 430 }")};
    getExpCtx()->mongoProcessInterface = std::make_shared<MockMongoInterface>(ops);

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());

    const auto expectedOutput =
        Document{{"shard", kMockShardName},
                 {"client_s", std::string("192.168.1.10:50844")},
                 {"opid", std::string(str::stream() << kMockShardName << ":430")}};

    ASSERT_DOCUMENT_EQ(currentOp->getNext().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceCurrentOpTest,
       ShouldReturnOpIDAndClientFieldNameUnmodifiedWhenNotInShardedContext) {
    getExpCtx()->fromMongos = false;

    std::vector<BSONObj> ops{fromjson("{ client: '192.168.1.10:50844', opid: 430 }")};
    getExpCtx()->mongoProcessInterface = std::make_shared<MockMongoInterface>(ops);

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());

    const auto expectedOutput =
        Document{{"client", std::string("192.168.1.10:50844")}, {"opid", 430}};

    ASSERT_DOCUMENT_EQ(currentOp->getNext().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailIfNoShardNameAvailableForShardedRequest) {
    getExpCtx()->fromMongos = true;

    getExpCtx()->mongoProcessInterface = std::make_shared<MockMongoInterface>(false);

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());

    ASSERT_THROWS_CODE(currentOp->getNext(), AssertionException, 40465);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailIfOpIDIsNonNumericWhenModifyingInShardedContext) {
    getExpCtx()->fromMongos = true;

    std::vector<BSONObj> ops{fromjson("{ client: '192.168.1.10:50844', opid: 'string' }")};
    getExpCtx()->mongoProcessInterface = std::make_shared<MockMongoInterface>(ops);

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());

    ASSERT_THROWS_CODE(currentOp->getNext(), AssertionException, ErrorCodes::TypeMismatch);
}

}  // namespace
}  // namespace mongo
