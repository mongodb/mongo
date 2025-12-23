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
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/extension/host/aggregation_stage/parse_node.h"
#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host/host_portal.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/sdk/tests/fruits_test_stage.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension {

auto nss = NamespaceString::createNamespaceString_forTest("document_source_extension_test"_sd);

class DocumentSourceExtensionTest : public AggregationContextFixture {
public:
    DocumentSourceExtensionTest() : DocumentSourceExtensionTest(nss) {}
    explicit DocumentSourceExtensionTest(NamespaceString nsString)
        : AggregationContextFixture(std::move(nsString)) {};

    void setUp() override {
        AggregationContextFixture::setUp();
        extension::sdk::HostServicesAPI::setHostServices(
            &extension::host_connector::HostServicesAdapter::get());
    }

    /**
     * Helper to create test pipeline.
     */
    std::unique_ptr<Pipeline> buildTestPipeline(const std::vector<BSONObj>& rawPipeline) {
        auto expCtx = getExpCtx();
        expCtx->setNamespaceString(_nss);
        expCtx->setInRouter(false);

        return pipeline_factory::makePipeline(
            rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    }

    BSONObj createDummySpecFromStageName(std::string_view stageName) {
        return BSON(std::string(stageName) << BSONObj());
    }

protected:
    NamespaceString _nss = NamespaceString::createNamespaceString_forTest(
        boost::none, "document_source_extension_test");

    sdk::ExtensionAggStageDescriptor _transformStaticDescriptor{
        sdk::shared_test_stages::TransformAggStageDescriptor::make()};

    static inline BSONObj kValidSpec = BSON(
        sdk::shared_test_stages::TransformAggStageDescriptor::kStageName << BSON("foo" << true));
    static inline BSONObj kInvalidSpec =
        BSON(sdk::shared_test_stages::TransformAggStageDescriptor::kStageName << BSONObj());
};

TEST_F(DocumentSourceExtensionTest, ParseTransformSuccess) {
    // Try to parse pipeline with custom extension stage before registering the extension,
    // should fail.
    std::vector<BSONObj> testPipeline{kValidSpec};
    ASSERT_THROWS_CODE(buildTestPipeline(testPipeline), AssertionException, 40324);
    // Register the extension stage and try to reparse.

    std::unique_ptr<host::HostPortal> hostPortal = std::make_unique<host::HostPortal>();
    host_connector::HostPortalAdapter portal{
        MONGODB_EXTENSION_API_VERSION, 1, "", std::move(hostPortal)};
    portal.getImpl().registerStageDescriptor(&_transformStaticDescriptor);

    auto parsedPipeline = buildTestPipeline(testPipeline);
    ASSERT(parsedPipeline);

    ASSERT_EQUALS(parsedPipeline->size(), 1u);
    const auto* stagePtr = parsedPipeline->peekFront();
    ASSERT_TRUE(stagePtr != nullptr);
    ASSERT_EQUALS(std::string(stagePtr->getSourceName()),
                  sdk::shared_test_stages::TransformAggStageDescriptor::kStageName);
    auto serializedPipeline =
        parsedPipeline->serializeToBson(SerializationOptions::kDebugQueryShapeSerializeOptions);
    ASSERT_EQUALS(serializedPipeline.size(), 1u);
    // The extension is in the form of DocumentSourceExtensionExpandable at this point, which
    // serializes to its query shape. The transform extension's query shape is just its stage
    // definition.
    ASSERT_BSONOBJ_EQ(serializedPipeline[0], kValidSpec);
}

TEST_F(DocumentSourceExtensionTest, ExpandToExtAst) {
    auto rootParseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::ExpandToExtAstParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    BSONObj stageBson = createDummySpecFromStageName(sdk::shared_test_stages::kExpandToExtAstName);
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 1);

    // Expanded pipeline contains LiteParsedExpanded.
    auto* first =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded[0].get());
    ASSERT_TRUE(first != nullptr);
    ASSERT_EQ(first->getParseTimeName(), std::string(sdk::shared_test_stages::kTransformName));
}

TEST_F(DocumentSourceExtensionTest, ExpandToExtParse) {
    auto rootParseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::ExpandToExtParseParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    BSONObj stageBson =
        createDummySpecFromStageName(sdk::shared_test_stages::kExpandToExtParseName);
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 1);

    // Expanded pipeline contains LiteParsedExpanded.
    auto* first =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded[0].get());
    ASSERT_TRUE(first != nullptr);
    ASSERT_EQ(first->getParseTimeName(), std::string(sdk::shared_test_stages::kTransformName));
}

TEST_F(DocumentSourceExtensionTest, ExpandToHostParse) {
    auto rootParseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::ExpandToHostParseParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    BSONObj stageBson =
        createDummySpecFromStageName(sdk::shared_test_stages::kExpandToHostParseName);

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 1);

    // Expanded pipeline contains LiteParsedDocumentSource.
    auto* lpds = dynamic_cast<LiteParsedDocumentSource*>(expanded[0].get());
    ASSERT_TRUE(lpds != nullptr);
    ASSERT_EQ(lpds->getParseTimeName(), std::string(DocumentSourceMatch::kStageName));

    // It is not an instance of LiteParsedExpanded.
    auto* notExpanded =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded[0].get());
    ASSERT_TRUE(notExpanded == nullptr);
}

TEST_F(DocumentSourceExtensionTest, ExpandToMixed) {
    auto rootParseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::ExpandToMixedParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    BSONObj stageBson = createDummySpecFromStageName(sdk::shared_test_stages::kExpandToMixedName);
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 4);

    auto* first =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded[0].get());
    ASSERT_TRUE(first != nullptr);

    auto* second =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded[1].get());
    ASSERT_TRUE(second != nullptr);

    // This one is NOT LiteParsedExpanded.
    auto* third = expanded[2].get();
    ASSERT_TRUE(third != nullptr);
    ASSERT_TRUE(dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(third) == nullptr);

    auto* fourth = expanded[3].get();
    ASSERT_TRUE(fourth != nullptr);

    ASSERT_EQ(first->getParseTimeName(), std::string(sdk::shared_test_stages::kTransformName));
    ASSERT_EQ(second->getParseTimeName(), std::string(sdk::shared_test_stages::kTransformName));
    ASSERT_EQ(third->getParseTimeName(), std::string(DocumentSourceMatch::kStageName));
    ASSERT_EQ(fourth->getParseTimeName(),
              std::string(DocumentSourceInternalSearchIdLookUp::kStageName));
}

TEST_F(DocumentSourceExtensionTest, ExpandedPipelineIsComputedOnce) {
    sdk::shared_test_stages::ExpandToExtParseParseNode::expandCalls = 0;

    auto rootParseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::ExpandToExtParseParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    BSONObj stageBson =
        createDummySpecFromStageName(sdk::shared_test_stages::kExpandToExtParseName);
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(rootHandle), _nss, LiteParserOptions{});

    // expand() was called during LiteParsedExpandable construction
    ASSERT_EQUALS(sdk::shared_test_stages::ExpandToExtParseParseNode::expandCalls, 1);

    // Cached expanded pipeline is accessed.
    [[maybe_unused]] const auto& firstExpanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(sdk::shared_test_stages::ExpandToExtParseParseNode::expandCalls, 1);

    // Cached expanded pipeline is accessed.
    [[maybe_unused]] const auto& secondExpanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(sdk::shared_test_stages::ExpandToExtParseParseNode::expandCalls, 1);
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

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> out;
        out.reserve(kExpansionSize);
        // $querySettings stage expects a document as argument
        out.emplace_back(new host::HostAggStageParseNode(
            sdk::shared_test_stages::TransformHostParseNode::make(kBadQuerySettingsSpec)));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return BSONObj();
    }
};
}  // namespace

