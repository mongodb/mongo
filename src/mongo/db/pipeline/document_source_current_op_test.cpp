// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_current_op.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/json.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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
                                       CurrentOpCursorMode cursorMode) const override {
        return _ops;
    }

    std::string getShardName(OperationContext* opCtx) const override {
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
    getExpCtx()->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "foo")));
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIfNotRunWithAggregateOne) {
    const auto specObj = fromjson("{$currentOp:{}}");
    getExpCtx()->setNamespaceString(NamespaceString::createNamespaceString_forTest("admin.foo"));
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIdleConnectionsIfNotBoolean) {
    const auto specObj = fromjson("{$currentOp:{idleConnections:1}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIdleSessionsIfNotBoolean) {
    const auto specObj = fromjson("{$currentOp:{idleSessions:1}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseAllUsersIfNotBoolean) {
    const auto specObj = fromjson("{$currentOp:{allUsers:1}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseLocalOpsIfNotBoolean) {
    const auto specObj = fromjson("{$currentOp:{localOps:1}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseTruncateOpsIfNotBoolean) {
    const auto specObj = fromjson("{$currentOp:{truncateOps:1}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseTargetAllNodesIfNotBoolean) {
    const auto specObj = fromjson("{$currentOp:{targetAllNodes:1}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseTrueTargetAllNodesIfTrueLocalOps) {
    const auto specObj = fromjson("{$currentOp:{targetAllNodes:true, localOps:true}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseTrueTargetAllNodesIfUnsharded) {
    const auto specObj = fromjson("{$currentOp:{targetAllNodes:true}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldParseAndSerializeTargetAllNodesIfSharded) {
    const auto specObj = fromjson("{$currentOp:{targetAllNodes:true}}");

    getExpCtx()->setFromRouter(true);

    const auto parsed =
        DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx());

    const auto currentOp = static_cast<DocumentSourceCurrentOp*>(parsed.get());

    const auto expectedOutput = Document{{"$currentOp", Document{{"targetAllNodes", true}}}};

    ASSERT_DOCUMENT_EQ(currentOp->serialize().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailToParseIfUnrecognisedParameterSpecified) {
    const auto specObj = fromjson("{$currentOp:{foo:true}}");
    ASSERT_THROWS_CODE(DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldParseAndSerializeAllExplicitlySpecifiedArguments) {
    const auto specObj = fromjson(
        "{$currentOp:{idleConnections:false, idleSessions:false, allUsers:true, localOps:true, "
        "truncateOps:false, targetAllNodes:false}}");

    const auto parsed =
        DocumentSourceCurrentOp::createFromBson(specObj.firstElement(), getExpCtx());

    const auto currentOp = static_cast<DocumentSourceCurrentOp*>(parsed.get());

    const auto expectedOutput = Document{{"$currentOp",
                                          Document{{"idleConnections", false},
                                                   {"idleSessions", false},
                                                   {"allUsers", true},
                                                   {"localOps", true},
                                                   {"truncateOps", false},
                                                   {"targetAllNodes", false}}}};

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
                localOps: true,
                targetAllNodes: false
            }
        })");
    auto docSource = DocumentSourceCurrentOp::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$currentOp": {
                "idleConnections": "?bool",
                "idleSessions": "?bool",
                "allUsers": "?bool",
                "localOps": "?bool",
                "targetAllNodes": "?bool"
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
        R"({"$currentOp": {}})",
        redact(*docSource));
}

TEST_F(DocumentSourceCurrentOpTest, ShouldReturnEOFImmediatelyIfNoCurrentOps) {
    getExpCtx()->setMongoProcessInterface(std::make_shared<MockMongoInterface>());

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());
    auto currentOpStage = exec::agg::buildStage(currentOp);

    ASSERT(currentOpStage->getNext().isEOF());
}

TEST_F(DocumentSourceCurrentOpTest,
       ShouldAddShardNameModifyOpIDAndClientFieldNameInShardedContext) {
    getExpCtx()->setFromRouter(true);

    std::vector<BSONObj> ops{fromjson("{ client: '192.168.1.10:50844', opid: 430 }")};
    getExpCtx()->setMongoProcessInterface(std::make_shared<MockMongoInterface>(ops));

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());
    auto currentOpStage = exec::agg::buildStage(currentOp);

    const auto expectedOutput =
        Document{{"shard", kMockShardName},
                 {"client_s", std::string("192.168.1.10:50844")},
                 {"opid", std::string(str::stream() << kMockShardName << ":430")}};

    ASSERT_DOCUMENT_EQ(currentOpStage->getNext().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceCurrentOpTest,
       ShouldReturnOpIDAndClientFieldNameUnmodifiedWhenNotInShardedContext) {
    getExpCtx()->setFromRouter(false);

    std::vector<BSONObj> ops{fromjson("{ client: '192.168.1.10:50844', opid: 430 }")};
    getExpCtx()->setMongoProcessInterface(std::make_shared<MockMongoInterface>(ops));

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());
    auto currentOpStage = exec::agg::buildStage(currentOp);

    const auto expectedOutput =
        Document{{"client", std::string("192.168.1.10:50844")}, {"opid", 430}};

    ASSERT_DOCUMENT_EQ(currentOpStage->getNext().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailIfNoShardNameAvailableForShardedRequest) {
    getExpCtx()->setFromRouter(true);

    getExpCtx()->setMongoProcessInterface(std::make_shared<MockMongoInterface>(false));

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());
    auto currentOpStage = exec::agg::buildStage(currentOp);

    ASSERT_THROWS_CODE(currentOpStage->getNext(), AssertionException, 40465);
}

TEST_F(DocumentSourceCurrentOpTest, ShouldFailIfOpIDIsNonNumericWhenModifyingInShardedContext) {
    getExpCtx()->setFromRouter(true);

    std::vector<BSONObj> ops{fromjson("{ client: '192.168.1.10:50844', opid: 'string' }")};
    getExpCtx()->setMongoProcessInterface(std::make_shared<MockMongoInterface>(ops));

    const auto currentOp = DocumentSourceCurrentOp::create(getExpCtx());
    auto currentOpStage = exec::agg::buildStage(currentOp);

    ASSERT_THROWS_CODE(currentOpStage->getNext(), AssertionException, ErrorCodes::TypeMismatch);
}

}  // namespace
}  // namespace mongo
