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

#include "mongo/db/extension/host/document_source_extension.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/extension/host/aggregation_stage/parse_node.h"
#include "mongo/db/extension/host/host_portal.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension {

auto nss = NamespaceString::createNamespaceString_forTest("document_source_extension_test"_sd);

class DocumentSourceExtensionTest : public AggregationContextFixture {
public:
    DocumentSourceExtensionTest() : DocumentSourceExtensionTest(nss) {}
    explicit DocumentSourceExtensionTest(NamespaceString nsString)
        : AggregationContextFixture(std::move(nsString)) {};

    extension::host::DocumentSourceExtension::ExtensionBase getExtensionBase(
        const mongo::extension::host::DocumentSourceExtension* stage) {
        return stage->extensionBase();
    }

    /**
     * Helper to create test pipeline.
     */
    std::unique_ptr<Pipeline> buildTestPipeline(const std::vector<BSONObj>& rawPipeline) {
        auto expCtx = getExpCtx();
        expCtx->setNamespaceString(_nss);
        expCtx->setInRouter(false);

        return Pipeline::parse(rawPipeline, expCtx);
    }

    // Runs after each individual test
    void tearDown() override {
        DocumentSourceExtensionTest::unregisterParsers();
    }

    static void unregisterParsers() {
        host::DocumentSourceExtension::unregisterParser_forTest(
            sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName);
    }

protected:
    NamespaceString _nss = NamespaceString::createNamespaceString_forTest(
        boost::none, "document_source_extension_test");

    sdk::ExtensionAggStageDescriptor _noOpStaticDescriptor{
        sdk::shared_test_stages::NoOpAggStageDescriptor::make()};

    static inline BSONObj kValidSpec =
        BSON(sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName << BSON("foo" << true));
    static inline BSONObj kInvalidSpec =
        BSON(sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName << BSONObj());
};

TEST_F(DocumentSourceExtensionTest, parseNoOpSuccess) {
    // Try to parse pipeline with custom extension stage before registering the extension,
    // should fail.
    std::vector<BSONObj> testPipeline{kValidSpec};
    ASSERT_THROWS_CODE(buildTestPipeline(testPipeline), AssertionException, 16436);
    // Register the extension stage and try to reparse.
    host::HostPortal::registerStageDescriptor(
        reinterpret_cast<const ::MongoExtensionAggStageDescriptor*>(&_noOpStaticDescriptor));
    auto parsedPipeline = buildTestPipeline(testPipeline);
    ASSERT(parsedPipeline);

    ASSERT_EQUALS(parsedPipeline->size(), 1u);
    const auto* stagePtr = parsedPipeline->peekFront();
    ASSERT_TRUE(stagePtr != nullptr);
    ASSERT_EQUALS(std::string(stagePtr->getSourceName()),
                  sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName);
    auto serializedPipeline = parsedPipeline->serializeToBson();
    ASSERT_EQUALS(serializedPipeline.size(), 1u);
    ASSERT_BSONOBJ_EQ(serializedPipeline[0], kValidSpec);

    const auto* documentSourceExtension =
        dynamic_cast<const host::DocumentSourceExtension*>(stagePtr);
    ASSERT_TRUE(documentSourceExtension != nullptr);
    auto extensionStage =
        exec::agg::buildStage(const_cast<host::DocumentSourceExtension*>(documentSourceExtension));
    // Ensure our stage is indeed a NoOp.
    ASSERT_TRUE(extensionStage->getNext().isEOF());
    Document inputDoc = Document{{"foo", 1}};
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    extensionStage->setSource(mock.get());
    auto next = extensionStage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), inputDoc);

    {
        // Test that a parsing failure correctly rethrows the uassert.
        std::vector<BSONObj> testPipeline{kInvalidSpec};
        ASSERT_THROWS_CODE(buildTestPipeline(testPipeline), AssertionException, 10596407);
    }
}

