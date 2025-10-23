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

#include "mongo/db/extension/sdk/aggregation_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/host_connector/executable_agg_stage.h"
#include "mongo/db/extension/host_connector/host_services_adapter.h"
#include "mongo/db/extension/host_connector/query_shape_opts_adapter.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/query_shape_opts_handle.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::extension::sdk {

namespace {

// Helpers
inline StringData stringViewToStringData(std::string_view sv) {
    return StringData{sv.data(), sv.size()};
}

template <class Variant>
const extension::AggStageAstNodeHandle& asAst(const Variant& v) {
    ASSERT_TRUE(std::holds_alternative<extension::AggStageAstNodeHandle>(v));
    return std::get<extension::AggStageAstNodeHandle>(v);
}

template <class Variant>
const extension::AggStageParseNodeHandle& asParse(const Variant& v) {
    ASSERT_TRUE(std::holds_alternative<extension::AggStageParseNodeHandle>(v));
    return std::get<extension::AggStageParseNodeHandle>(v);
}

static constexpr std::string_view kDesugarToEmptyName = "$desugarToEmpty";
static constexpr std::string_view kCountingName = "$counting";
static constexpr std::string_view kNestedDesugaringName = "$nestedDesugaring";
static constexpr std::string_view kGetExpandedSizeLessName =
    "$getExpandedSizeLessThanActualExpansionSize";
static constexpr std::string_view kGetExpandedSizeGreaterName =
    "$getExpandedSizeGreaterThanActualExpansionSize";

class AggStageTest : public unittest::Test {
public:
    void setUp() override {
        // Initialize HostServices so that aggregation stages will be able to access member
        // functions, e.g. to run assertions.
        extension::sdk::HostServicesHandle::setHostServices(
            extension::host_connector::HostServicesAdapter::get());
    }
};

class DesugarToEmptyParseNode : public sdk::AggStageParseNode {
public:
    DesugarToEmptyParseNode() : sdk::AggStageParseNode(kDesugarToEmptyName) {}

    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<sdk::VariantNode> expand() const override {
        return {};
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<DesugarToEmptyParseNode>();
    }
};

class CountingAst final : public sdk::AggStageAstNode {
public:
    static int alive;

    CountingAst() : sdk::AggStageAstNode(kCountingName) {
        ++alive;
    }

    ~CountingAst() override {
        --alive;
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<shared_test_stages::NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<CountingAst>();
    }
};

class CountingParse final : public sdk::AggStageParseNode {
public:
    static constexpr size_t kExpansionSize = 1;
    static int alive;

    CountingParse() : sdk::AggStageParseNode(kCountingName) {
        ++alive;
    }

    ~CountingParse() override {
        --alive;
    }

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(CountingAst::make()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<CountingParse>();
    }
};

inline int CountingParse::alive = 0;
inline int CountingAst::alive = 0;

class NestedDesugaringParseNode final : public sdk::AggStageParseNode {
public:
    NestedDesugaringParseNode() : sdk::AggStageParseNode(kNestedDesugaringName) {}

    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(CountingAst::make()));
        expanded.emplace_back(new sdk::ExtensionAggStageParseNode(CountingParse::make()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<NestedDesugaringParseNode>();
    }
};

class GetExpandedSizeLessThanActualExpansionSizeParseNode final : public sdk::AggStageParseNode {
public:
    GetExpandedSizeLessThanActualExpansionSizeParseNode()
        : sdk::AggStageParseNode(kGetExpandedSizeLessName) {}

    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize - 1;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(CountingAst::make()));
        expanded.emplace_back(new sdk::ExtensionAggStageParseNode(CountingParse::make()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<GetExpandedSizeLessThanActualExpansionSizeParseNode>();
    }
};

class GetExpandedSizeGreaterThanActualExpansionSizeParseNode final : public sdk::AggStageParseNode {
public:
    GetExpandedSizeGreaterThanActualExpansionSizeParseNode()
        : sdk::AggStageParseNode(kGetExpandedSizeGreaterName) {}

    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize + 1;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(CountingAst::make()));
        expanded.emplace_back(new sdk::ExtensionAggStageParseNode(CountingParse::make()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<GetExpandedSizeGreaterThanActualExpansionSizeParseNode>();
    }
};

class NameMismatchParseNode : public sdk::AggStageParseNode {
public:
    NameMismatchParseNode() : sdk::AggStageParseNode("$nameB") {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(
            new sdk::ExtensionAggStageAstNode(shared_test_stages::NoOpAggStageAstNode::make()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<NameMismatchParseNode>();
    }
};

class NameMismatchStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string("$nameA");

    NameMismatchStageDescriptor()
        : sdk::AggStageDescriptor(kStageName, MongoExtensionAggStageType::kDesugar) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        return std::make_unique<NameMismatchParseNode>();
    }

    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<NameMismatchStageDescriptor>();
    }
};