TEST_F(DocumentSourceExtensionTest, ExpandToHostAst) {
    auto rootParseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::ExpandToHostAstParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};

    BSONObj stageBson = createDummySpecFromStageName(sdk::shared_test_stages::kExpandToHostAstName);
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 1);

    // Expanded pipeline contains LiteParsedDocumentSource.
    auto* lpds = dynamic_cast<LiteParsedDocumentSource*>(expanded[0].get());
    ASSERT_TRUE(lpds != nullptr);
    ASSERT_EQ(lpds->getParseTimeName(),
              std::string(DocumentSourceInternalSearchIdLookUp::kStageName));
}

TEST_F(DocumentSourceExtensionTest, ExpandPropagatesHostLiteParseFailure) {
    auto* rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<ExpandToHostParseBadSpecParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};

    BSONObj stageBson = createDummySpecFromStageName(kExpandToHostBadSpecName);
    ASSERT_THROWS_CODE(
        [&] {
            host::DocumentSourceExtension::LiteParsedExpandable lp(
                stageBson.firstElement(), std::move(rootHandle), _nss, {});
        }(),
        AssertionException,
        7746800);
}

TEST_F(DocumentSourceExtensionTest, ExpandRecursesMultipleLevels) {
    auto rootParseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::TopParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    BSONObj stageBson = createDummySpecFromStageName(sdk::shared_test_stages::kTopName);
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 4);

    auto* first =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded[0].get());
    ASSERT_TRUE(first != nullptr);
    ASSERT_EQ(first->getParseTimeName(), std::string(sdk::shared_test_stages::kLeafAName));

    auto* second =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded[1].get());
    ASSERT_TRUE(second != nullptr);
    ASSERT_EQ(second->getParseTimeName(), std::string(sdk::shared_test_stages::kLeafBName));

    auto* third =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded[2].get());
    ASSERT_TRUE(third != nullptr);
    ASSERT_EQ(third->getParseTimeName(), std::string(sdk::shared_test_stages::kLeafCName));

    auto* fourth =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded[3].get());
    ASSERT_TRUE(fourth != nullptr);
    ASSERT_EQ(fourth->getParseTimeName(), std::string(sdk::shared_test_stages::kLeafDName));
}

namespace {
static constexpr std::string_view kDepthLeafName = "$depthLeaf";
static constexpr std::string_view kAdjCycleName = "$adjacentCycle";
static constexpr std::string_view kNodeAName = "$nodeA";
static constexpr std::string_view kNodeBName = "$nodeB";
static constexpr std::string_view kTopSameNameChildren = "$topSameNameChildren";

// Leaf AST used by several tests
class DepthLeafAstNode : public sdk::AggStageAstNode {
public:
    DepthLeafAstNode() : sdk::AggStageAstNode(kDepthLeafName) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::TransformLogicalAggStage>();
    }
};

// Helper to ensure each recursive stage used for depth checking has a unique name.
static std::string_view makeRecursiveDepthName(int remaining) {
    static thread_local std::string buf;
    buf = str::stream() << "$depthChain#" << remaining;
    return std::string_view{buf.data(), buf.size()};
}

// Depth chain builder that emits a single recursive child until depth = 0, at which it ends by
// emitting a leaf AST.
class DepthChainParseNode : public sdk::AggStageParseNode {
public:
    DepthChainParseNode(int remaining)
        : sdk::AggStageParseNode(makeRecursiveDepthName(remaining)), _remaining(remaining) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> out;
        out.reserve(1);
        if (_remaining > 0) {
            out.emplace_back(new sdk::ExtensionAggStageParseNode(
                std::make_unique<DepthChainParseNode>(_remaining - 1)));
        } else {
            out.emplace_back(
                new sdk::ExtensionAggStageAstNode(std::make_unique<DepthLeafAstNode>()));
        }
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return {};
    }

private:
    int _remaining;
};

// Adjacent cycle where a stage expands into itself: A -> A
class AdjacentCycleParseNode : public sdk::AggStageParseNode {
public:
    AdjacentCycleParseNode() : sdk::AggStageParseNode(kAdjCycleName) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> out;
        out.reserve(1);
        out.emplace_back(
            new sdk::ExtensionAggStageParseNode(std::make_unique<AdjacentCycleParseNode>()));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return {};
    }
};

// Non-adjacent cycle where a stage expands into a stage that then expands into itself: A -> B -> A
class NodeAParseNode : public sdk::AggStageParseNode {
public:
    NodeAParseNode() : sdk::AggStageParseNode(kNodeAName) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<VariantNodeHandle> expand() const override;

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return {};
    }
};

class NodeBParseNode : public sdk::AggStageParseNode {
public:
    NodeBParseNode() : sdk::AggStageParseNode(kNodeBName) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<VariantNodeHandle> expand() const override;

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return {};
    }
};

std::vector<VariantNodeHandle> NodeAParseNode::expand() const {
    std::vector<VariantNodeHandle> out;
    out.reserve(1);
    out.emplace_back(new sdk::ExtensionAggStageParseNode(std::make_unique<NodeBParseNode>()));
    return out;
}

std::vector<VariantNodeHandle> NodeBParseNode::expand() const {
    std::vector<VariantNodeHandle> out;
    out.reserve(1);
    out.emplace_back(new sdk::ExtensionAggStageParseNode(std::make_unique<NodeAParseNode>()));
    return out;
}

// Expands into siblings with the same stage name. They are expanded on separate paths, so this must
// succeed.
class TopSameNameChildrenParseNode : public sdk::AggStageParseNode {
public:
    TopSameNameChildrenParseNode() : sdk::AggStageParseNode(kTopSameNameChildren) {}

    size_t getExpandedSize() const override {
        return 2;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> out;
        out.reserve(2);
        out.emplace_back(new sdk::ExtensionAggStageParseNode(
            std::make_unique<sdk::shared_test_stages::TransformAggStageParseNode>()));
        out.emplace_back(new sdk::ExtensionAggStageParseNode(
            std::make_unique<sdk::shared_test_stages::TransformAggStageParseNode>()));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return {};
    }
};
}  // namespace

TEST_F(DocumentSourceExtensionTest, ExpandToMaxDepthSucceeds) {
    // Chain length 10 -> exactly hits kMaxExpansionDepth (default 10) on deepest frame.
    auto depth = host::DocumentSourceExtension::LiteParsedExpandable::kMaxExpansionDepth;
    auto* rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<DepthChainParseNode>(depth));
    AggStageParseNodeHandle rootHandle{rootParseNode};

    BSONObj stageBson = createDummySpecFromStageName(makeRecursiveDepthName(depth));
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    // Final expansion produces exactly one AST leaf.
    ASSERT_EQUALS(expanded.size(), 1);
    auto* leaf =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded[0].get());
    ASSERT_TRUE(leaf != nullptr);
    ASSERT_EQ(leaf->getParseTimeName(), std::string(kDepthLeafName));
}

