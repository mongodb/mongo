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
#include "mongo/db/extension/host_connector/host_services_adapter.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/pipeline.h"
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
        extension::sdk::HostServicesHandle::setHostServices(
            extension::host_connector::HostServicesAdapter::get());
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
        host::DocumentSourceExtension::unregisterParser_forTest("$noOp2");
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

TEST_F(DocumentSourceExtensionTest, ParseNoOpSuccess) {
    // Try to parse pipeline with custom extension stage before registering the extension,
    // should fail.
    std::vector<BSONObj> testPipeline{kValidSpec};
    ASSERT_THROWS_CODE(buildTestPipeline(testPipeline), AssertionException, 16436);
    // Register the extension stage and try to reparse.

    std::unique_ptr<host::HostPortal> hostPortal = std::make_unique<host::HostPortal>();
    host_connector::HostPortalAdapter portal{
        MONGODB_EXTENSION_API_VERSION, 1, "", std::move(hostPortal)};
    portal.getImpl().registerStageDescriptor(&_noOpStaticDescriptor);

    auto parsedPipeline = buildTestPipeline(testPipeline);
    ASSERT(parsedPipeline);

    ASSERT_EQUALS(parsedPipeline->size(), 1u);
    const auto* stagePtr = parsedPipeline->peekFront();
    ASSERT_TRUE(stagePtr != nullptr);
    ASSERT_EQUALS(std::string(stagePtr->getSourceName()),
                  sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName);
    auto serializedPipeline =
        parsedPipeline->serializeToBson(SerializationOptions::kDebugQueryShapeSerializeOptions);
    ASSERT_EQUALS(serializedPipeline.size(), 1u);
    // The extension is in the form of DocumentSourceExtensionExpandable at this point, which
    // serializes to its query shape. There is no query shape for the no-op extension.
    ASSERT_BSONOBJ_EQ(serializedPipeline[0], BSONObj());
}

TEST_F(DocumentSourceExtensionTest, ExpandToExtAst) {
    auto rootParseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::ExpandToExtAstParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(sdk::shared_test_stages::kExpandToExtAstName),
        std::move(rootHandle),
        _nss,
        LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 1);

    // Expanded pipeline contains LiteParsedExpanded.
    auto* first =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded.front().get());
    ASSERT_TRUE(first != nullptr);
    ASSERT_EQ(first->getParseTimeName(), std::string(sdk::shared_test_stages::kNoOpName));
}

TEST_F(DocumentSourceExtensionTest, ExpandToExtParse) {
    auto rootParseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::ExpandToExtParseParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(sdk::shared_test_stages::kExpandToExtParseName),
        std::move(rootHandle),
        _nss,
        LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 1);

    // Expanded pipeline contains LiteParsedExpanded.
    auto* first =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded.front().get());
    ASSERT_TRUE(first != nullptr);
    ASSERT_EQ(first->getParseTimeName(), std::string(sdk::shared_test_stages::kNoOpName));
}

TEST_F(DocumentSourceExtensionTest, ExpandToHostParse) {
    auto rootParseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::ExpandToHostParseParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(sdk::shared_test_stages::kExpandToHostParseName),
        std::move(rootHandle),
        _nss,
        LiteParserOptions{});

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
    auto rootParseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::ExpandToMixedParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(sdk::shared_test_stages::kExpandToMixedName),
        std::move(rootHandle),
        _nss,
        LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 4);

    const auto it0 = expanded.begin();
    const auto it1 = std::next(expanded.begin(), 1);
    const auto it2 = std::next(expanded.begin(), 2);
    const auto it3 = std::next(expanded.begin(), 3);

    auto* first = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it0->get());
    ASSERT_TRUE(first != nullptr);

    auto* second = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it1->get());
    ASSERT_TRUE(second != nullptr);

    auto* third = dynamic_cast<LiteParsedDocumentSource*>(it2->get());
    ASSERT_TRUE(third != nullptr);
    auto* notExpanded =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it2->get());
    ASSERT_TRUE(notExpanded == nullptr);

    auto* fourth = dynamic_cast<LiteParsedDocumentSource*>(it3->get());
    ASSERT_TRUE(fourth != nullptr);

    ASSERT_EQ(first->getParseTimeName(), std::string(sdk::shared_test_stages::kNoOpName));
    ASSERT_EQ(second->getParseTimeName(), std::string(sdk::shared_test_stages::kNoOpName));
    ASSERT_EQ(third->getParseTimeName(), std::string(DocumentSourceMatch::kStageName));
    ASSERT_EQ(fourth->getParseTimeName(),
              std::string(DocumentSourceInternalSearchIdLookUp::kStageName));
}