class ParseNodeVTableTest : public unittest::Test {
public:
    // This special handle class is only used within this fixture so that we can unit test the
    // assertVTableConstraints functionality of the handle.
    class TestParseNodeVTableHandle : public extension::AggStageParseNodeHandle {
    public:
        TestParseNodeVTableHandle(absl::Nonnull<::MongoExtensionAggStageParseNode*> parseNode)
            : extension::AggStageParseNodeHandle(parseNode) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

class AstNodeVTableTest : public unittest::Test {
public:
    class TestAstNodeVTableHandle : public extension::AggStageAstNodeHandle {
    public:
        TestAstNodeVTableHandle(absl::Nonnull<::MongoExtensionAggStageAstNode*> astNode)
            : extension::AggStageAstNodeHandle(astNode) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

TEST_F(AggStageTest, CountingParseExpansionSucceedsTest) {
    auto countingParseNode = new ExtensionAggStageParseNode(CountingParse::make());
    auto handle = extension::AggStageParseNodeHandle{countingParseNode};

    auto expanded = handle.expand();
    ASSERT_EQUALS(expanded.size(), 1);

    const auto& astHandle = asAst(expanded[0]);
    ASSERT_EQ(astHandle.getName(), stringViewToStringData(kCountingName));
}

TEST_F(AggStageTest, NestedExpansionSucceedsTest) {
    auto nestedDesugarParseNode = new ExtensionAggStageParseNode(NestedDesugaringParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{nestedDesugarParseNode};

    auto expanded = handle.expand();
    ASSERT_EQUALS(expanded.size(), 2);

    // First element is the AstNode.
    const auto& firstAstHandle = asAst(expanded[0]);
    ASSERT_EQ(firstAstHandle.getName(), stringViewToStringData(kCountingName));

    // Second element is the nested ParseNode.
    const auto& nestedParseHandle = asParse(expanded[1]);
    ASSERT_EQ(nestedParseHandle.getName(), stringViewToStringData(kCountingName));

    // Expand the nested node.
    auto nestedExpanded = nestedParseHandle.expand();
    ASSERT_EQUALS(nestedExpanded.size(), 1);

    const auto& nestedAstHandle = asAst(nestedExpanded[0]);
    ASSERT_EQ(nestedAstHandle.getName(), stringViewToStringData(kCountingName));
}

TEST_F(AggStageTest, HandlesPreventMemoryLeaksOnSuccess) {
    CountingAst::alive = 0;
    CountingParse::alive = 0;

    auto nestedDesugarParseNode = new ExtensionAggStageParseNode(NestedDesugaringParseNode::make());

    {
        auto handle = extension::AggStageParseNodeHandle{nestedDesugarParseNode};

        [[maybe_unused]] auto expanded = handle.expand();
        ASSERT_EQUALS(CountingAst::alive, 1);
        ASSERT_EQUALS(CountingParse::alive, 1);
    }

    // Assert that the result of expand(), a vector of VariantNodeHandles, is properly cleaned up
    // once it goes out of scope.
    ASSERT_EQUALS(CountingAst::alive, 0);
    ASSERT_EQUALS(CountingParse::alive, 0);
}

TEST_F(AggStageTest, HandlesPreventMemoryLeaksOnFailure) {
    CountingAst::alive = 0;
    CountingParse::alive = 0;

    auto nestedDesugarParseNode = new ExtensionAggStageParseNode(NestedDesugaringParseNode::make());

    auto handle = extension::AggStageParseNodeHandle{nestedDesugarParseNode};

    auto failExpand = globalFailPointRegistry().find("failExtensionExpand");
    failExpand->setMode(FailPoint::skip, 1);

    ASSERT_THROWS_CODE(
        [&] {
            [[maybe_unused]] auto expanded = handle.expand();
        }(),
        DBException,
        11113805);

    // Assert that the result of expand(), a vector of VariantNodeHandles, is properly cleaned up
    // after an exception is thrown.
    ASSERT_EQUALS(CountingAst::alive, 0);
    ASSERT_EQUALS(CountingParse::alive, 0);

    failExpand->setMode(FailPoint::off, 0);
}

TEST_F(AggStageTest, ExtExpandPreventsMemoryLeaksOnFailure) {
    CountingAst::alive = 0;
    CountingParse::alive = 0;

    auto nestedDesugarParseNode = new ExtensionAggStageParseNode(NestedDesugaringParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{nestedDesugarParseNode};

    auto failExpand = globalFailPointRegistry().find("failVariantNodeConversion");
    failExpand->setMode(FailPoint::skip, 1);

    ASSERT_THROWS_CODE(
        [&] {
            [[maybe_unused]] auto expanded = handle.expand();
        }(),
        DBException,
        11197200);

    ASSERT_EQUALS(CountingAst::alive, 0);
    ASSERT_EQUALS(CountingParse::alive, 0);

    failExpand->setMode(FailPoint::off, 0);
}

DEATH_TEST_F(AggStageTest, EmptyDesugarExpansionFails, "11113803") {
    auto emptyDesugarParseNode = new ExtensionAggStageParseNode(DesugarToEmptyParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{emptyDesugarParseNode};

    [[maybe_unused]] auto expanded = handle.expand();
}

DEATH_TEST_F(AggStageTest, GetExpandedSizeLessThanActualExpansionSizeFails, "11113802") {
    auto getExpandedSizeLessThanActualExpansionSizeParseNode =
        new ExtensionAggStageParseNode(GetExpandedSizeLessThanActualExpansionSizeParseNode::make());
    auto handle =
        extension::AggStageParseNodeHandle{getExpandedSizeLessThanActualExpansionSizeParseNode};

    [[maybe_unused]] auto expanded = handle.expand();
}

DEATH_TEST_F(AggStageTest, GetExpandedSizeGreaterThanActualExpansionSizeFails, "11113802") {
    auto getExpandedSizeGreaterThanActualExpansionSizeParseNode = new ExtensionAggStageParseNode(
        GetExpandedSizeGreaterThanActualExpansionSizeParseNode::make());
    auto handle =
        extension::AggStageParseNodeHandle{getExpandedSizeGreaterThanActualExpansionSizeParseNode};

    [[maybe_unused]] auto expanded = handle.expand();
}

DEATH_TEST_F(AggStageTest, DescriptorAndParseNodeNameMismatchFails, "11217602") {
    auto descriptor =
        std::make_unique<ExtensionAggStageDescriptor>(NameMismatchStageDescriptor::make());
    auto handle = extension::AggStageDescriptorHandle{descriptor.get()};

    BSONObj stageBson = BSON(NameMismatchStageDescriptor::kStageName << BSONObj());
    [[maybe_unused]] auto parseNodeHandle = handle.parse(stageBson);
}

DEATH_TEST_F(ParseNodeVTableTest, InvalidParseNodeVTableFailsGetName, "11217600") {
    auto noOpParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::NoOpAggStageParseNode::make());
    auto handle = TestParseNodeVTableHandle{noOpParseNode};

    auto vtable = handle.vtable();
    vtable.get_name = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ParseNodeVTableTest, InvalidParseNodeVTableFailsGetQueryShape, "10977600") {
    auto noOpParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::NoOpAggStageParseNode::make());
    auto handle = TestParseNodeVTableHandle{noOpParseNode};

    auto vtable = handle.vtable();
    vtable.get_query_shape = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ParseNodeVTableTest, InvalidParseNodeVTableFailsGetExpandedSize, "11113800") {
    auto noOpParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::NoOpAggStageParseNode::make());
    auto handle = TestParseNodeVTableHandle{noOpParseNode};

    auto vtable = handle.vtable();
    vtable.get_expanded_size = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ParseNodeVTableTest, InvalidParseNodeVTableFailsExpand, "10977601") {
    auto noOpParseNode =
        new ExtensionAggStageParseNode(shared_test_stages::NoOpAggStageParseNode::make());
    auto handle = TestParseNodeVTableHandle{noOpParseNode};

    auto vtable = handle.vtable();
    vtable.expand = nullptr;
    handle.assertVTableConstraints(vtable);
};

TEST_F(AggStageTest, NoOpAstNodeTest) {
    auto noOpAggStageAstNode =
        new ExtensionAggStageAstNode(shared_test_stages::NoOpAggStageAstNode::make());
    auto handle = extension::AggStageAstNodeHandle{noOpAggStageAstNode};

    [[maybe_unused]] auto logicalStageHandle = handle.bind();
}

DEATH_TEST_F(AstNodeVTableTest, InvalidAstNodeVTableFailsGetName, "11217601") {
    auto noOpAstNode =
        new ExtensionAggStageAstNode(shared_test_stages::NoOpAggStageAstNode::make());
    auto handle = TestAstNodeVTableHandle{noOpAstNode};

    auto vtable = handle.vtable();
    vtable.get_name = nullptr;
    handle.assertVTableConstraints(vtable);
}

DEATH_TEST_F(AstNodeVTableTest, InvalidAstNodeVTableBind, "11113700") {
    auto noOpAstNode =
        new ExtensionAggStageAstNode(shared_test_stages::NoOpAggStageAstNode::make());
    auto handle = TestAstNodeVTableHandle{noOpAstNode};

    auto vtable = handle.vtable();
    vtable.bind = nullptr;
    handle.assertVTableConstraints(vtable);
}

class SimpleSerializationLogicalStage : public LogicalAggStage {
public:
    static constexpr StringData kStageName = "$simpleSerialization";
    static constexpr StringData kStageSpec = "mongodb";