using DocumentSourceExtensionTestDeathTest = DocumentSourceExtensionTest;
DEATH_TEST_F(DocumentSourceExtensionTestDeathTest, ExpandExceedsMaxDepthFails, "10955800") {
    auto depth = host::DocumentSourceExtension::LiteParsedExpandable::kMaxExpansionDepth + 1;
    auto* rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<DepthChainParseNode>(depth));
    AggStageParseNodeHandle rootHandle{rootParseNode};

    BSONObj stageBson = createDummySpecFromStageName(makeRecursiveDepthName(depth));
    [[maybe_unused]] host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(rootHandle), _nss, LiteParserOptions{});
}

TEST_F(DocumentSourceExtensionTest, ExpandAdjacentCycleFails) {
    auto* rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<AdjacentCycleParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    BSONObj stageBson = createDummySpecFromStageName(kAdjCycleName);

    ASSERT_THROWS_WITH_CHECK(
        [&] {
            [[maybe_unused]] host::DocumentSourceExtension::LiteParsedExpandable lp(
                stageBson.firstElement(), std::move(rootHandle), _nss, LiteParserOptions{});
        }(),
        AssertionException,
        [](const AssertionException& ex) {
            ASSERT_EQ(ex.code(), 10955801);
            ASSERT_STRING_CONTAINS(ex.reason(),
                                   str::stream()
                                       << "Cycle detected during stage expansion for stage "
                                       << std::string(kAdjCycleName));
            ASSERT_STRING_CONTAINS(ex.reason(), "$adjacentCycle -> $adjacentCycle");
            assertionCount.tripwire.subtractAndFetch(1);
        });
}

TEST_F(DocumentSourceExtensionTest, ExpandNonAdjacentCycleFails) {
    auto* rootParseNode = new sdk::ExtensionAggStageParseNode(std::make_unique<NodeAParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    BSONObj stageBson = createDummySpecFromStageName(kNodeAName);

    ASSERT_THROWS_WITH_CHECK(
        [&] {
            [[maybe_unused]] host::DocumentSourceExtension::LiteParsedExpandable lp(
                stageBson.firstElement(), std::move(rootHandle), _nss, LiteParserOptions{});
        }(),
        AssertionException,
        [](const AssertionException& ex) {
            ASSERT_EQ(ex.code(), 10955801);
            ASSERT_STRING_CONTAINS(ex.reason(),
                                   str::stream()
                                       << "Cycle detected during stage expansion for stage "
                                       << std::string(kNodeBName));
            ASSERT_STRING_CONTAINS(ex.reason(), "$nodeB -> $nodeA -> $nodeB");
            assertionCount.tripwire.subtractAndFetch(1);
        });
}

TEST_F(DocumentSourceExtensionTest, ExpandSameStageOnDifferentBranchesSucceeds) {
    auto* rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<TopSameNameChildrenParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    BSONObj stageBson = createDummySpecFromStageName(kTopSameNameChildren);

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 2);

    auto* first =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded[0].get());
    auto* second =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded[1].get());
    ASSERT_TRUE(first != nullptr);
    ASSERT_TRUE(second != nullptr);

    // Both leaves are the Transform leaf from TransformAggStageParseNode.
    ASSERT_EQ(first->getParseTimeName(), std::string(sdk::shared_test_stages::kTransformName));
    ASSERT_EQ(second->getParseTimeName(), std::string(sdk::shared_test_stages::kTransformName));
}

namespace {
static constexpr std::string_view kExpandToSearchName = "$expandToSearch";
static const BSONObj kSearchSpec = BSON(
    "$search" << BSON("index" << "default" << "text" << BSON("query" << "foo" << "path" << "a")));

class TransformAggStageParseNode : public sdk::AggStageParseNode {
public:
    TransformAggStageParseNode()
        : sdk::AggStageParseNode(sdk::shared_test_stages::kTransformName) {}

    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<sdk::shared_test_stages::TransformAggStageAstNode>()));
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<sdk::shared_test_stages::SearchLikeSourceAggStageAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<TransformAggStageParseNode>();
    }
};

class SearchLikeSourceAggStageParseNode : public sdk::AggStageParseNode {
public:
    SearchLikeSourceAggStageParseNode()
        : sdk::AggStageParseNode(sdk::shared_test_stages::kSearchLikeSourceStageName) {}

    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<sdk::shared_test_stages::SearchLikeSourceAggStageAstNode>()));
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<sdk::shared_test_stages::TransformAggStageAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<SearchLikeSourceAggStageParseNode>();
    }
};

class ExpandToSearchAggStageParseNode : public sdk::AggStageParseNode {
public:
    ExpandToSearchAggStageParseNode() : sdk::AggStageParseNode(kExpandToSearchName) {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new host::HostAggStageParseNode(
            sdk::shared_test_stages::TransformHostParseNode::make(kSearchSpec)));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<ExpandToSearchAggStageParseNode>();
    }
};

class SingleActionRequiredPrivilegesAggStageAstNode : public sdk::AggStageAstNode {
public:
    static constexpr std::string_view kName = "$singleAction";

    SingleActionRequiredPrivilegesAggStageAstNode() : sdk::AggStageAstNode(kName) {}

    BSONObj getProperties() const override {
        return BSON("requiredPrivileges" << BSON_ARRAY(BSON(
                        "resourcePattern" << "namespace"
                                          << "actions" << BSON_ARRAY(BSON("action" << "find")))));
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::TransformLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<SingleActionRequiredPrivilegesAggStageAstNode>();
    }
};

class MultipleActionsRequiredPrivilegesAggStageAstNode : public sdk::AggStageAstNode {
public:
    static constexpr std::string_view kName = "$multipleActions";

    MultipleActionsRequiredPrivilegesAggStageAstNode() : sdk::AggStageAstNode(kName) {}

    BSONObj getProperties() const override {
        return BSON("requiredPrivileges" << BSON_ARRAY(BSON(
                        "resourcePattern" << "namespace"
                                          << "actions"
                                          << BSON_ARRAY(BSON("action" << "listIndexes")
                                                        << BSON("action" << "planCacheRead")))));
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::TransformLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<MultipleActionsRequiredPrivilegesAggStageAstNode>();
    }
};

class MultipleRequiredPrivilegesAggStageAstNode : public sdk::AggStageAstNode {
public:
    static constexpr std::string_view kName = "$multiplePrivileges";

    MultipleRequiredPrivilegesAggStageAstNode() : sdk::AggStageAstNode(kName) {}

    BSONObj getProperties() const override {
        return BSON("requiredPrivileges" << BSON_ARRAY(
                        BSON("resourcePattern" << "namespace"
                                               << "actions" << BSON_ARRAY(BSON("action" << "find")))
                        << BSON("resourcePattern" << "namespace"
                                                  << "actions"
                                                  << BSON_ARRAY(BSON("action" << "indexStats")))));
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::TransformLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<MultipleRequiredPrivilegesAggStageAstNode>();
    }
};