namespace {
static constexpr std::string_view kExpandToExtAstName = "$expandToExtAst";
static constexpr std::string_view kExpandToExtParseName = "$expandToExtParse";
static constexpr std::string_view kExpandToHostParseName = "$expandToHostParse";
static constexpr std::string_view kExpandToMixedName = "$expandToMixed";

static const BSONObj kMatchSpec = BSON("$match" << BSON("a" << 1));

class ExpandToExtAstParseNode : public sdk::AggStageParseNode {
public:
    ExpandToExtAstParseNode() : sdk::AggStageParseNode(kExpandToExtAstName) {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> out;
        out.reserve(kExpansionSize);
        out.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<sdk::shared_test_stages::NoOpAggStageAstNode>()));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return BSONObj();
    }
};

class ExpandToExtParseParseNode : public sdk::AggStageParseNode {
public:
    static int expandCalls;
    static constexpr size_t kExpansionSize = 1;

    ExpandToExtParseParseNode() : sdk::AggStageParseNode(kExpandToExtParseName) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<sdk::VariantNode> expand() const override {
        ++expandCalls;
        std::vector<sdk::VariantNode> out;
        out.reserve(kExpansionSize);
        out.emplace_back(new sdk::ExtensionAggStageParseNode(
            std::make_unique<sdk::shared_test_stages::NoOpAggStageParseNode>()));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return BSONObj();
    }
};

int ExpandToExtParseParseNode::expandCalls = 0;

class NoOpHostParseNode : public host::AggStageParseNode {
public:
    static inline std::unique_ptr<host::AggStageParseNode> make(BSONObj spec) {
        return std::make_unique<NoOpHostParseNode>(spec);
    }
};

class ExpandToHostParseParseNode : public sdk::AggStageParseNode {
public:
    ExpandToHostParseParseNode() : sdk::AggStageParseNode(kExpandToHostParseName) {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> out;
        out.reserve(kExpansionSize);
        out.emplace_back(new host::HostAggStageParseNode(NoOpHostParseNode::make(kMatchSpec)));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return BSONObj();
    }
};

class ExpandToMixedParseNode : public sdk::AggStageParseNode {
public:
    ExpandToMixedParseNode() : sdk::AggStageParseNode(kExpandToMixedName) {}

    static constexpr size_t kExpansionSize = 3;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> out;
        out.reserve(kExpansionSize);
        out.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<sdk::shared_test_stages::NoOpAggStageAstNode>()));
        out.emplace_back(new sdk::ExtensionAggStageParseNode(
            std::make_unique<sdk::shared_test_stages::NoOpAggStageParseNode>()));
        out.emplace_back(new host::HostAggStageParseNode(NoOpHostParseNode::make(kMatchSpec)));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return BSONObj();
    }
};
}  // namespace

TEST_F(DocumentSourceExtensionTest, ExpandToExtAst) {
    auto rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<ExpandToExtAstParseNode>());
    host_connector::AggStageParseNodeHandle rootHandle{rootParseNode};
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(kExpandToExtAstName), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 1);

    // Expanded pipeline contains LiteParsedExpanded.
    auto* first =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded.front().get());
    ASSERT_TRUE(first != nullptr);
    ASSERT_EQ(first->getParseTimeName(), std::string(sdk::shared_test_stages::kNoOpName));
}

TEST_F(DocumentSourceExtensionTest, ExpandToExtParse) {
    auto rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<ExpandToExtParseParseNode>());
    host_connector::AggStageParseNodeHandle rootHandle{rootParseNode};
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(kExpandToExtParseName), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 1);

    // Expanded pipeline contains LiteParsedExpanded.
    auto* first =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded.front().get());
    ASSERT_TRUE(first != nullptr);
    ASSERT_EQ(first->getParseTimeName(), std::string(sdk::shared_test_stages::kNoOpName));
}

TEST_F(DocumentSourceExtensionTest, ExpandToHostParse) {
    auto rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<ExpandToHostParseParseNode>());
    host_connector::AggStageParseNodeHandle rootHandle{rootParseNode};

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(kExpandToHostParseName), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 1);

    // Expanded pipeline contains LiteParsedDocumentSource.
    auto* lpds = dynamic_cast<LiteParsedDocumentSource*>(expanded.front().get());
    ASSERT_TRUE(lpds != nullptr);
    ASSERT_EQ(lpds->getParseTimeName(), std::string(DocumentSourceMatch::kStageName));

    // It is not an instance of LiteParsedExpanded.
    auto* notExpanded =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded.front().get());
    ASSERT_TRUE(notExpanded == nullptr);
}