    SimpleSerializationLogicalStage() {}

    BSONObj serialize() const override {
        return BSON(kStageName << kStageSpec);
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON(kStageName << verbosity);
    }

    static inline std::unique_ptr<extension::sdk::LogicalAggStage> make() {
        return std::make_unique<SimpleSerializationLogicalStage>();
    }
};

TEST(AggregationStageTest, SimpleSerializationSucceeds) {
    auto logicalStage =
        new extension::sdk::ExtensionLogicalAggStage(SimpleSerializationLogicalStage::make());
    auto handle = extension::LogicalAggStageHandle{logicalStage};

    auto serialized = handle.serialize();
    ASSERT_BSONOBJ_EQ(BSON(SimpleSerializationLogicalStage::kStageName
                           << SimpleSerializationLogicalStage::kStageSpec),
                      serialized);
}

TEST(AggregationStageTest, Explain) {
    auto logicalStage =
        new extension::sdk::ExtensionLogicalAggStage(SimpleSerializationLogicalStage::make());
    auto handle = extension::LogicalAggStageHandle{logicalStage};

    // Test that different verbosity levels can be passed through to the extension implementation
    // correctly.
    {
        auto output = handle.explain(ExplainOptions::Verbosity::kQueryPlanner);
        ASSERT_BSONOBJ_EQ(BSON(SimpleSerializationLogicalStage::kStageName
                               << ::MongoExtensionExplainVerbosity::kQueryPlanner),
                          output);
    }

    {
        auto output = handle.explain(ExplainOptions::Verbosity::kExecStats);
        ASSERT_BSONOBJ_EQ(BSON(SimpleSerializationLogicalStage::kStageName
                               << ::MongoExtensionExplainVerbosity::kExecStats),
                          output);
    }

    {
        auto output = handle.explain(ExplainOptions::Verbosity::kExecAllPlans);
        ASSERT_BSONOBJ_EQ(BSON(SimpleSerializationLogicalStage::kStageName
                               << ::MongoExtensionExplainVerbosity::kExecAllPlans),
                          output);
    }
}

class SimpleQueryShapeParseNode : public sdk::AggStageParseNode {
public:
    static constexpr StringData kStageName = "$simpleQueryShape";
    static constexpr StringData kStageSpec = "mongodb";

