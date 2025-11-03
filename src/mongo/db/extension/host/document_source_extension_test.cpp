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
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension {

auto nss = NamespaceString::createNamespaceString_forTest("document_source_extension_test"_sd);

class DocumentSourceExtensionTest : public AggregationContextFixture {
public:
    DocumentSourceExtensionTest() : DocumentSourceExtensionTest(nss) {}
    explicit DocumentSourceExtensionTest(NamespaceString nsString)
        : AggregationContextFixture(std::move(nsString)) {};

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

TEST_F(DocumentSourceExtensionTest, parseNoOpSuccess) {
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
    auto serializedPipeline = parsedPipeline->serializeToBson();
    ASSERT_EQUALS(serializedPipeline.size(), 1u);
    ASSERT_BSONOBJ_EQ(serializedPipeline[0],
                      BSON(sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName
                           << "serializedForExecution"));

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
    AggStageParseNodeHandle rootHandle{rootParseNode};
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
    AggStageParseNodeHandle rootHandle{rootParseNode};
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
    AggStageParseNodeHandle rootHandle{rootParseNode};

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
    AggStageParseNodeHandle rootHandle{rootParseNode};
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
    AggStageParseNodeHandle rootHandle{rootParseNode};
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
    AggStageParseNodeHandle rootHandle{rootParseNode};

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
    AggStageParseNodeHandle rootHandle{rootParseNode};
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

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> out;
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

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> out;
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

    std::vector<sdk::VariantNode> expand() const override;

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

    std::vector<sdk::VariantNode> expand() const override;

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return {};
    }
};

std::vector<sdk::VariantNode> NodeAParseNode::expand() const {
    std::vector<sdk::VariantNode> out;
    out.reserve(1);
    out.emplace_back(new sdk::ExtensionAggStageParseNode(std::make_unique<NodeBParseNode>()));
    return out;
}

std::vector<sdk::VariantNode> NodeBParseNode::expand() const {
    std::vector<sdk::VariantNode> out;
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

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> out;
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

DEATH_TEST_F(DocumentSourceExtensionTest, FindStageIdThrowsForUnknownStage, "11250700") {
    [[maybe_unused]] auto stageId = host::DocumentSourceExtension::findStageId("$unknownStage");
}

TEST_F(DocumentSourceExtensionTest, FindStageIdMatchesRegisteredStage) {
    std::unique_ptr<host::HostPortal> hostPortal = std::make_unique<host::HostPortal>();
    host_connector::HostPortalAdapter portal{
        MONGODB_EXTENSION_API_VERSION, 1, "", std::move(hostPortal)};
    portal.getImpl().registerStageDescriptor(&_noOpStaticDescriptor);

    // Build a pipeline that contains the registered stage.
    std::vector<BSONObj> testPipeline{kValidSpec};
    auto pipeline = buildTestPipeline(testPipeline);
    ASSERT(pipeline);

    const auto* stagePtr = pipeline->peekFront();
    ASSERT_TRUE(stagePtr != nullptr);

    const auto* documentSourceExtension =
        dynamic_cast<const host::DocumentSourceExtension*>(stagePtr);
    ASSERT_TRUE(documentSourceExtension != nullptr);

    // Compare the id returned by the constructed stage against the id resolved via findStageId().
    auto stageId = documentSourceExtension->getId();
    auto mapId = host::DocumentSourceExtension::findStageId(
        sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName);
    ASSERT_EQUALS(stageId, mapId);
}

namespace {
static constexpr std::string_view kNoOp2Name = "$noOp2";

class NoOp2AggStageAstNode : public sdk::AggStageAstNode {
public:
    NoOp2AggStageAstNode() : sdk::AggStageAstNode(kNoOp2Name) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<sdk::shared_test_stages::NoOpLogicalAggStage>();
    }
};

class NoOp2AggStageParseNode : public sdk::AggStageParseNode {
public:
    NoOp2AggStageParseNode() : sdk::AggStageParseNode(kNoOp2Name) {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(
            new sdk::ExtensionAggStageAstNode(std::make_unique<NoOp2AggStageAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return BSONObj();
    }
};

class NoOp2AggStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kNoOp2Name);

    NoOp2AggStageDescriptor()
        : sdk::AggStageDescriptor(kStageName, MongoExtensionAggStageType::kNoOp) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        return std::make_unique<NoOp2AggStageParseNode>();
    }
};
}  // namespace

TEST_F(DocumentSourceExtensionTest, FindStageIdsMatchRegisteredStagesMultiple) {
    // Register two different NoOp descriptors.
    std::unique_ptr<host::HostPortal> hostPortal = std::make_unique<host::HostPortal>();
    host_connector::HostPortalAdapter portal{
        MONGODB_EXTENSION_API_VERSION, 1, "", std::move(hostPortal)};
    portal.getImpl().registerStageDescriptor(&_noOpStaticDescriptor);

    sdk::ExtensionAggStageDescriptor noOp2StaticDescriptor{
        std::make_unique<NoOp2AggStageDescriptor>()};
    portal.getImpl().registerStageDescriptor(&noOp2StaticDescriptor);

    // Build a pipeline with both stages.
    BSONObj secondValidSpec = BSON(NoOp2AggStageDescriptor::kStageName << BSON("bar" << true));
    std::vector<BSONObj> testPipeline{kValidSpec, secondValidSpec};
    auto pipeline = buildTestPipeline(testPipeline);
    ASSERT(pipeline);

    ASSERT_EQUALS(pipeline->getSources().size(), 2U);

    // First stage should match $noOp id.
    auto firstIt = pipeline->getSources().begin();
    const auto* firstStage = dynamic_cast<const host::DocumentSourceExtension*>(firstIt->get());
    ASSERT_TRUE(firstStage != nullptr);
    auto firstStageId = firstStage->getId();
    auto firstMapId = host::DocumentSourceExtension::findStageId(
        sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName);
    ASSERT_EQUALS(firstStageId, firstMapId);

    // Second stage should match $noOp2 id.
    auto secondIt = std::next(firstIt);
    const auto* secondStage = dynamic_cast<const host::DocumentSourceExtension*>(secondIt->get());
    ASSERT_TRUE(secondStage != nullptr);
    auto secondStageId = secondStage->getId();
    auto secondMapId =
        host::DocumentSourceExtension::findStageId(NoOp2AggStageDescriptor::kStageName);
    ASSERT_EQUALS(secondStageId, secondMapId);
}

}  // namespace mongo::extension
