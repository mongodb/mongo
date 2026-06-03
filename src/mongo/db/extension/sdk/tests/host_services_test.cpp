/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/sdk/host_services.h"

#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/host_connector/adapter/query_shape_opts_adapter.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo::extension {
namespace {

class HostServicesTest : public unittest::Test {
public:
    void setUp() override {
        sdk::HostServicesAPI::setHostServices(&host_connector::HostServicesAdapter::get());
    }

    auto getExpCtx() {
        return _expCtx;
    }

    boost::intrusive_ptr<ExpressionContextForTest> _expCtx = new ExpressionContextForTest();
};

/**
 * This parse node is meant to mimic how $vectorSearch will parse and serialize its 'filter' field.
 * It wraps the filter expression in a $match node, uses HostServices::createHostAggStageParseNode()
 * to generate a parse node, then calls getQueryShape() on that node to generate the 'filter' shape.
 */
class NestedFilterParseNode : public sdk::AggStageParseNode {
public:
    NestedFilterParseNode(const mongo::BSONObj& arguments)
        : sdk::AggStageParseNode("$nestedFilter"),
          _arguments(arguments.getOwned()),
          _parseNode(nullptr) {
        auto& hostServices = sdk::HostServicesAPI::getInstance();
        mongo::BSONObj matchFilter = BSON("$match" << _arguments["filter"].Obj());
        _parseNode = hostServices->createHostAggStageParseNode(matchFilter);
    }

    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        return {};
    }

    mongo::BSONObj getQueryShape(const sdk::QueryShapeOptsHandle& opts) const override {
        auto matchNodeShape = _parseNode->getQueryShape(*opts.get());
        return BSON("$nestedFilter" << BSON("filter" << matchNodeShape["$match"]));
    }

    mongo::BSONObj toBsonForLog() const override {
        return BSON(_name << _arguments);
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<NestedFilterParseNode>(_arguments);
    }

private:
    mongo::BSONObj _arguments;
    mongo::extension::AggStageParseNodeHandle _parseNode;
};

/**
 * Tests that the MongoExtensionLogMessage created by HostServicesHandle::createLogMessageStruct
 * has the correct fields populated.
 */
TEST_F(HostServicesTest, CreateLogMessageStructEmptyAttrs) {
    std::string logMessage = "Test log message";
    std::int32_t logCode = 12345;
    ::MongoExtensionLogSeverity logSeverity = ::MongoExtensionLogSeverity::kInfo;

    auto structuredLogGuard =
        sdk::LoggerAPI::createLogMessageStruct(logMessage, logCode, logSeverity, {});
    auto structuredLog = *structuredLogGuard.get();
    ASSERT_EQUALS(structuredLog.code, static_cast<uint32_t>(logCode));
    ASSERT_EQUALS(structuredLog.type, ::MongoExtensionLogType::kLog);
    ASSERT_EQUALS(structuredLog.severityOrLevel.severity, logSeverity);
    ASSERT_EQUALS(structuredLog.attributes.size, 0);
    ASSERT_EQUALS(structuredLog.attributes.elements, nullptr);

    auto messageView = byteViewAsStringView(structuredLog.message);
    ASSERT_EQUALS(std::string(messageView), logMessage);
}

TEST_F(HostServicesTest, CreateLogMessageStructWithAttrs) {
    std::string logMessage = "Test log message";
    std::int32_t logCode = 12345;
    ::MongoExtensionLogSeverity logSeverity = ::MongoExtensionLogSeverity::kInfo;

    std::vector<sdk::ExtensionLogAttribute> attrs = {{"hi", "finley"}};

    auto structuredLogGuard =
        sdk::LoggerAPI::createLogMessageStruct(logMessage, logCode, logSeverity, attrs);
    auto structuredLog = *structuredLogGuard.get();
    ASSERT_EQUALS(structuredLog.code, static_cast<uint32_t>(logCode));
    ASSERT_EQUALS(structuredLog.type, ::MongoExtensionLogType::kLog);
    ASSERT_EQUALS(structuredLog.severityOrLevel.severity, logSeverity);
    ASSERT_EQUALS(structuredLog.attributes.size, 1);
    ASSERT_EQUALS(std::string(byteViewAsStringView(structuredLog.attributes.elements[0].name)),
                  "hi");
    ASSERT_EQUALS(std::string(byteViewAsStringView(structuredLog.attributes.elements[0].value)),
                  "finley");

    auto messageView = byteViewAsStringView(structuredLog.message);
    ASSERT_EQUALS(std::string(messageView), logMessage);
}