    SimpleQueryShapeParseNode() : sdk::AggStageParseNode(toStdStringViewForInterop(kStageName)) {}

    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<sdk::VariantNode> expand() const override {
        return {};
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSON(kStageName << kStageSpec);
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<SimpleQueryShapeParseNode>();
    }
};

TEST_F(AggStageTest, SimpleComputeQueryShapeSucceeds) {
    auto parseNode =
        new extension::sdk::ExtensionAggStageParseNode(SimpleQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts{};
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);
    ASSERT_BSONOBJ_EQ(
        BSON(SimpleQueryShapeParseNode::kStageName << SimpleQueryShapeParseNode::kStageSpec),
        queryShape);
}

class IdentifierQueryShapeParseNode : public sdk::AggStageParseNode {
public:
    static constexpr StringData kStageName = "$identifierQueryShape";
    static constexpr StringData kIndexFieldName = "index";
    static constexpr StringData kIndexValue = "identifier";

    IdentifierQueryShapeParseNode()
        : sdk::AggStageParseNode(toStdStringViewForInterop(kStageName)) {}

    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<sdk::VariantNode> expand() const override {
        return {};
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        sdk::QueryShapeOptsHandle ctxHandle(ctx);
        BSONObjBuilder builder;

        builder.append(kIndexFieldName, ctxHandle.serializeIdentifier(std::string(kIndexValue)));

        return BSON(kStageName << builder.obj());
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<IdentifierQueryShapeParseNode>();
    }