class MultipleChildrenRequiredPrivilegesAggStageParseNode : public sdk::AggStageParseNode {
public:
    static constexpr std::string_view kName = "$multipleChildrenRequiredPrivileges";

    MultipleChildrenRequiredPrivilegesAggStageParseNode() : sdk::AggStageParseNode(kName) {}

    static constexpr size_t kExpansionSize = 6;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new host::HostAggStageParseNode(
            sdk::shared_test_stages::TransformHostParseNode::make(kSearchSpec)));
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<SingleActionRequiredPrivilegesAggStageAstNode>()));
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<sdk::shared_test_stages::TransformAggStageAstNode>()));
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<MultipleActionsRequiredPrivilegesAggStageAstNode>()));
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<MultipleRequiredPrivilegesAggStageAstNode>()));
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<sdk::shared_test_stages::NonePosAggStageAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<MultipleChildrenRequiredPrivilegesAggStageParseNode>();
    }
};

class EmptyActionsArrayRequiredPrivilegesAggStageAstNode : public sdk::AggStageAstNode {
public:
    static constexpr std::string_view kName = "$emptyActions";

    EmptyActionsArrayRequiredPrivilegesAggStageAstNode() : sdk::AggStageAstNode(kName) {}

    BSONObj getProperties() const override {
        return BSON("requiredPrivileges"
                    << BSON_ARRAY(BSON("resourcePattern" << "namespace"
                                                         << "actions" << BSONArray())));
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::TransformLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<EmptyActionsArrayRequiredPrivilegesAggStageAstNode>();
    }
};

class EmptyRequiredPrivilegesAggStageAstNode : public sdk::AggStageAstNode {
public:
    static constexpr std::string_view kName = "$emptyRequiredPrivileges";

    EmptyRequiredPrivilegesAggStageAstNode() : sdk::AggStageAstNode(kName) {}

    BSONObj getProperties() const override {
        return BSON("requiredPrivileges" << BSONArray());
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::TransformLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<EmptyRequiredPrivilegesAggStageAstNode>();
    }
};

static constexpr std::string_view kSourceName = "$sortKeySource";

class ValidateSortKeyMetadataExecStage : public sdk::ExecAggStageSource {
public:
    ValidateSortKeyMetadataExecStage() : sdk::ExecAggStageSource(kSourceName) {}

    ValidateSortKeyMetadataExecStage(std::string_view name, const mongo::BSONObj& arguments)
        : sdk::ExecAggStageSource(name) {}

    ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                   ::MongoExtensionExecAggStage* execStage) override {
        if (_currentIndex >= _documentsWithMetadata.size()) {
            return ExtensionGetNextResult::eof();
        }
        // Note, here we can create the result as a byte view, since this stage guarantees to keep
        // the results valid.
        auto documentResult =
            ExtensionBSONObj::makeAsByteView(_documentsWithMetadata[_currentIndex].first);
        auto metadataResult =
            ExtensionBSONObj::makeAsByteView(_documentsWithMetadata[_currentIndex++].second);
        return ExtensionGetNextResult::advanced(std::move(documentResult),
                                                std::move(metadataResult));
    }
    // Allow this to be public for visibility in unit tests.
    UnownedExecAggStageHandle& _getSource() override {
        return sdk::ExecAggStageSource::_getSource();
    }
    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }
    void open() override {}
    void reopen() override {}
    void close() override {}

    static inline auto getInputResults() {
        return _documentsWithMetadata;
    }

    static inline std::unique_ptr<sdk::ExecAggStageSource> make() {
        return std::make_unique<ValidateSortKeyMetadataExecStage>();
    }

private:
    static inline const std::vector<std::pair<BSONObj, BSONObj>> _documentsWithMetadata = {
        {BSON("_id" << 1 << "field1" << "val1"),
         BSON("$sortKey" << BSON("val1" << 5.0))},  // SingleElement $sortKey.
        {BSON("_id" << 2 << "field2" << "val2"),
         BSON("$sortKey" << BSON("val1" << 1.0 << "val2"
                                        << 2.0))},  // MultiElement $sortKey passed in a obj type.
        {BSON("_id" << 3 << "field3" << "val3"),
         BSON("$sortKey" << BSON_ARRAY(3.0
                                       << 4.0))},  // MultiElement $sortKey passed in a array type.
        {BSON("_id" << 4 << "field4" << "val4"), BSON("$sortKey" << BSONObj())},  // Empty $sortKey.
        {BSON("_id" << 4 << "field4" << "val4"),
         BSON("$sortKey" << 1.0)}};  // $sortKey is not an obj.
    size_t _currentIndex = 0;
};

DEFAULT_LOGICAL_STAGE(ValidateSortKeyMetadata);

class ValidateSortKeyMetadataAstStage
    : public sdk::TestAstNode<ValidateSortKeyMetadataLogicalStage> {
public:
    ValidateSortKeyMetadataAstStage()
        : sdk::TestAstNode<ValidateSortKeyMetadataLogicalStage>(kSourceName, BSONObj()) {}

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<ValidateSortKeyMetadataAstStage>();
    }
};
}  // namespace

TEST_F(DocumentSourceExtensionTest, TransformAstNodeWithDefaultGetPropertiesSucceeds) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::TransformAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};

    host::DocumentSourceExtension::LiteParsedExpanded lp(
        std::string(sdk::shared_test_stages::kTransformName), std::move(handle), _nss);
    ASSERT_FALSE(lp.isInitialSource());
    ASSERT_FALSE(lp.requiresAuthzChecks());
    ASSERT_EQUALS(lp.requiredPrivileges(/*isMongos*/ false, /*bypassDocumentValidation*/ false),
                  PrivilegeVector{});
}

TEST_F(DocumentSourceExtensionTest, TransformParseNodeInheritsDefaultGetPropertiesFromAstNode) {
    auto parseNode = new sdk::ExtensionAggStageParseNode(
        sdk::shared_test_stages::TransformAggStageParseNode::make());
    auto handle = AggStageParseNodeHandle{parseNode};
    BSONObj stageBson = createDummySpecFromStageName(sdk::shared_test_stages::kTransformName);

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(handle), _nss, LiteParserOptions{});
    ASSERT_FALSE(lp.isInitialSource());
    ASSERT_FALSE(lp.requiresAuthzChecks());
    ASSERT_EQUALS(lp.requiredPrivileges(/*isMongos*/ false, /*bypassDocumentValidation*/ false),
                  PrivilegeVector{});
}

TEST_F(DocumentSourceExtensionTest, TransformAggStageAstNodeSucceeds) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::TransformAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};

    host::DocumentSourceExtension::LiteParsedExpanded lp(
        std::string(sdk::shared_test_stages::kTransformName), std::move(handle), _nss);
    ASSERT_FALSE(lp.isInitialSource());
}

TEST_F(DocumentSourceExtensionTest, SearchLikeSourceAggStageAstNode) {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::SearchLikeSourceAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};

    host::DocumentSourceExtension::LiteParsedExpanded lp(
        std::string(sdk::shared_test_stages::kSearchLikeSourceStageName), std::move(handle), _nss);
    ASSERT_TRUE(lp.isInitialSource());
}