/**
 * Tests that the MongoExtensionLogMessage created by
 * HostServicesHandle::createDebugLogMessageStruct has the correct fields populated.
 */
TEST_F(HostServicesTest, CreateDebugLogMessageStructWithAttrs) {
    std::string logMessage = "Test debug log message";
    std::int32_t logCode = 12345;
    std::int32_t logLevel = 1;

    std::vector<sdk::ExtensionLogAttribute> attrs = {{"hi", "mongodb"}};

    auto structuredDebugLogGuard =
        sdk::LoggerAPI::createDebugLogMessageStruct(logMessage, logCode, logLevel, attrs);
    auto structuredDebugLog = *structuredDebugLogGuard.get();

    ASSERT_EQUALS(structuredDebugLog.code, static_cast<uint32_t>(logCode));
    ASSERT_EQUALS(structuredDebugLog.type, ::MongoExtensionLogType::kDebug);
    ASSERT_EQUALS(structuredDebugLog.severityOrLevel.level, logLevel);
    ASSERT_EQUALS(structuredDebugLog.attributes.size, 1);
    ASSERT_EQUALS(std::string(byteViewAsStringView(structuredDebugLog.attributes.elements[0].name)),
                  "hi");
    ASSERT_EQUALS(
        std::string(byteViewAsStringView(structuredDebugLog.attributes.elements[0].value)),
        "mongodb");

    auto messageView = byteViewAsStringView(structuredDebugLog.message);
    ASSERT_EQUALS(std::string(messageView), logMessage);
}

TEST_F(HostServicesTest, CreateDebugLogMessageStructEmptyAttrs) {
    std::string logMessage = "Test debug log message";
    std::int32_t logCode = 12345;
    std::int32_t logLevel = 1;

    auto structuredDebugLogGuard =
        sdk::LoggerAPI::createDebugLogMessageStruct(logMessage, logCode, logLevel, {});
    auto structuredDebugLog = *structuredDebugLogGuard.get();

    ASSERT_EQUALS(structuredDebugLog.code, static_cast<uint32_t>(logCode));
    ASSERT_EQUALS(structuredDebugLog.type, ::MongoExtensionLogType::kDebug);
    ASSERT_EQUALS(structuredDebugLog.severityOrLevel.level, logLevel);

    auto messageView = byteViewAsStringView(structuredDebugLog.message);
    ASSERT_EQUALS(std::string(messageView), logMessage);
}

TEST_F(HostServicesTest, userAsserted) {
    auto errmsg = "an error";
    int errorCode = 11111;
    BSONObj errInfo = BSON("message" << errmsg << "errorCode" << errorCode);
    ::MongoExtensionByteView errInfoByteView = objAsByteView(errInfo);

    StatusHandle status(sdk::HostServicesAPI::getInstance()->userAsserted(errInfoByteView));

    ASSERT_EQ(status->getCode(), errorCode);
    // Reason is not populated on the status for re-throwable exceptions.
    ASSERT_EQ(status->getReason(), "");
}

using HostServicesTestDeathTest = HostServicesTest;
DEATH_TEST_REGEX_F(HostServicesTestDeathTest, tripwireAsserted, "22222") {
    auto errmsg = "fatal error";
    int errorCode = 22222;
    BSONObj errInfo = BSON("message" << errmsg << "errorCode" << errorCode);
    ::MongoExtensionByteView errInfoByteView = objAsByteView(errInfo);

    [[maybe_unused]] auto status =
        sdk::HostServicesAPI::getInstance()->tripwireAsserted(errInfoByteView);
}

TEST_F(HostServicesTest, CreateIdLookup_ValidSpecReturnsHostNode) {
    auto bsonSpec = BSON("$_internalSearchIdLookup" << BSONObj());
    auto hostAstNode = extension::sdk::HostServicesAPI::getInstance()->createIdLookup(bsonSpec);
    ASSERT_TRUE(hostAstNode->getName() ==
                std::string(DocumentSourceInternalSearchIdLookUp::kStageName));
}