    static std::string applyHmacForTest(StringData sd) {
        return "REDACT_" + std::string{sd};
    }
};

TEST_F(AggStageTest, SerializingIdentifierQueryShapeSucceedsWithNoTransformation) {
    auto parseNode = new ExtensionAggStageParseNode(IdentifierQueryShapeParseNode::make());

    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts{};
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);
    ASSERT_BSONOBJ_EQ(BSON(IdentifierQueryShapeParseNode::kStageName
                           << BSON(IdentifierQueryShapeParseNode::kIndexFieldName
                                   << IdentifierQueryShapeParseNode::kIndexValue)),
                      queryShape);
}

TEST_F(AggStageTest, SerializingIdentifierQueryShapeSucceedsWithTransformation) {
    auto parseNode = new ExtensionAggStageParseNode(IdentifierQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
    opts.transformIdentifiers = true;
    opts.transformIdentifiersCallback = IdentifierQueryShapeParseNode::applyHmacForTest;

    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);
    ASSERT_BSONOBJ_EQ(BSON(IdentifierQueryShapeParseNode::kStageName
                           << BSON(IdentifierQueryShapeParseNode::kIndexFieldName
                                   << IdentifierQueryShapeParseNode::applyHmacForTest(
                                          IdentifierQueryShapeParseNode::kIndexValue))),
                      queryShape);
}

TEST_F(AggStageTest, DesugarToEmptyDescriptorParseTest) {
    auto descriptor = std::make_unique<ExtensionAggStageDescriptor>(
        shared_test_stages::NoOpAggStageDescriptor::make());
    auto handle = extension::AggStageDescriptorHandle{descriptor.get()};

    BSONObj stageBson =
        BSON(shared_test_stages::NoOpAggStageDescriptor::kStageName << BSON("foo" << true));
    auto parseNodeHandle = handle.parse(stageBson);

    auto expanded = parseNodeHandle.expand();

    ASSERT_EQUALS(expanded.size(), 1);
    ASSERT_TRUE(std::holds_alternative<extension::AggStageAstNodeHandle>(expanded[0]));
}

TEST_F(AggStageTest, SourceStageParseTest) {
    auto descriptor = std::make_unique<ExtensionAggStageDescriptor>(
        shared_test_stages::SourceAggStageDescriptor::make());
    auto handle = extension::AggStageDescriptorHandle{descriptor.get()};

    BSONObj stageBson =
        BSON(shared_test_stages::SourceAggStageDescriptor::kStageName << BSON("foo" << true));
    auto parseNodeHandle = handle.parse(stageBson);
    ASSERT_EQ(shared_test_stages::SourceAggStageDescriptor::kStageName, handle.getName());
    ASSERT_EQ(::MongoExtensionAggStageType::kSource, handle.getType());
}

class FieldPathQueryShapeParseNode : public sdk::AggStageParseNode {
public:
    static constexpr StringData kStageName = "$fieldPathQueryShape";
    static constexpr StringData kSingleFieldPath = "simpleField";
    static constexpr StringData kNestedFieldPath = "nested.Field.Path";

    FieldPathQueryShapeParseNode()
        : sdk::AggStageParseNode(toStdStringViewForInterop(kStageName)) {}

    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<sdk::VariantNode> expand() const override {
        return {};
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        sdk::QueryShapeOptsHandle ctxHandle(ctx);
        BSONObjBuilder builder;

        builder.append(kSingleFieldPath,
                       ctxHandle.serializeFieldPath(std::string(kSingleFieldPath)));
        builder.append(kNestedFieldPath,
                       ctxHandle.serializeFieldPath(std::string(kNestedFieldPath)));

        return BSON(kStageName << builder.obj());
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<FieldPathQueryShapeParseNode>();
    }

    static std::string applyHmacForTest(StringData sd) {
        return "REDACT_" + std::string{sd};
    }
};

TEST_F(AggStageTest, SerializingFieldPathQueryShapeSucceedsWithNoTransformation) {
    auto parseNode = new ExtensionAggStageParseNode(FieldPathQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts{};
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);

    ASSERT_BSONOBJ_EQ(BSON(FieldPathQueryShapeParseNode::kStageName
                           << BSON(FieldPathQueryShapeParseNode::kSingleFieldPath
                                   << FieldPathQueryShapeParseNode::kSingleFieldPath
                                   << FieldPathQueryShapeParseNode::kNestedFieldPath
                                   << FieldPathQueryShapeParseNode::kNestedFieldPath)),
                      queryShape);
}

TEST_F(AggStageTest, SerializingFieldPathQueryShapeSucceedsWithTransformation) {
    auto parseNode = new ExtensionAggStageParseNode(FieldPathQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts{};
    opts.transformIdentifiers = true;
    opts.transformIdentifiersCallback = FieldPathQueryShapeParseNode::applyHmacForTest;

    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);

    auto transformedSingleField =
        opts.transformIdentifiersCallback(FieldPathQueryShapeParseNode::kSingleFieldPath);

    std::stringstream ss;
    ss << opts.transformIdentifiersCallback("nested") << "."
       << opts.transformIdentifiersCallback("Field") << "."
       << opts.transformIdentifiersCallback("Path");
    auto transformedNestedField = ss.str();

    ASSERT_BSONOBJ_EQ(
        BSON(FieldPathQueryShapeParseNode::kStageName
             << BSON(FieldPathQueryShapeParseNode::kSingleFieldPath
                     << transformedSingleField << FieldPathQueryShapeParseNode::kNestedFieldPath
                     << transformedNestedField)),
        queryShape);
}

class LiteralQueryShapeParseNode : public sdk::AggStageParseNode {
public:
    static constexpr StringData kStageName = "$literalQueryShape";
    static constexpr StringData kStringField = "str";
    static constexpr StringData kStringValue = "mongodb";
    static constexpr StringData kNumberField = "num";
    static constexpr int kNumberValue = 5;
    static constexpr StringData kObjectField = "obj";
    static const BSONObj kObjectValue;
    static constexpr StringData kDateField = "date";
    static const Date_t kDateValue;