TEST_F(DocumentSourceExtensionTest,
       TransformAggStageParseNodeInheritsInitialSourceFromFirstAstNode) {
    auto parseNode = new sdk::ExtensionAggStageParseNode(TransformAggStageParseNode::make());
    auto handle = AggStageParseNodeHandle{parseNode};
    BSONObj stageBson = createDummySpecFromStageName(sdk::shared_test_stages::kTransformName);

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(handle), _nss, LiteParserOptions{});
    ASSERT_FALSE(lp.isInitialSource());
}

TEST_F(DocumentSourceExtensionTest,
       SearchLikeSourceAggStageParseNodeInheritsInitialSourceFromFirstAstNode) {
    auto parseNode = new sdk::ExtensionAggStageParseNode(SearchLikeSourceAggStageParseNode::make());
    auto handle = AggStageParseNodeHandle{parseNode};
    BSONObj stageBson =
        createDummySpecFromStageName(sdk::shared_test_stages::kSearchLikeSourceStageName);

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(handle), _nss, LiteParserOptions{});
    ASSERT_TRUE(lp.isInitialSource());
}

TEST_F(DocumentSourceExtensionTest, ExpandToMatchParseNodeInheritsPropertiesFromMatch) {
    auto parseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::ExpandToHostParseParseNode>());
    auto handle = AggStageParseNodeHandle{parseNode};
    BSONObj stageBson =
        createDummySpecFromStageName(sdk::shared_test_stages::kExpandToHostParseName);

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(handle), _nss, LiteParserOptions{});
    ASSERT_FALSE(lp.isInitialSource());
    ASSERT_FALSE(lp.requiresAuthzChecks());
    ASSERT_EQUALS(lp.requiredPrivileges(/*isMongos*/ false, /*bypassDocumentValidation*/ false),
                  PrivilegeVector{});
}

TEST_F(DocumentSourceExtensionTest, ExpandToSearchParseNodeInheritsPropertiesFromSearch) {
    auto parseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<ExpandToSearchAggStageParseNode>());
    auto handle = AggStageParseNodeHandle{parseNode};
    BSONObj stageBson = createDummySpecFromStageName(kExpandToSearchName);

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(handle), _nss, LiteParserOptions{});
    ASSERT_TRUE(lp.isInitialSource());

    auto privileges = lp.requiredPrivileges(/*isMongos*/ false, /*bypassDocumentValidation*/ false);
    ASSERT_EQ(privileges.size(), 1);

    const auto& privilege = privileges.front();
    ASSERT_TRUE(privilege.getResourcePattern().isExactNamespacePattern());
    ASSERT_EQ(privilege.getResourcePattern().ns(), _nss);

    const auto& actions = privilege.getActions();
    ASSERT_EQ(actions.getActionsAsStringDatas().size(), 1);
    ASSERT_TRUE(actions.contains(ActionType::find));

    ASSERT_TRUE(lp.requiresAuthzChecks());
}

TEST_F(DocumentSourceExtensionTest, EmptyRequiredPrivilegesAstNodeSucceeds) {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(EmptyRequiredPrivilegesAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};

    host::DocumentSourceExtension::LiteParsedExpanded lp(
        std::string(EmptyRequiredPrivilegesAggStageAstNode::kName), std::move(handle), _nss);

    ASSERT_EQUALS(lp.requiredPrivileges(/*isMongos*/ false, /*bypassDocumentValidation*/ false),
                  PrivilegeVector{});
    ASSERT_FALSE(lp.requiresAuthzChecks());
}

TEST_F(DocumentSourceExtensionTest, SingleRequiredPrivilegesAstNodeProducesFindPrivilege) {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(SingleActionRequiredPrivilegesAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};

    host::DocumentSourceExtension::LiteParsedExpanded lp(
        std::string(SingleActionRequiredPrivilegesAggStageAstNode::kName), std::move(handle), _nss);

    auto privileges = lp.requiredPrivileges(/*isMongos*/ false, /*bypassDocumentValidation*/ false);
    ASSERT_EQ(privileges.size(), 1);

    const auto& privilege = privileges.front();
    ASSERT_TRUE(privilege.getResourcePattern().isExactNamespacePattern());
    ASSERT_EQ(privilege.getResourcePattern().ns(), _nss);

    const auto& actions = privilege.getActions();
    ASSERT_EQ(actions.getActionsAsStringDatas().size(), 1);
    ASSERT_TRUE(actions.contains(ActionType::find));

    ASSERT_TRUE(lp.requiresAuthzChecks());
}

TEST_F(DocumentSourceExtensionTest,
       MultipleActionsRequiredPrivilegesAstNodeProducesUnionOfActions) {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(MultipleActionsRequiredPrivilegesAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};

    host::DocumentSourceExtension::LiteParsedExpanded lp(
        std::string(MultipleActionsRequiredPrivilegesAggStageAstNode::kName),
        std::move(handle),
        _nss);

    auto privileges = lp.requiredPrivileges(/*isMongos*/ false, /*bypassDocumentValidation*/ false);
    ASSERT_EQ(privileges.size(), 1);

    const auto& privilege = privileges.front();
    ASSERT_TRUE(privilege.getResourcePattern().isExactNamespacePattern());
    ASSERT_EQ(privilege.getResourcePattern().ns(), _nss);

    const auto& actions = privilege.getActions();
    ASSERT_EQ(actions.getActionsAsStringDatas().size(), 2);
    ASSERT_TRUE(actions.contains(ActionType::listIndexes));
    ASSERT_TRUE(actions.contains(ActionType::planCacheRead));

    ASSERT_TRUE(lp.requiresAuthzChecks());
}

TEST_F(DocumentSourceExtensionTest, MultipleRequiredPrivilegesAstNodeProducesUnionOfPrivileges) {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(MultipleRequiredPrivilegesAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};

    host::DocumentSourceExtension::LiteParsedExpanded lp(
        std::string(MultipleRequiredPrivilegesAggStageAstNode::kName), std::move(handle), _nss);

    auto privileges = lp.requiredPrivileges(/*isMongos*/ false, /*bypassDocumentValidation*/ false);
    ASSERT_EQ(privileges.size(), 1);

    const auto& privilege = privileges.front();
    ASSERT_TRUE(privilege.getResourcePattern().isExactNamespacePattern());
    ASSERT_EQ(privilege.getResourcePattern().ns(), _nss);

    const auto& actions = privilege.getActions();
    ASSERT_EQ(actions.getActionsAsStringDatas().size(), 2);
    ASSERT_TRUE(actions.contains(ActionType::find));
    ASSERT_TRUE(actions.contains(ActionType::indexStats));

    ASSERT_TRUE(lp.requiresAuthzChecks());
}