TEST_F(DocumentSourceExtensionTest, ExpandToMixed) {
    auto rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<ExpandToMixedParseNode>());
    host_connector::AggStageParseNodeHandle rootHandle{rootParseNode};
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(kExpandToMixedName), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 3);

    const auto it0 = expanded.begin();
    const auto it1 = std::next(expanded.begin(), 1);
    const auto it2 = std::next(expanded.begin(), 2);

    auto* first = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it0->get());
    ASSERT_TRUE(first != nullptr);

    auto* second = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it1->get());
    ASSERT_TRUE(second != nullptr);

    auto* third = dynamic_cast<LiteParsedDocumentSource*>(it2->get());
    ASSERT_TRUE(third != nullptr);
    auto* notExpanded =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it2->get());
    ASSERT_TRUE(notExpanded == nullptr);

    ASSERT_EQ(first->getParseTimeName(), std::string(sdk::shared_test_stages::kNoOpName));
    ASSERT_EQ(second->getParseTimeName(), std::string(sdk::shared_test_stages::kNoOpName));
    ASSERT_EQ(third->getParseTimeName(), std::string(DocumentSourceMatch::kStageName));
}

TEST_F(DocumentSourceExtensionTest, ExpandedPipelineIsComputedOnce) {
    ExpandToExtParseParseNode::expandCalls = 0;

    auto rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<ExpandToExtParseParseNode>());
    host_connector::AggStageParseNodeHandle rootHandle{rootParseNode};
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(kExpandToExtParseName), std::move(rootHandle), _nss, LiteParserOptions{});

    // expand() was called during LiteParsedExpandable construction
    ASSERT_EQUALS(ExpandToExtParseParseNode::expandCalls, 1);

    // Cached expanded pipeline is accessed.
    [[maybe_unused]] const auto& firstExpanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(ExpandToExtParseParseNode::expandCalls, 1);

    // Cached expanded pipeline is accessed.
    [[maybe_unused]] const auto& secondExpanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(ExpandToExtParseParseNode::expandCalls, 1);
}

namespace {
static constexpr std::string_view kExpandToHostBadSpecName = "$expandToHostBadSpec";
static const BSONObj kBadQuerySettingsSpec = BSON("$querySettings" << 1);

class ExpandToHostParseBadSpecParseNode : public sdk::AggStageParseNode {
public:
    ExpandToHostParseBadSpecParseNode() : sdk::AggStageParseNode(kExpandToHostBadSpecName) {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> out;
        out.reserve(kExpansionSize);
        // $querySettings stage expects a document as argument
        out.emplace_back(
            new host::HostAggStageParseNode(NoOpHostParseNode::make(kBadQuerySettingsSpec)));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return BSONObj();
    }
};
}  // namespace

TEST_F(DocumentSourceExtensionTest, ExpandPropagatesHostLiteParseFailure) {
    auto* rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<ExpandToHostParseBadSpecParseNode>());
    host_connector::AggStageParseNodeHandle rootHandle{rootParseNode};

    ASSERT_THROWS_CODE(
        [&] {
            host::DocumentSourceExtension::LiteParsedExpandable lp(
                std::string(kExpandToHostBadSpecName), std::move(rootHandle), _nss, {});
        }(),
        AssertionException,
        7746800);
}

namespace {
static constexpr std::string_view kTopName = "$top";
static constexpr std::string_view kMidAName = "$midA";
static constexpr std::string_view kMidBName = "$midB";
static constexpr std::string_view kLeafAName = "$leafA";
static constexpr std::string_view kLeafBName = "$leafB";
static constexpr std::string_view kLeafCName = "$leafC";
static constexpr std::string_view kLeafDName = "$leafD";

class LeafAAstNode : public sdk::AggStageAstNode {
public:
    LeafAAstNode() : sdk::AggStageAstNode(kLeafAName) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
    }
};

class LeafBAstNode : public sdk::AggStageAstNode {
public:
    LeafBAstNode() : sdk::AggStageAstNode(kLeafBName) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
    }
};