    LiteralQueryShapeParseNode() : sdk::AggStageParseNode(toStdStringViewForInterop(kStageName)) {}

    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<sdk::VariantNode> expand() const override {
        return {};
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        sdk::QueryShapeOptsHandle ctxHandle(ctx);

        // Build a BSON object for the spec and keep the memory in scope across the calls to
        // serialize literal.
        BSONObjBuilder specBuilder;
        specBuilder.append(kStringField, kStringValue);
        specBuilder.append(kNumberField, kNumberValue);
        specBuilder.append(kObjectField, kObjectValue);
        specBuilder.append(kDateField, kDateValue);
        auto spec = specBuilder.obj();

        // Build the query shape.
        BSONObjBuilder builder;
        ctxHandle.appendLiteral(builder, kStringField, spec[kStringField]);
        ctxHandle.appendLiteral(builder, kNumberField, spec[kNumberField]);
        ctxHandle.appendLiteral(builder, kObjectField, spec[kObjectField]);
        ctxHandle.appendLiteral(builder, kDateField, spec[kDateField]);
        return BSON(kStageName << builder.obj());
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<LiteralQueryShapeParseNode>();
    }

    static std::string applyHmacForTest(StringData sd) {
        return "REDACT_" + std::string{sd};
    }
};

inline const BSONObj LiteralQueryShapeParseNode::kObjectValue = BSON("hi" << "mongodb");
inline const Date_t LiteralQueryShapeParseNode::kDateValue = Date_t::fromMillisSinceEpoch(1000);

TEST_F(AggStageTest, SerializingLiteralQueryShapeSucceedsWithNoTransformation) {
    auto parseNode = new ExtensionAggStageParseNode(LiteralQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts{};
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);

    BSONObjBuilder specBuilder;
    specBuilder.append(LiteralQueryShapeParseNode::kStringField,
                       LiteralQueryShapeParseNode::kStringValue);
    specBuilder.append(LiteralQueryShapeParseNode::kNumberField,
                       LiteralQueryShapeParseNode::kNumberValue);
    specBuilder.append(LiteralQueryShapeParseNode::kObjectField,
                       LiteralQueryShapeParseNode::kObjectValue);
    specBuilder.append(LiteralQueryShapeParseNode::kDateField,
                       LiteralQueryShapeParseNode::kDateValue);
    auto spec = specBuilder.obj();

    ASSERT_BSONOBJ_EQ(BSON(LiteralQueryShapeParseNode::kStageName << spec), queryShape);
}

TEST_F(AggStageTest, SerializingLiteralQueryShapeSucceedsWithDebugShape) {
    auto parseNode = new ExtensionAggStageParseNode(LiteralQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);

    BSONObjBuilder specBuilder;
    specBuilder.append(LiteralQueryShapeParseNode::kStringField, "?string");
    specBuilder.append(LiteralQueryShapeParseNode::kNumberField, "?number");
    specBuilder.append(LiteralQueryShapeParseNode::kObjectField, "?object");
    specBuilder.append(LiteralQueryShapeParseNode::kDateField, "?date");
    auto spec = specBuilder.obj();

    ASSERT_BSONOBJ_EQ(BSON(LiteralQueryShapeParseNode::kStageName << spec), queryShape);
}

TEST_F(AggStageTest, SerializingLiteralQueryShapeSucceedsWithRepresentativeValues) {
    auto parseNode = new ExtensionAggStageParseNode(LiteralQueryShapeParseNode::make());
    auto handle = extension::AggStageParseNodeHandle{parseNode};

    SerializationOptions opts = SerializationOptions::kRepresentativeQueryShapeSerializeOptions;
    extension::host_connector::QueryShapeOptsAdapter adapter{&opts};
    auto queryShape = handle.getQueryShape(adapter);

    BSONObjBuilder specBuilder;
    specBuilder.append(LiteralQueryShapeParseNode::kStringField, "?");
    specBuilder.append(LiteralQueryShapeParseNode::kNumberField, 1);
    specBuilder.append(LiteralQueryShapeParseNode::kObjectField, BSON("?" << "?"));
    specBuilder.append(LiteralQueryShapeParseNode::kDateField, Date_t::fromMillisSinceEpoch(0));
    auto spec = specBuilder.obj();

    ASSERT_BSONOBJ_EQ(BSON(LiteralQueryShapeParseNode::kStageName << spec), queryShape);
}

class NoOpExtensionExecAggStage : public extension::sdk::ExecAggStage {
public:
    extension::ExtensionGetNextResult getNext() override {
        MONGO_UNIMPLEMENTED;
    }