TEST_F(HostServicesTest, CreateIdLookup_InvalidSpecFails) {
    auto bsonSpec = BSON("$match" << BSONObj());
    ASSERT_THROWS_CODE(extension::sdk::HostServicesAPI::getInstance()->createIdLookup(bsonSpec),
                       DBException,
                       11134200);

    bsonSpec = BSON("$_internalSearchIdLookup" << 5);
    ASSERT_THROWS_CODE(extension::sdk::HostServicesAPI::getInstance()->createIdLookup(bsonSpec),
                       DBException,
                       11134200);
}

TEST_F(HostServicesTest, NestedFilterParseNodeBasicPredicate) {
    auto filterSpec = BSON("filter" << BSON("status" << "A"));
    auto parseNode = new extension::sdk::ExtensionAggStageParseNodeAdapter(
        std::make_unique<NestedFilterParseNode>(filterSpec));
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts, getExpCtx()};
    auto queryShape = handle->getQueryShape(adapter);
    ASSERT_BSONOBJ_EQ(
        queryShape,
        BSON("$nestedFilter" << BSON("filter" << BSON("status" << BSON("$eq" << "?string")))));
}

TEST_F(HostServicesTest, NestedFilterParseNodeComplexPredicate) {
    auto filterSpec =
        BSON("filter" << BSON("$and" << BSON_ARRAY(
                                  BSON("status" << BSON("$not" << BSON("$eq" << "true")))
                                  << BSON("age" << BSON("$gt" << 30))
                                  << BSON("$or" << BSON_ARRAY(BSON("score" << BSON("$gte" << 80))
                                                              << BSON("level" << "advanced"))))));
    auto parseNode = new extension::sdk::ExtensionAggStageParseNodeAdapter(
        std::make_unique<NestedFilterParseNode>(filterSpec));
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts, getExpCtx()};
    auto queryShape = handle->getQueryShape(adapter);
    ASSERT_BSONOBJ_EQ(
        queryShape,
        BSON("$nestedFilter" << BSON(
                 "filter" << BSON(
                     "$and" << BSON_ARRAY(
                         BSON("status" << BSON("$not" << BSON("$eq" << "?string")))
                         << BSON("age" << BSON("$gt" << "?number"))
                         << BSON("$or"
                                 << BSON_ARRAY(BSON("score" << BSON("$gte" << "?number"))
                                               << BSON("level" << BSON("$eq" << "?string")))))))));
}

TEST_F(HostServicesTest, NestedFilterParseNodeRejectsInvalidPredicate1) {
    auto filterSpec = BSON("filter" << BSON("$invalidOperator" << 5));
    auto parseNode = new extension::sdk::ExtensionAggStageParseNodeAdapter(
        std::make_unique<NestedFilterParseNode>(filterSpec));
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts{};
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts, getExpCtx()};

    ASSERT_THROWS_CODE(handle->getQueryShape(adapter), DBException, ErrorCodes::BadValue);
}

TEST_F(HostServicesTest, NestedFilterParseNodeRejectsInvalidPredicate2) {
    // The inner $or predicate must take an array, so it is rejected.
    auto filterSpec = BSON(
        "filter" << BSON("$and" << BSON_ARRAY(BSON("status" << "A") << BSON("$or" << BSONObj()))));
    auto parseNode = new extension::sdk::ExtensionAggStageParseNodeAdapter(
        std::make_unique<NestedFilterParseNode>(filterSpec));
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts{};
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts, getExpCtx()};

    ASSERT_THROWS_CODE(handle->getQueryShape(adapter), DBException, ErrorCodes::BadValue);
}

// Helper that assembles the full $_internalDocumentResultsAndMetadata stage BSON from an inner
// source spec and an optional metadata variable name.
BSONObj makeDrmStageSpec(BSONObj sourceSpec, StringData varName = StringData{}) {
    BSONObjBuilder stageBuilder;
    {
        BSONObjBuilder innerBuilder(
            stageBuilder.subobjStart(DocumentSourceInternalDocumentResultsAndMetadata::kStageName));
        innerBuilder.append("source", sourceSpec);
        if (!varName.empty()) {
            innerBuilder.append("metadata", BSON("as" << varName));
        }
    }
    return stageBuilder.obj();
}