TEST_F(DocumentSourceExtensionTest, ExpandedPipelineIsComputedOnce) {
    sdk::shared_test_stages::ExpandToExtParseParseNode::expandCalls = 0;

    auto rootParseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::ExpandToExtParseParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(sdk::shared_test_stages::kExpandToExtParseName),
        std::move(rootHandle),
        _nss,
        LiteParserOptions{});

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
            sdk::shared_test_stages::NoOpHostParseNode::make(kBadQuerySettingsSpec)));
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

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(sdk::shared_test_stages::kExpandToHostAstName),
        std::move(rootHandle),
        _nss,
        LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 1);

    // Expanded pipeline contains LiteParsedDocumentSource.
    auto* lpds = dynamic_cast<LiteParsedDocumentSource*>(expanded.front().get());
    ASSERT_TRUE(lpds != nullptr);
    ASSERT_EQ(lpds->getParseTimeName(),
              std::string(DocumentSourceInternalSearchIdLookUp::kStageName));
}

TEST_F(DocumentSourceExtensionTest, ExpandPropagatesHostLiteParseFailure) {
    auto* rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<ExpandToHostParseBadSpecParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};

    ASSERT_THROWS_CODE(
        [&] {
            host::DocumentSourceExtension::LiteParsedExpandable lp(
                std::string(kExpandToHostBadSpecName), std::move(rootHandle), _nss, {});
        }(),
        AssertionException,
        7746800);
}

TEST_F(DocumentSourceExtensionTest, ExpandRecursesMultipleLevels) {
    auto rootParseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::TopParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};
    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(sdk::shared_test_stages::kTopName),
        std::move(rootHandle),
        _nss,
        LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 4);

    const auto it0 = expanded.begin();
    const auto it1 = std::next(expanded.begin(), 1);
    const auto it2 = std::next(expanded.begin(), 2);
    const auto it3 = std::next(expanded.begin(), 3);

    auto* first = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it0->get());
    ASSERT_TRUE(first != nullptr);
    ASSERT_EQ(first->getParseTimeName(), std::string(sdk::shared_test_stages::kLeafAName));

    auto* second = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it1->get());
    ASSERT_TRUE(second != nullptr);
    ASSERT_EQ(second->getParseTimeName(), std::string(sdk::shared_test_stages::kLeafBName));

    auto* third = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it2->get());
    ASSERT_TRUE(third != nullptr);
    ASSERT_EQ(third->getParseTimeName(), std::string(sdk::shared_test_stages::kLeafCName));

    auto* fourth = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it3->get());
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
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
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
            std::make_unique<sdk::shared_test_stages::NoOpAggStageParseNode>()));
        out.emplace_back(new sdk::ExtensionAggStageParseNode(
            std::make_unique<sdk::shared_test_stages::NoOpAggStageParseNode>()));
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

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(makeRecursiveDepthName(depth)),
        std::move(rootHandle),
        _nss,
        LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    // Final expansion produces exactly one AST leaf.
    ASSERT_EQUALS(expanded.size(), 1);
    auto* leaf =
        dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(expanded.front().get());
    ASSERT_TRUE(leaf != nullptr);
    ASSERT_EQ(leaf->getParseTimeName(), std::string(kDepthLeafName));
}