    static inline std::unique_ptr<extension::sdk::ExecAggStage> make() {
        return std::make_unique<NoOpExtensionExecAggStage>();
    }
};

class ValidExtensionExecAggStage : public extension::sdk::ExecAggStage {
public:
    extension::ExtensionGetNextResult getNext() override {
        if (_results.empty()) {
            return extension::ExtensionGetNextResult::eof();
        }
        if (_results.size() == 2) {
            // The result at the front of the queue is removed so that the size doesn't stay at 2.
            // This needs to be done so that the EOF case can be tested. Note that the behavior of
            // removing from the results queue for a "pause execution" state does not accurately
            // represent a "paused execution" state in a getNext() function.
            _results.pop_front();
            return extension::ExtensionGetNextResult::pauseExecution();
        } else {
            auto result = extension::ExtensionGetNextResult::advanced(_results.front());
            _results.pop_front();
            return result;
        }
    }

    static inline std::unique_ptr<extension::sdk::ExecAggStage> make() {
        return std::make_unique<ValidExtensionExecAggStage>();
    }

private:
    std::deque<BSONObj> _results = {
        BSON("$meow" << "adithi"), BSON("$meow" << "josh"), BSON("$meow" << "cedric")};
};

class InvalidExtensionExecAggStageAdvancedState : public extension::sdk::ExecAggStage {
public:
    extension::ExtensionGetNextResult getNext() override {
        return {.code = extension::GetNextCode::kAdvanced, .res = boost::none};
    }

    static inline std::unique_ptr<extension::sdk::ExecAggStage> make() {
        return std::make_unique<InvalidExtensionExecAggStageAdvancedState>();
    }

private:
    std::deque<BSONObj> _results = {};
};

class InvalidExtensionExecAggStagePauseExecutionState : public extension::sdk::ExecAggStage {
public:
    extension::ExtensionGetNextResult getNext() override {
        return {.code = extension::GetNextCode::kPauseExecution,
                .res = boost::make_optional(BSON("$dog" << "I should not exist"))};
    }

    static inline std::unique_ptr<extension::sdk::ExecAggStage> make() {
        return std::make_unique<InvalidExtensionExecAggStagePauseExecutionState>();
    }

private:
    std::deque<BSONObj> _results = {};
};

class InvalidExtensionExecAggStageEofState : public extension::sdk::ExecAggStage {
public:
    extension::ExtensionGetNextResult getNext() override {
        return {.code = extension::GetNextCode::kEOF,
                .res = boost::make_optional(BSON("$dog" << "I should not exist"))};
    }

    static inline std::unique_ptr<extension::sdk::ExecAggStage> make() {
        return std::make_unique<InvalidExtensionExecAggStageEofState>();
    }

private:
    std::deque<BSONObj> _results = {};
};

class InvalidExtensionExecAggStageGetNextCode : public extension::sdk::ExecAggStage {
public:
    extension::ExtensionGetNextResult getNext() override {
        return {.code = static_cast<const GetNextCode>(10), .res = boost::none};
    }