TEST_F(HostServicesTest, CreateDocumentResultsAndMetadata_EmptySpecFails) {
    BSONObj emptySpec;
    ASSERT_THROWS_CODE(
        extension::sdk::HostServicesAPI::getInstance()->createDocumentResultsAndMetadata(emptySpec),
        DBException,
        12601501);
}

TEST_F(HostServicesTest, CreateDocumentResultsAndMetadata_WrongStageNameFails) {
    BSONObj wrongStage = BSON("$_internalSearchIdLookup" << BSONObj());
    ASSERT_THROWS_CODE(
        extension::sdk::HostServicesAPI::getInstance()->createDocumentResultsAndMetadata(
            wrongStage),
        DBException,
        12601501);
}

TEST_F(HostServicesTest, CreateDocumentResultsAndMetadata_ValidSpecWithMetadata) {
    BSONObj stageSpec = makeDrmStageSpec(BSON("$collStats" << BSONObj()), "SEARCH_META");
    auto hostAstNode =
        extension::sdk::HostServicesAPI::getInstance()->createDocumentResultsAndMetadata(stageSpec);
    ASSERT_EQ(hostAstNode->getName(),
              std::string(DocumentSourceInternalDocumentResultsAndMetadata::kStageName));
}

TEST_F(HostServicesTest, CreateDocumentResultsAndMetadata_ValidSpecWithoutMetadata) {
    BSONObj stageSpec = makeDrmStageSpec(BSON("$collStats" << BSONObj()));
    auto hostAstNode =
        extension::sdk::HostServicesAPI::getInstance()->createDocumentResultsAndMetadata(stageSpec);
    ASSERT_EQ(hostAstNode->getName(),
              std::string(DocumentSourceInternalDocumentResultsAndMetadata::kStageName));
}

TEST_F(HostServicesTest, CreateDocumentResultsAndMetadata_NestedSourceSpecWithMetadata) {
    // A non-trivial source spec with nested objects and arrays must flow through unchanged.
    BSONObj sourceSpec =
        BSON("$search" << BSON("index" << "default" << "text"
                                       << BSON("query" << "foo" << "path" << BSON_ARRAY("a" << "b"))
                                       << "returnStoredSource" << true));
    BSONObj stageSpec = makeDrmStageSpec(sourceSpec, "SEARCH_META");
    auto hostAstNode =
        extension::sdk::HostServicesAPI::getInstance()->createDocumentResultsAndMetadata(stageSpec);
    ASSERT_EQ(hostAstNode->getName(),
              std::string(DocumentSourceInternalDocumentResultsAndMetadata::kStageName));
}

TEST_F(HostServicesTest, CreateDocumentResultsAndMetadata_NestedSourceSpecWithoutMetadata) {
    BSONObj sourceSpec =
        BSON("$search" << BSON("index" << "default" << "text"
                                       << BSON("query" << "foo" << "path" << BSON_ARRAY("a" << "b"))
                                       << "returnStoredSource" << true));
    BSONObj stageSpec = makeDrmStageSpec(sourceSpec);
    auto hostAstNode =
        extension::sdk::HostServicesAPI::getInstance()->createDocumentResultsAndMetadata(stageSpec);
    ASSERT_EQ(hostAstNode->getName(),
              std::string(DocumentSourceInternalDocumentResultsAndMetadata::kStageName));
}

TEST_F(HostServicesTest, CreateDocumentResultsAndMetadata_NodeOutlivesStageSpecBuffer) {
    // The node must remain valid after the caller's BSON is destroyed, exercising the owned copy of
    // the stage BSON taken during construction.
    extension::AggStageAstNodeHandle hostAstNode = [] {
        BSONObj stageSpec = makeDrmStageSpec(
            BSON("$collStats" << BSON("latencyStats" << BSONObj())), "SEARCH_META");
        return extension::sdk::HostServicesAPI::getInstance()->createDocumentResultsAndMetadata(
            stageSpec);
    }();
    ASSERT_EQ(hostAstNode->getName(),
              std::string(DocumentSourceInternalDocumentResultsAndMetadata::kStageName));
}

}  // namespace
}  // namespace mongo::extension