DEATH_TEST_F(DocumentSourceExtensionTest, ExpandExceedsMaxDepthFails, "10955800") {
    auto depth = host::DocumentSourceExtension::LiteParsedExpandable::kMaxExpansionDepth + 1;
    auto* rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<DepthChainParseNode>(depth));
    AggStageParseNodeHandle rootHandle{rootParseNode};

    [[maybe_unused]] host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(makeRecursiveDepthName(depth)),
        std::move(rootHandle),
        _nss,
        LiteParserOptions{});
}

TEST_F(DocumentSourceExtensionTest, ExpandAdjacentCycleFails) {
    auto* rootParseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<AdjacentCycleParseNode>());
    AggStageParseNodeHandle rootHandle{rootParseNode};

    ASSERT_THROWS_WITH_CHECK(
        [&] {
            [[maybe_unused]] host::DocumentSourceExtension::LiteParsedExpandable lp(
                std::string(kAdjCycleName), std::move(rootHandle), _nss, LiteParserOptions{});
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

    ASSERT_THROWS_WITH_CHECK(
        [&] {
            [[maybe_unused]] host::DocumentSourceExtension::LiteParsedExpandable lp(
                std::string(kNodeAName), std::move(rootHandle), _nss, LiteParserOptions{});
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

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(kTopSameNameChildren), std::move(rootHandle), _nss, LiteParserOptions{});

    const auto& expanded = lp.getExpandedPipeline();
    ASSERT_EQUALS(expanded.size(), 2);

    auto it0 = expanded.begin();
    auto it1 = std::next(expanded.begin(), 1);

    auto* first = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it0->get());
    auto* second = dynamic_cast<host::DocumentSourceExtension::LiteParsedExpanded*>(it1->get());
    ASSERT_TRUE(first != nullptr);
    ASSERT_TRUE(second != nullptr);

    // Both leaves are the NoOp leaf from NoOpAggStageParseNode.
    ASSERT_EQ(first->getParseTimeName(), std::string(sdk::shared_test_stages::kNoOpName));
    ASSERT_EQ(second->getParseTimeName(), std::string(sdk::shared_test_stages::kNoOpName));
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
            sdk::shared_test_stages::NoOpHostParseNode::make(kSearchSpec)));
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
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
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
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
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
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
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
            sdk::shared_test_stages::NoOpHostParseNode::make(kSearchSpec)));
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<SingleActionRequiredPrivilegesAggStageAstNode>()));
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<sdk::shared_test_stages::NoOpAggStageAstNode>()));
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
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
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
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<EmptyRequiredPrivilegesAggStageAstNode>();
    }
};
}  // namespace

TEST_F(DocumentSourceExtensionTest, NoOpAstNodeWithDefaultGetPropertiesSucceeds) {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(sdk::shared_test_stages::NoOpAggStageAstNode::make());
    auto handle = AggStageAstNodeHandle{astNode};

    host::DocumentSourceExtension::LiteParsedExpanded lp(
        std::string(sdk::shared_test_stages::kNoOpName), std::move(handle), _nss);
    ASSERT_FALSE(lp.isInitialSource());
    ASSERT_FALSE(lp.requiresAuthzChecks());
    ASSERT_EQUALS(lp.requiredPrivileges(/*isMongos*/ false, /*bypassDocumentValidation*/ false),
                  PrivilegeVector{});
}

TEST_F(DocumentSourceExtensionTest, NoOpParseNodeInheritsDefaultGetPropertiesFromAstNode) {
    auto parseNode =
        new sdk::ExtensionAggStageParseNode(sdk::shared_test_stages::NoOpAggStageParseNode::make());
    auto handle = AggStageParseNodeHandle{parseNode};

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(sdk::shared_test_stages::kNoOpName),
        std::move(handle),
        _nss,
        LiteParserOptions{});
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

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(sdk::shared_test_stages::kTransformName),
        std::move(handle),
        _nss,
        LiteParserOptions{});
    ASSERT_FALSE(lp.isInitialSource());
}