    static inline std::unique_ptr<extension::sdk::ExecAggStage> make() {
        return std::make_unique<InvalidExtensionExecAggStageGetNextCode>();
    }

private:
    std::deque<BSONObj> _results = {};
};

class ExecAggStageVTableTest : public unittest::Test {
public:
    // This special handle class is only used within this fixture so that we can unit test the
    // assertVTableConstraints functionality of the handle.
    class TestExecAggStageVTableHandle : public extension::host_connector::ExecAggStageHandle {
    public:
        TestExecAggStageVTableHandle(absl::Nonnull<::MongoExtensionExecAggStage*> execAggStage)
            : extension::host_connector::ExecAggStageHandle(execAggStage) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

DEATH_TEST_F(ExecAggStageVTableTest, InvalidExecAggStageVTableFailsGetNext, "10956800") {
    auto noOpExecAggStage =
        new extension::sdk::ExtensionExecAggStage(NoOpExtensionExecAggStage::make());
    auto handle = TestExecAggStageVTableHandle{noOpExecAggStage};

    auto vtable = handle.vtable();
    vtable.get_next = nullptr;
    handle.assertVTableConstraints(vtable);
};

TEST(AggregationStageTest, ValidExecAggStageVTableGetNextSucceeds) {
    auto validExecAggStage =
        new extension::sdk::ExtensionExecAggStage(ValidExtensionExecAggStage::make());
    auto handle = extension::host_connector::ExecAggStageHandle{validExecAggStage};

    auto getNext = handle.getNext();
    ASSERT_EQUALS(extension::GetNextCode::kAdvanced, getNext.code);
    ASSERT_BSONOBJ_EQ(BSON("$meow" << "adithi"), getNext.res.get());

    getNext = handle.getNext();
    ASSERT_EQUALS(extension::GetNextCode::kPauseExecution, getNext.code);
    ASSERT_EQ(boost::none, getNext.res);

    getNext = handle.getNext();
    ASSERT_EQUALS(extension::GetNextCode::kAdvanced, getNext.code);
    ASSERT_BSONOBJ_EQ(BSON("$meow" << "cedric"), getNext.res.get());

    getNext = handle.getNext();
    ASSERT_EQUALS(extension::GetNextCode::kEOF, getNext.code);
    ASSERT_EQ(boost::none, getNext.res);
};

DEATH_TEST_F(AggStageTest, InvalidExtensionGetNextResultAdvanced, "10956801") {
    auto invalidExtensionExecAggStageAdvancedState = new extension::sdk::ExtensionExecAggStage(
        InvalidExtensionExecAggStageAdvancedState::make());
    auto handle =
        extension::host_connector::ExecAggStageHandle{invalidExtensionExecAggStageAdvancedState};
    [[maybe_unused]] auto getNext = handle.getNext();
};

DEATH_TEST_F(AggStageTest, InvalidExtensionGetNextResultPauseExecution, "10956802") {
    auto invalidExtensionExecAggStagePauseExecutionState =
        new extension::sdk::ExtensionExecAggStage(
            InvalidExtensionExecAggStagePauseExecutionState::make());
    auto handle = extension::host_connector::ExecAggStageHandle{
        invalidExtensionExecAggStagePauseExecutionState};
    [[maybe_unused]] auto getNext = handle.getNext();
};

DEATH_TEST_F(AggStageTest, InvalidExtensionGetNextResultEOF, "10956805") {
    auto invalidExtensionExecAggStageEofState =
        new extension::sdk::ExtensionExecAggStage(InvalidExtensionExecAggStageEofState::make());
    auto handle =
        extension::host_connector::ExecAggStageHandle{invalidExtensionExecAggStageEofState};
    [[maybe_unused]] auto getNext = handle.getNext();
};

DEATH_TEST_F(AggStageTest, InvalidMongoExtensionGetNextResultCode, "10956803") {
    ::MongoExtensionGetNextResult result = {
        .code = static_cast<::MongoExtensionGetNextResultCode>(10), .result = nullptr};
    [[maybe_unused]] auto converted =
        extension::host_connector::convertCRepresentationToGetNextResult(&result);
};

TEST_F(AggStageTest, ValidateStructStateAfterConvertingStructToGetNextResult) {
    ::MongoExtensionGetNextResult result = {
        .code = static_cast<::MongoExtensionGetNextResultCode>(10), .result = nullptr};
    ASSERT_THROWS_WITH_CHECK(
        [&] {
            [[maybe_unused]] auto converted =
                extension::host_connector::convertCRepresentationToGetNextResult(&result);
        }(),
        AssertionException,
        [](const AssertionException& ex) {
            ASSERT_EQ(ex.code(), 10956803);
            ASSERT_STRING_CONTAINS(
                ex.reason(), str::stream() << "Invalid MongoExtensionGetNextResultCode: " << 10);
            assertionCount.tripwire.subtractAndFetch(1);
        });
    ASSERT_EQ(static_cast<::MongoExtensionGetNextResultCode>(10), result.code);
    ASSERT_EQ(nullptr, result.result);

    result = {.code = ::MongoExtensionGetNextResultCode::kAdvanced,
              .result = new VecByteBuf(BSON("$adithi" << "{cats: 0}"))};
    auto converted = extension::host_connector::convertCRepresentationToGetNextResult(&result);
    ASSERT_EQ(GetNextCode::kAdvanced, converted.code);
    ASSERT_BSONOBJ_EQ(BSON("$adithi" << "{cats: 0}"), converted.res.get());
};

DEATH_TEST_F(AggStageTest, InvalidGetNextCode, "10956804") {
    auto invalidExtensionExecAggStageGetNextCode =
        new extension::sdk::ExtensionExecAggStage(InvalidExtensionExecAggStageGetNextCode::make());
    auto handle =
        extension::host_connector::ExecAggStageHandle{invalidExtensionExecAggStageGetNextCode};
    [[maybe_unused]] auto getNext = handle.getNext();
};

}  // namespace

}  // namespace mongo::extension::sdk