TEST_F(DocumentSourceExtensionTest, ExpandableToMultipleMixedChildrenUnionsRequiredPrivileges) {
    // Build the parse node that expands to:
    //   1) Host node (e.g., $search via TransformHostParseNode(kSearchSpec)) -> find
    //   2) SingleActionRequiredPrivilegesAggStageAstNode -> find
    //   3) TransformAggStageAstNode -> no privileges
    //   4) MultipleActionsRequiredPrivilegesAggStageAstNode -> find, listIndexes, planCacheRead
    //   6) MultipleChildrenRequiredPrivilegesAggStageParseNode -> find, indexStats
    //   6) NonePosAggStageAstNode -> no privileges
    auto parseNode = new sdk::ExtensionAggStageParseNode(
        MultipleChildrenRequiredPrivilegesAggStageParseNode::make());
    auto handle = AggStageParseNodeHandle{parseNode};
    BSONObj stageBson =
        createDummySpecFromStageName(MultipleChildrenRequiredPrivilegesAggStageParseNode::kName);

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        stageBson.firstElement(), std::move(handle), _nss, LiteParserOptions{});

    auto privileges = lp.requiredPrivileges(/*isMongos*/ false, /*bypassDocumentValidation*/ false);

    ASSERT_EQ(privileges.size(), 1);

    const auto& privilege = privileges.front();
    ASSERT_TRUE(privilege.getResourcePattern().isExactNamespacePattern());
    ASSERT_EQ(privilege.getResourcePattern().ns(), _nss);

    const auto& actions = privilege.getActions();
    ASSERT_EQ(actions.getActionsAsStringDatas().size(), 4);
    ASSERT_TRUE(actions.contains(ActionType::find));
    ASSERT_TRUE(actions.contains(ActionType::listIndexes));
    ASSERT_TRUE(actions.contains(ActionType::planCacheRead));
    ASSERT_TRUE(actions.contains(ActionType::indexStats));

    ASSERT_TRUE(lp.requiresAuthzChecks());
}

DEATH_TEST_F(DocumentSourceExtensionTestDeathTest,
             EmptyActionsArrayRequiredPrivilegesAggStageAstNodeFails,
             "11350600") {
    auto astNode = new sdk::ExtensionAggStageAstNode(
        EmptyActionsArrayRequiredPrivilegesAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};

    host::DocumentSourceExtension::LiteParsedExpanded lp(
        std::string(EmptyActionsArrayRequiredPrivilegesAggStageAstNode::kName),
        std::move(handle),
        _nss);

    [[maybe_unused]] auto privileges =
        lp.requiredPrivileges(/*isMongos*/ false, /*bypassDocumentValidation*/ false);
}

TEST_F(DocumentSourceExtensionTest, ShouldPropagateValidGetNextResultsForTransformExtensionStage) {
    // Create the $documents stage with test data. $documents is a source stage.
    auto docSourcesList = DocumentSourceDocuments::createFromBson(
        BSON("$documents" << BSON_ARRAY(BSON("sourceField" << 1)
                                        << BSON("sourceField" << 2) << BSON("sourceField" << 3)
                                        << BSON("sourceField" << 4) << BSON("sourceField" << 5)))
            .firstElement(),
        getExpCtx());

    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::AddFruitsToDocumentsAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    // Stitch the document sources returned by DocumentSourceDocuments::createFromBson(...) before
    // stitching it with the extension stage.
    std::vector<boost::intrusive_ptr<exec::agg::Stage>> stages;
    for (auto& docSource : docSourcesList) {
        stages.push_back(exec::agg::buildStage(docSource));
    }

    // The document sources are stitched in this order: queue -> project -> unwind -> replaceRoot.
    for (size_t i = 1; i < stages.size(); ++i) {
        stages[i]->setSource(stages[i - 1].get());
    }

    // Tests that exec::agg::ExtensionStage::setSource() correctly overrides
    // exec::agg::Stage::setSource() and sets the source stage for the transform extension stage.
    auto extensionStage = exec::agg::buildStageAndStitch(optimizable, stages.back().get());

    // See sdk::shared_test_stages::TransformExecAggStage for the full expected test document suite.
    std::vector<BSONObj> expectedTransforms = {
        BSON("_id" << 1 << "apples" << "red"),
        BSON("_id" << 2 << "oranges" << 5),
        BSON("_id" << 3 << "bananas" << false),
        BSON("_id" << 4 << "tropical fruits" << BSON_ARRAY("rambutan" << "durian" << "lychee")),
        BSON("_id" << 5 << "pie" << 3.14159)};

    // Verify all documents are transformed correctly.
    for (int i = 1; i <= 5; ++i) {
        auto next = extensionStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        BSONObj docToBson = next.releaseDocument().toBson();

        // Verify transformation added the expected fields.
        ASSERT_TRUE(docToBson.hasField("existingDoc"));
        ASSERT_TRUE(docToBson.hasField("addedFields"));

        // Verify the source document was preserved in "existingDoc".
        ASSERT_BSONOBJ_EQ(docToBson.getObjectField("existingDoc"), BSON("sourceField" << i));

        // Verify the transformed fields match the expected values.
        ASSERT_BSONOBJ_EQ(docToBson.getObjectField("addedFields"), expectedTransforms[i - 1]);
    }

    // Verify that the next result after all the documents have been exhausted has a status of EOF.
    ASSERT_TRUE(extensionStage->getNext().isEOF());
}

TEST_F(
    DocumentSourceExtensionTest,
    ShouldPropagateValidGetNextResultsForTransformExtensionStageWithSourceAsTransformExtensionStage) {
    // Create the $documents stage with test data. $documents is a source stage.
    auto docSourcesList = DocumentSourceDocuments::createFromBson(
        BSON("$documents" << BSON_ARRAY(BSON("sourceField" << 1)
                                        << BSON("sourceField" << 2) << BSON("sourceField" << 3)
                                        << BSON("sourceField" << 4) << BSON("sourceField" << 5)))
            .firstElement(),
        getExpCtx());

    auto firstAstNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::AddFruitsToDocumentsAstNode::make());
    auto firstAstHandle = AggStageAstNodeHandle(firstAstNode);

    auto secondAstNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::AddFruitsToDocumentsAstNode::make());
    auto secondAstHandle = AggStageAstNodeHandle(secondAstNode);

    auto firstOptimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(firstAstHandle));
    auto secondOptimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(secondAstHandle));

    // Stitch the document sources returned by DocumentSourceDocuments::createFromBson(...) before
    // stitching it with the extension stage.
    std::vector<boost::intrusive_ptr<exec::agg::Stage>> stages;
    for (auto& docSource : docSourcesList) {
        stages.push_back(exec::agg::buildStage(docSource));
    }

    // The document sources are stitched in this order: queue -> project -> unwind ->replaceRoot.
    for (size_t i = 1; i < stages.size(); ++i) {
        stages[i]->setSource(stages[i - 1].get());
    }

    // Tests that exec::agg::ExtensionStage::setSource() correctly overrides
    // exec::agg::Stage::setSource() and sets the source stage for the transform extension stage.
    auto firstTransformExtensionStage =
        exec::agg::buildStageAndStitch(firstOptimizable, stages.back().get());

    // Set the source of the second transform extension stage to be the first transform extension
    // stage.
    auto secondTransformExtensionStage =
        exec::agg::buildStageAndStitch(secondOptimizable, firstTransformExtensionStage);

    // See sdk::shared_test_stages::TransformExecAggStage for the full expected test documentsuite.
    std::vector<BSONObj> expectedTransforms = {
        BSON("_id" << 1 << "apples" << "red"),
        BSON("_id" << 2 << "oranges" << 5),
        BSON("_id" << 3 << "bananas" << false),
        BSON("_id" << 4 << "tropical fruits" << BSON_ARRAY("rambutan" << "durian" << "lychee")),
        BSON("_id" << 5 << "pie" << 3.14159)};

    // Verify all documents are transformed correctly.
    for (int i = 1; i <= 5; ++i) {
        auto nextResult = secondTransformExtensionStage->getNext();
        ASSERT_TRUE(nextResult.isAdvanced());
        BSONObj docToBson = nextResult.releaseDocument().toBson();

        // Verify transformation added the expected fields.
        ASSERT_TRUE(docToBson.hasField("existingDoc"));
        ASSERT_TRUE(docToBson.hasField("addedFields"));

        // For $documents -> $transformExtensionStage:
        // Verify the source document was preserved in "existingDoc".
        ASSERT_BSONOBJ_EQ(docToBson.getObjectField("existingDoc").getObjectField("existingDoc"),
                          BSON("sourceField" << i));
        // Verify the transformed fields match the expected values.
        ASSERT_BSONOBJ_EQ(docToBson.getObjectField("existingDoc").getObjectField("addedFields"),
                          expectedTransforms[i - 1]);

        // For $transformExtensionStage -> $transformExtensionStage:
        // Verify the source document was preserved in "existingDoc".
        ASSERT_BSONOBJ_EQ(docToBson.getObjectField("existingDoc"),
                          BSON("existingDoc" << BSON("sourceField" << i) << "addedFields"
                                             << expectedTransforms[i - 1]));
        // Verify the transformed fields match the expected values.
        ASSERT_BSONOBJ_EQ(docToBson.getObjectField("addedFields"), expectedTransforms[i - 1]);
    }
    // Verify that the next result after all the documents have been exhausted has a status of EOF.
    ASSERT_TRUE(secondTransformExtensionStage->getNext().isEOF());
}