TEST_F(DocumentSourceExtensionTest,
       SearchLikeSourceAggStageParseNodeInheritsInitialSourceFromFirstAstNode) {
    auto parseNode = new sdk::ExtensionAggStageParseNode(SearchLikeSourceAggStageParseNode::make());
    auto handle = AggStageParseNodeHandle{parseNode};

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(sdk::shared_test_stages::kSearchLikeSourceStageName),
        std::move(handle),
        _nss,
        LiteParserOptions{});
    ASSERT_TRUE(lp.isInitialSource());
}

TEST_F(DocumentSourceExtensionTest, ExpandToMatchParseNodeInheritsPropertiesFromMatch) {
    auto parseNode = new sdk::ExtensionAggStageParseNode(
        std::make_unique<sdk::shared_test_stages::ExpandToHostParseParseNode>());
    auto handle = AggStageParseNodeHandle{parseNode};

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(sdk::shared_test_stages::kExpandToHostParseName),
        std::move(handle),
        _nss,
        LiteParserOptions{});
    ASSERT_FALSE(lp.isInitialSource());
    ASSERT_FALSE(lp.requiresAuthzChecks());
    ASSERT_EQUALS(lp.requiredPrivileges(/*isMongos*/ false, /*bypassDocumentValidation*/ false),
                  PrivilegeVector{});
}

TEST_F(DocumentSourceExtensionTest, ExpandToSearchParseNodeInheritsPropertiesFromSearch) {
    auto parseNode =
        new sdk::ExtensionAggStageParseNode(std::make_unique<ExpandToSearchAggStageParseNode>());
    auto handle = AggStageParseNodeHandle{parseNode};

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(kExpandToSearchName), std::move(handle), _nss, LiteParserOptions{});
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
    //   1) Host node (e.g., $search via NoOpHostParseNode(kSearchSpec)) -> find
    //   2) SingleActionRequiredPrivilegesAggStageAstNode -> find
    //   3) NoOpAggStageAstNode -> no privileges
    //   4) MultipleActionsRequiredPrivilegesAggStageAstNode -> find, listIndexes, planCacheRead
    //   6) MultipleChildrenRequiredPrivilegesAggStageParseNode -> find, indexStats
    //   6) NonePosAggStageAstNode -> no privileges
    auto parseNode = new sdk::ExtensionAggStageParseNode(
        MultipleChildrenRequiredPrivilegesAggStageParseNode::make());
    auto handle = AggStageParseNodeHandle{parseNode};

    host::DocumentSourceExtension::LiteParsedExpandable lp(
        std::string(MultipleChildrenRequiredPrivilegesAggStageParseNode::kName),
        std::move(handle),
        _nss,
        LiteParserOptions{});

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

DEATH_TEST_F(DocumentSourceExtensionTest,
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

TEST_F(DocumentSourceExtensionTest, ShouldPropagateValidGetNextResultsForSourceExtensionStage) {
    auto astNode =
        new sdk::ExtensionAggStageAstNode(sdk::shared_test_stages::SourceAggStageAstNode::make());
    auto astHandle = AggStageAstNodeHandle(astNode);

    auto optimizable =
        host::DocumentSourceExtensionOptimizable::create(getExpCtx(), std::move(astHandle));

    auto extensionStage = exec::agg::buildStage(optimizable);

    // See sdk::shared_test_stages::SourceExecAggStage for the full test document suite.
    {
        auto next = extensionStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                           (Document{BSON("_id" << 1 << "apples" << "red")}));
    }
    {
        auto next = extensionStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{BSON("_id" << 2 << "oranges" << 5)}));
    }
    {
        auto next = extensionStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                           (Document{BSON("_id" << 3 << "bananas" << false)}));
    }
    {
        auto next = extensionStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(
            next.releaseDocument(),
            (Document{BSON("_id" << 4 << "tropical fruits"
                                 << BSON_ARRAY("rambutan" << "durian" << "lychee"))}));
    }
    {
        auto next = extensionStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                           (Document{BSON("_id" << 5 << "pie" << 3.14159)}));
    }
}

}  // namespace mongo::extension