class LeafCAstNode : public sdk::AggStageAstNode {
public:
    LeafCAstNode() : sdk::AggStageAstNode(kLeafCName) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
    }
};

class LeafDAstNode : public sdk::AggStageAstNode {
public:
    LeafDAstNode() : sdk::AggStageAstNode(kLeafDName) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
    }
};

class MidAParseNode : public sdk::AggStageParseNode {
public:
    MidAParseNode() : sdk::AggStageParseNode(kMidAName) {}

    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> out;
        out.reserve(kExpansionSize);
        out.emplace_back(new sdk::ExtensionAggStageAstNode(std::make_unique<LeafAAstNode>()));
        out.emplace_back(new sdk::ExtensionAggStageAstNode(std::make_unique<LeafBAstNode>()));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return {};
    }
};

class MidBParseNode : public sdk::AggStageParseNode {
public:
    MidBParseNode() : sdk::AggStageParseNode(kMidBName) {}

    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> out;
        out.reserve(kExpansionSize);
        out.emplace_back(new sdk::ExtensionAggStageAstNode(std::make_unique<LeafCAstNode>()));
        out.emplace_back(new sdk::ExtensionAggStageAstNode(std::make_unique<LeafDAstNode>()));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return {};
    }
};

class TopParseNode : public sdk::AggStageParseNode {
public:
    TopParseNode() : sdk::AggStageParseNode(kTopName) {}

    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> out;
        out.reserve(kExpansionSize);
        out.emplace_back(new sdk::ExtensionAggStageParseNode(std::make_unique<MidAParseNode>()));
        out.emplace_back(new sdk::ExtensionAggStageParseNode(std::make_unique<MidBParseNode>()));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return {};
    }
};
}  // namespace

TEST_F(DocumentSourceExtensionTest, ExpandRecursesMultipleLevels) {
    auto rootParseNode = new sdk::ExtensionAggStageParseNode(std::make_unique<TopParseNode>());
    host_connector::AggStageParseNodeHandle rootHandle{rootParseNode};
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(kTopName), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 4);

    const auto it0 = expanded.begin();
    const auto it1 = std::next(expanded.begin(), 1);
    const auto it2 = std::next(expanded.begin(), 2);
    const auto it3 = std::next(expanded.begin(), 3);

    auto* first = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it0->get());
    ASSERT_TRUE(first != nullptr);
    ASSERT_EQ(first->getParseTimeName(), std::string(kLeafAName));

    auto* second = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it1->get());
    ASSERT_TRUE(second != nullptr);
    ASSERT_EQ(second->getParseTimeName(), std::string(kLeafBName));

    auto* third = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it2->get());
    ASSERT_TRUE(third != nullptr);
    ASSERT_EQ(third->getParseTimeName(), std::string(kLeafCName));

    auto* fourth = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it3->get());
    ASSERT_TRUE(fourth != nullptr);
    ASSERT_EQ(fourth->getParseTimeName(), std::string(kLeafDName));
}

TEST_F(DocumentSourceExtensionTest, extensionBaseSucceeds) {
    // Register the extension stage and parse a simple pipeline using it.
    extension::host::HostPortal::registerStageDescriptor(
        reinterpret_cast<const ::MongoExtensionAggStageDescriptor*>(&_noOpStaticDescriptor));

    std::vector<BSONObj> testPipeline{kValidSpec};
    auto parsedPipeline = buildTestPipeline(testPipeline);
    ASSERT(parsedPipeline);
    ASSERT_EQUALS(parsedPipeline->size(), 1u);

    const auto* stagePtr = parsedPipeline->peekFront();
    ASSERT_TRUE(stagePtr != nullptr);
    const auto* documentSourceExtension =
        dynamic_cast<const extension::host::DocumentSourceExtension*>(stagePtr);
    ASSERT_TRUE(documentSourceExtension != nullptr);

    // Extract ExtensionBase from the existing stage and validate core fields.
    auto init = getExtensionBase(documentSourceExtension);
    ASSERT_EQUALS(std::string(init.name),
                  extension::sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName);
    ASSERT_TRUE(init.exprCtx.get() == stagePtr->getExpCtx().get());
    ASSERT_BSONOBJ_EQ(init.rawStage, kValidSpec);
}

}  // namespace mongo::extension