TEST_F(DocumentSourceExtensionTest,
       ShouldPropagateValidGetNextResultsForTransformExtensionStageWithSourceExtensionStage) {
    auto sourceAstNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::FruitsAsDocumentsAstNode::make());
    auto sourceAstHandle = AggStageAstNodeHandle(sourceAstNode);

    auto sourceOptimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(sourceAstHandle));

    auto sourceStage = exec::agg::buildStage(sourceOptimizable);

    auto transformAstNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::AddFruitsToDocumentsAstNode::make());
    auto transformAstHandle = AggStageAstNodeHandle(transformAstNode);

    auto transformOptimizable = host::DocumentSourceExtensionOptimizable::create(
        getExpCtx(), std::move(transformAstHandle));

    // Tests that exec::agg::ExtensionStage::setSource() correctly overrides
    // exec::agg::Stage::setSource() and sets the source stage for the transform extension stage.
    auto extensionStage = exec::agg::buildStageAndStitch(transformOptimizable, sourceStage);

    // See sdk::shared_test_stages::TransformExecAggStage for the full expected test document suite.
    std::vector<BSONObj> expectedTransforms = {
        BSON("_id" << 1 << "apples" << "red"),
        BSON("_id" << 2 << "oranges" << 5),
        BSON("_id" << 3 << "bananas" << false),
        BSON("_id" << 4 << "tropical fruits" << BSON_ARRAY("rambutan" << "durian" << "lychee")),
        BSON("_id" << 5 << "pie" << 3.14159)};

    // Verify all documents are transformed correctly.
    for (int i = 1; i <= 5; ++i) {
        auto next = extensionStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        BSONObj docToBson = next.releaseDocument().toBson();

        // Verify transformation added the expected fields.
        ASSERT_TRUE(docToBson.hasField("existingDoc"));
        ASSERT_TRUE(docToBson.hasField("addedFields"));

        // Verify the source document was preserved in "existingDoc".
        ASSERT_BSONOBJ_EQ(docToBson.getObjectField("existingDoc"), expectedTransforms[i - 1]);

        // Verify the transformed fields match the expected values.
        ASSERT_BSONOBJ_EQ(docToBson.getObjectField("addedFields"), expectedTransforms[i - 1]);
    }

    // Verify that the next result after all the documents have been exhausted has a status of EOF.
    ASSERT_TRUE(extensionStage->getNext().isEOF());
}

TEST_F(DocumentSourceExtensionTest, ShouldEofWhenSourceStageEofsEarly) {
    // Create the $documents stage with test data. $documents is a source stage.
    auto docSourcesList = DocumentSourceDocuments::createFromBson(
        BSON("$documents" << BSON_ARRAY(BSON("sourceField" << 1))).firstElement(), getExpCtx());

    auto astNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::AddFruitsToDocumentsAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    // Stitch the document sources returned by DocumentSourceDocuments::createFromBson(...) before
    // stitching it with the extension stage.
    std::vector<boost::intrusive_ptr<exec::agg::Stage>> stages;
    for (auto& docSource : docSourcesList) {
        stages.push_back(exec::agg::buildStage(docSource));
    }

    // The document sources are stitched in this order: queue -> project -> unwind -> replaceRoot.
    for (size_t i = 1; i < stages.size(); ++i) {
        stages[i]->setSource(stages[i - 1].get());
    }

    // Tests that exec::agg::ExtensionStage::setSource() correctly overrides
    // exec::agg::Stage::setSource() and sets the source stage for the transform extension stage.
    auto extensionStage = exec::agg::buildStageAndStitch(optimizable, stages.back().get());

    // See sdk::shared_test_stages::TransformExecAggStage for the full expected test document suite.
    std::vector<BSONObj> expectedTransforms = {BSON("_id" << 1 << "apples" << "red")};

    // Verify all documents are transformed correctly.
    auto next = extensionStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    BSONObj docToBson = next.releaseDocument().toBson();

    // Verify transformation added the expected fields.
    ASSERT_TRUE(docToBson.hasField("existingDoc"));
    ASSERT_TRUE(docToBson.hasField("addedFields"));

    // Verify the source document was preserved in "existingDoc".
    ASSERT_BSONOBJ_EQ(docToBson.getObjectField("existingDoc"), BSON("sourceField" << 1));

    // Verify the transformed fields match the expected values.
    ASSERT_BSONOBJ_EQ(docToBson.getObjectField("addedFields"), expectedTransforms[0]);

    // Exhausted all documents in stream, should return a status of EOF.
    ASSERT_TRUE(extensionStage->getNext().isEOF());

    // Verify that the status is still EOF even when getNext() is called again.
    ASSERT_TRUE(extensionStage->getNext().isEOF());
}

TEST_F(DocumentSourceExtensionTest,
       ShouldPropagateValidGetNextResultsForHostStageTransformStageHostStage) {
    // Create the $documents stage with test data. $documents is a source stage.
    auto docSourcesList = DocumentSourceDocuments::createFromBson(
        BSON("$documents" << BSON_ARRAY(BSON("fruit" << 1)
                                        << BSON("vegetable" << 1) << BSON("fruit" << 1)
                                        << BSON("fruit" << 1) << BSON("vegetable" << 1)))
            .firstElement(),
        getExpCtx());

    auto firstAstNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::AddFruitsToDocumentsAstNode::make());
    auto firstAstHandle = AggStageAstNodeHandle(firstAstNode);

    auto firstOptimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(firstAstHandle));

    boost::intrusive_ptr<DocumentSourceMatch> matchDocSourceStage = DocumentSourceMatch::create(
        BSON("existingDoc" << BSON("fruit" << 1)), make_intrusive<ExpressionContextForTest>());

    // Stitch the document sources returned by DocumentSourceDocuments::createFromBson(...) before
    // stitching it with the extension stage.
    std::vector<boost::intrusive_ptr<exec::agg::Stage>> stages;
    for (auto& docSource : docSourcesList) {
        stages.push_back(exec::agg::buildStage(docSource));
    }

    // The document sources are stitched in this order: queue -> project -> unwind ->replaceRoot.
    for (size_t i = 1; i < stages.size(); ++i) {
        stages[i]->setSource(stages[i - 1].get());
    }

    // Tests that exec::agg::ExtensionStage::setSource() correctly overrides
    // exec::agg::Stage::setSource() and sets the source stage for the transform extension stage.
    auto transformExtensionStage =
        exec::agg::buildStageAndStitch(firstOptimizable, stages.back().get());

    // Set the source of the host match stage to be the transform extension stage.
    auto secondTransformStage =
        exec::agg::buildStageAndStitch(matchDocSourceStage, transformExtensionStage);

    // See sdk::shared_test_stages::TransformExecAggStage for the full expected test documentsuite.
    std::vector<BSONObj> expectedTransforms = {
        BSON("_id" << 1 << "apples" << "red"),
        BSON("_id" << 3 << "bananas" << false),
        BSON("_id" << 4 << "tropical fruits" << BSON_ARRAY("rambutan" << "durian" << "lychee"))};

    // Verify all documents are transformed correctly.
    for (int i = 1; i <= 3; ++i) {
        auto nextResult = secondTransformStage->getNext();
        ASSERT_TRUE(nextResult.isAdvanced());
        BSONObj docToBson = nextResult.releaseDocument().toBson();

        // Verify transformation added the expected fields.
        ASSERT_TRUE(docToBson.hasField("existingDoc"));
        ASSERT_TRUE(docToBson.hasField("addedFields"));

        // Verify the source document was preserved in "existingDoc".
        ASSERT_BSONOBJ_EQ(docToBson.getObjectField("existingDoc"), BSON("fruit" << 1));

        // Verify the transformed fields match the expected values.
        ASSERT_BSONOBJ_EQ(docToBson.getObjectField("addedFields"), expectedTransforms[i - 1]);
    }
    // Verify that the next result after all the documents have been exhausted has a status of EOF.
    ASSERT_TRUE(secondTransformStage->getNext().isEOF());
}

TEST_F(DocumentSourceExtensionTest, ShouldPropagateSourceMetadata) {
    auto sourceAstNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::FruitsAsDocumentsAstNode::make());
    auto sourceAstHandle = AggStageAstNodeHandle(sourceAstNode);

    auto sourceOptimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(sourceAstHandle));
    auto sourceStage = exec::agg::buildStage(sourceOptimizable);

    const auto& inputResults =
        sdk::shared_test_stages::FruitsAsDocumentsExecStage::getInputResults();
    std::vector<Document> expectedDocuments;
    for (const auto& inputResult : inputResults) {
        expectedDocuments.emplace_back(
            Document::createDocumentWithMetadata(inputResult.first, inputResult.second));
    }

    // Verify metadata is present on output documents.
    for (const auto& expectedDocument : expectedDocuments) {
        auto nextResult = sourceStage->getNext();
        ASSERT_TRUE(nextResult.isAdvanced());

        auto actualDoc = nextResult.releaseDocument();
        const auto& actualMetadata = actualDoc.metadata();
        const auto& expectedMetadata = expectedDocument.metadata();

        // Verify documents match.
        ASSERT_DOCUMENT_EQ(actualDoc, expectedDocument);
        // Verify metadata match.
        ASSERT_EQ(actualMetadata, expectedMetadata);
    }
    ASSERT_TRUE(sourceStage->getNext().isEOF());
}

TEST_F(DocumentSourceExtensionTest, TransformReceivesSourceMetadata) {
    auto sourceAstNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::FruitsAsDocumentsAstNode::make());
    auto sourceAstHandle = AggStageAstNodeHandle(sourceAstNode);

    auto sourceOptimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(sourceAstHandle));
    auto sourceStage = exec::agg::buildStage(sourceOptimizable);

    auto transformAstNode = new sdk::ExtensionAggStageAstNode(
        sdk::shared_test_stages::AddFruitsToDocumentsAstNode::make());
    auto transformAstHandle = AggStageAstNodeHandle(transformAstNode);

    auto transformOptimizable = host::DocumentSourceExtensionOptimizable::create(
        getExpCtx(), std::move(transformAstHandle));

    auto transformStage = exec::agg::buildStageAndStitch(transformOptimizable, sourceStage);

    const auto& inputResults =
        sdk::shared_test_stages::FruitsAsDocumentsExecStage::getInputResults();
    std::vector<Document> expectedDocuments;
    for (const auto& inputResult : inputResults) {
        const BSONObj& transformedDocBson =
            BSON("existingDoc" << inputResult.first << "addedFields" << inputResult.first);
        expectedDocuments.emplace_back(
            Document::createDocumentWithMetadata(transformedDocBson, inputResult.second));
    }

    // Verify transform stage output has metadata.
    for (const auto& expectedDocument : expectedDocuments) {
        auto nextResult = transformStage->getNext();
        ASSERT_TRUE(nextResult.isAdvanced());

        auto actualDoc = nextResult.releaseDocument();
        const auto& actualMetadata = actualDoc.metadata();
        const auto& expectedMetadata = expectedDocument.metadata();

        // Verify documents match.
        ASSERT_DOCUMENT_EQ(actualDoc, expectedDocument);
        // Verify metadata match.
        ASSERT_EQ(actualMetadata, expectedMetadata);
    }
    ASSERT_TRUE(transformStage->getNext().isEOF());
}

TEST_F(DocumentSourceExtensionTest, ShouldPropagateSortKeyMetadata) {
    auto sourceAstNode = new sdk::ExtensionAggStageAstNode(ValidateSortKeyMetadataAstStage::make());
    auto sourceAstHandle = AggStageAstNodeHandle(sourceAstNode);

    auto sourceOptimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(sourceAstHandle));
    auto sourceStage = exec::agg::buildStage(sourceOptimizable);
    const auto& inputResults = ValidateSortKeyMetadataExecStage::getInputResults();

    // Verify $sortKey is present on first three documents.
    for (auto index = 0; index < 3; ++index) {
        auto next = sourceStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        const auto& actualDocument = next.releaseDocument();
        const auto& expectedDocument = Document::createDocumentWithMetadata(
            inputResults[index].first, inputResults[index].second);
        ASSERT_DOCUMENT_EQ(actualDocument, expectedDocument);
        ASSERT_EQ(actualDocument.metadata(), expectedDocument.metadata());
    }
    // Verify that an error is thrown if the sort key value is empty.
    {
        ASSERT_THROWS_CODE(sourceStage->getNext(), DBException, 31282);
    }
    // Verify an error is thrown when the sort key has an invalid type (neither an object nor an
    // array).
    {
        ASSERT_THROWS_CODE(sourceStage->getNext(), DBException, 11503701);
    }
    ASSERT_TRUE(sourceStage->getNext().isEOF());
}
}  // namespace mongo::extension
