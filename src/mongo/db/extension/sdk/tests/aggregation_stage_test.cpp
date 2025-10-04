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
#include "mongo/db/extension/host_adapter/aggregation_stage.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/query_shape_opts_handle.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {

class NoOpLogicalAggregationStage : public extension::sdk::LogicalAggregationStage {
public:
    NoOpLogicalAggregationStage() {}
};

class NoOpAstNode : public extension::sdk::AggregationStageAstNode {
public:
    std::unique_ptr<extension::sdk::LogicalAggregationStage> bind() const override {
        return std::make_unique<NoOpLogicalAggregationStage>();
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageAstNode> make() {
        return std::make_unique<NoOpAstNode>();
    }
};

class NoOpParseNode : public extension::sdk::AggregationStageParseNode {
public:
    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<extension::sdk::VariantNode> expand() const override {
        std::vector<extension::sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(std::make_unique<NoOpAstNode>());
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageParseNode> make() {
        return std::make_unique<NoOpParseNode>();
    }
};

class DesugarToEmptyDescriptor : public extension::sdk::AggregationStageDescriptor {
public:
    static inline const std::string kStageName = "$emptyDesugarExtension";

    DesugarToEmptyDescriptor()
        : extension::sdk::AggregationStageDescriptor(kStageName,
                                                     MongoExtensionAggregationStageType::kDesugar) {
    }

    std::unique_ptr<extension::sdk::LogicalAggregationStage> parse(
        BSONObj stageBson) const override {
        return std::make_unique<NoOpLogicalAggregationStage>();
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageDescriptor> make() {
        return std::make_unique<DesugarToEmptyDescriptor>();
    }
};

class DesugarToEmptyParseNode : public extension::sdk::AggregationStageParseNode {
public:
    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<extension::sdk::VariantNode> expand() const override {
        return {};
    }

    BSONObj getQueryShape(const ::MongoHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageParseNode> make() {
        return std::make_unique<DesugarToEmptyParseNode>();
    }
};

class CountingAST final : public extension::sdk::AggregationStageAstNode {
public:
    static int alive;

    CountingAST() {
        ++alive;
    }

    ~CountingAST() override {
        --alive;
    }

    std::unique_ptr<extension::sdk::LogicalAggregationStage> bind() const override {
        return std::make_unique<NoOpLogicalAggregationStage>();
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageAstNode> make() {
        return std::make_unique<CountingAST>();
    }
};

class CountingParse final : public extension::sdk::AggregationStageParseNode {
public:
    static constexpr size_t kExpansionSize = 1;
    static int alive;

    CountingParse() {
        ++alive;
    }

    ~CountingParse() override {
        --alive;
    }

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<extension::sdk::VariantNode> expand() const override {
        std::vector<extension::sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(std::make_unique<CountingAST>());
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageParseNode> make() {
        return std::make_unique<CountingParse>();
    }
};

int CountingParse::alive = 0;
int CountingAST::alive = 0;

class NestedDesugaringParseNode final : public extension::sdk::AggregationStageParseNode {
public:
    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<extension::sdk::VariantNode> expand() const override {
        std::vector<extension::sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(std::make_unique<CountingAST>());
        expanded.emplace_back(std::make_unique<CountingParse>());
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageParseNode> make() {
        return std::make_unique<NestedDesugaringParseNode>();
    }
};

class GetExpandedSizeLessThanActualExpansionSizeParseNode final
    : public extension::sdk::AggregationStageParseNode {
public:
    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize - 1;
    }

    std::vector<extension::sdk::VariantNode> expand() const override {
        std::vector<extension::sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(std::make_unique<CountingAST>());
        expanded.emplace_back(std::make_unique<CountingParse>());
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageParseNode> make() {
        return std::make_unique<GetExpandedSizeLessThanActualExpansionSizeParseNode>();
    }
};

class GetExpandedSizeGreaterThanActualExpansionSizeParseNode final
    : public extension::sdk::AggregationStageParseNode {
public:
    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize + 1;
    }

    std::vector<extension::sdk::VariantNode> expand() const override {
        std::vector<extension::sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(std::make_unique<CountingAST>());
        expanded.emplace_back(std::make_unique<CountingParse>());
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageParseNode> make() {
        return std::make_unique<GetExpandedSizeGreaterThanActualExpansionSizeParseNode>();
    }
};

class ParseNodeVTableTest : public unittest::Test {
public:
    // This special handle class is only used within this fixture so that we can unit test the
    // assertVTableConstraints functionality of the handle.
    class TestParseNodeVTableHandle
        : public extension::host_adapter::AggregationStageParseNodeHandle {
    public:
        TestParseNodeVTableHandle(
            absl::Nonnull<::MongoExtensionAggregationStageParseNode*> parseNode)
            : extension::host_adapter::AggregationStageParseNodeHandle(parseNode) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

class AstNodeVTableTest : public unittest::Test {
public:
    class TestAstNodeVTableHandle : public extension::host_adapter::AggregationStageAstNodeHandle {
    public:
        TestAstNodeVTableHandle(absl::Nonnull<::MongoExtensionAggregationStageAstNode*> astNode)
            : extension::host_adapter::AggregationStageAstNodeHandle(astNode) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

TEST(AggregationStageTest, CountingParseExpansionSucceedsTest) {
    auto countingParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(CountingParse::make());
    auto handle =
        extension::host_adapter::AggregationStageParseNodeHandle{countingParseNode.release()};

    auto expanded = handle.expand();
    ASSERT_EQUALS(expanded.size(), 1);

    // TODO SERVER-111605 Check expansion results by name once get_name is added.
    ASSERT_TRUE(std::holds_alternative<extension::host_adapter::AggregationStageAstNodeHandle>(
        expanded[0]));
}

TEST(AggregationStageTest, NestedExpansionSucceedsTest) {
    auto nestedDesugarParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(
            NestedDesugaringParseNode::make());
    auto handle =
        extension::host_adapter::AggregationStageParseNodeHandle{nestedDesugarParseNode.release()};

    auto expanded = handle.expand();
    ASSERT_EQUALS(expanded.size(), 2);

    // TODO SERVER-111605 Check expansion results by name once get_name is added.
    // First element is the AST node.
    ASSERT_TRUE(std::holds_alternative<extension::host_adapter::AggregationStageAstNodeHandle>(
        expanded[0]));

    // Second element is the nested ParseNode.
    ASSERT_TRUE(std::holds_alternative<extension::host_adapter::AggregationStageParseNodeHandle>(
        expanded[1]));
    auto& nestedHandle =
        std::get<extension::host_adapter::AggregationStageParseNodeHandle>(expanded[1]);

    // Expand the nested node.
    auto nestedExpanded = nestedHandle.expand();
    ASSERT_EQUALS(nestedExpanded.size(), 1);
    ASSERT_TRUE(std::holds_alternative<extension::host_adapter::AggregationStageAstNodeHandle>(
        nestedExpanded[0]));
}

TEST(AggregationStageTest, HandlesPreventMemoryLeaksOnSuccess) {
    CountingAST::alive = 0;
    CountingParse::alive = 0;

    auto nestedDesugarParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(
            NestedDesugaringParseNode::make());

    {
        auto handle = extension::host_adapter::AggregationStageParseNodeHandle{
            nestedDesugarParseNode.release()};

        [[maybe_unused]] auto expanded = handle.expand();
        ASSERT_EQUALS(CountingAST::alive, 1);
        ASSERT_EQUALS(CountingParse::alive, 1);
    }

    // Assert that the result of expand(), a vector of VariantNodeHandles, is properly cleaned up
    // once it goes out of scope.
    ASSERT_EQUALS(CountingAST::alive, 0);
    ASSERT_EQUALS(CountingParse::alive, 0);
}

TEST(AggregationStageTest, HandlesPreventMemoryLeaksOnFailure) {
    CountingAST::alive = 0;
    CountingParse::alive = 0;

    auto nestedDesugarParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(
            NestedDesugaringParseNode::make());

    auto handle =
        extension::host_adapter::AggregationStageParseNodeHandle{nestedDesugarParseNode.release()};

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
    ASSERT_EQUALS(CountingAST::alive, 0);
    ASSERT_EQUALS(CountingParse::alive, 0);

    failExpand->setMode(FailPoint::off, 0);
}

DEATH_TEST(AggregationStageTest, EmptyDesugarExpansionFails, "11113803") {
    auto emptyDesugarParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(
            DesugarToEmptyParseNode::make());
    auto handle =
        extension::host_adapter::AggregationStageParseNodeHandle{emptyDesugarParseNode.release()};

    [[maybe_unused]] auto expanded = handle.expand();
}

DEATH_TEST(AggregationStageTest, GetExpandedSizeLessThanActualExpansionSizeFails, "11113802") {
    auto getExpandedSizeLessThanActualExpansionSizeParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(
            GetExpandedSizeLessThanActualExpansionSizeParseNode::make());
    auto handle = extension::host_adapter::AggregationStageParseNodeHandle{
        getExpandedSizeLessThanActualExpansionSizeParseNode.release()};

    [[maybe_unused]] auto expanded = handle.expand();
}

DEATH_TEST(AggregationStageTest, GetExpandedSizeGreaterThanActualExpansionSizeFails, "11113802") {
    auto getExpandedSizeGreaterThanActualExpansionSizeParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(
            GetExpandedSizeGreaterThanActualExpansionSizeParseNode::make());
    auto handle = extension::host_adapter::AggregationStageParseNodeHandle{
        getExpandedSizeGreaterThanActualExpansionSizeParseNode.release()};

    [[maybe_unused]] auto expanded = handle.expand();
}

DEATH_TEST_F(ParseNodeVTableTest, InvalidParseNodeVTableFailsGetQueryShape, "10977601") {
    auto noOpParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(NoOpParseNode::make());
    auto handle = TestParseNodeVTableHandle{noOpParseNode.release()};

    auto vtable = handle.vtable();
    vtable.get_query_shape = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ParseNodeVTableTest, InvalidParseNodeVTableFailsGetExpandedSize, "11113800") {
    auto noOpParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(NoOpParseNode::make());
    auto handle = TestParseNodeVTableHandle{noOpParseNode.release()};

    auto vtable = handle.vtable();
    vtable.get_expanded_size = nullptr;
    handle.assertVTableConstraints(vtable);
};

DEATH_TEST_F(ParseNodeVTableTest, InvalidParseNodeVTableFailsExpand, "10977602") {
    auto noOpParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(NoOpParseNode::make());
    auto handle = TestParseNodeVTableHandle{noOpParseNode.release()};

    auto vtable = handle.vtable();
    vtable.expand = nullptr;
    handle.assertVTableConstraints(vtable);
};

TEST(AggregationStageTest, NoOpAstNodeTest) {
    auto noOpAggregationStageAstNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageAstNode>(NoOpAstNode::make());
    auto handle = extension::host_adapter::AggregationStageAstNodeHandle{
        noOpAggregationStageAstNode.release()};

    [[maybe_unused]] auto logicalStageHandle = handle.bind();
}

DEATH_TEST_F(AstNodeVTableTest, InvalidAstNodeVTable, "11113700") {
    auto noOpAstNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageAstNode>(NoOpAstNode::make());
    auto handle = TestAstNodeVTableHandle{noOpAstNode.release()};

    auto vtable = handle.vtable();
    vtable.bind = nullptr;
    handle.assertVTableConstraints(vtable);
};

class SimpleQueryShapeParseNode : public extension::sdk::AggregationStageParseNode {
public:
    static constexpr StringData kStageName = "$simpleQueryShape";
    static constexpr StringData kStageSpec = "mongodb";

    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<extension::sdk::VariantNode> expand() const override {
        return {};
    }

    BSONObj getQueryShape(const ::MongoHostQueryShapeOpts* ctx) const override {
        return BSON(kStageName << kStageSpec);
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageParseNode> make() {
        return std::make_unique<SimpleQueryShapeParseNode>();
    }
};

TEST(AggregationStageTest, SimpleComputeQueryShapeSucceeds) {
    auto parseNode = std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(
        SimpleQueryShapeParseNode::make());
    auto handle = extension::host_adapter::AggregationStageParseNodeHandle{parseNode.release()};

    SerializationOptions opts{};
    auto queryShape = handle.getQueryShape(opts);
    ASSERT_BSONOBJ_EQ(
        BSON(SimpleQueryShapeParseNode::kStageName << SimpleQueryShapeParseNode::kStageSpec),
        queryShape);
}

class IdentifierQueryShapeParseNode : public extension::sdk::AggregationStageParseNode {
public:
    static constexpr StringData kStageName = "$identifierQueryShape";
    static constexpr StringData kIndexFieldName = "index";
    static constexpr StringData kIndexValue = "identifier";

    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<extension::sdk::VariantNode> expand() const override {
        return {};
    }

    BSONObj getQueryShape(const ::MongoHostQueryShapeOpts* ctx) const override {
        extension::sdk::QueryShapeOptsHandle ctxHandle(ctx);
        BSONObjBuilder builder;

        builder.append(kIndexFieldName, ctxHandle.serializeIdentifier(std::string(kIndexValue)));

        return BSON(kStageName << builder.obj());
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageParseNode> make() {
        return std::make_unique<IdentifierQueryShapeParseNode>();
    }

    static std::string applyHmacForTest(StringData sd) {
        return "REDACT_" + std::string{sd};
    }
};

TEST(AggregationStageTest, SerializingIdentifierQueryShapeSucceedsWithNoTransformation) {
    auto parseNode = std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(
        IdentifierQueryShapeParseNode::make());
    auto handle = extension::host_adapter::AggregationStageParseNodeHandle{parseNode.release()};

    SerializationOptions opts{};
    auto queryShape = handle.getQueryShape(opts);
    ASSERT_BSONOBJ_EQ(BSON(IdentifierQueryShapeParseNode::kStageName
                           << BSON(IdentifierQueryShapeParseNode::kIndexFieldName
                                   << IdentifierQueryShapeParseNode::kIndexValue)),
                      queryShape);
}

TEST(AggregationStageTest, SerializingIdentifierQueryShapeSucceedsWithTransformation) {
    auto parseNode = std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(
        IdentifierQueryShapeParseNode::make());
    auto handle = extension::host_adapter::AggregationStageParseNodeHandle{parseNode.release()};

    SerializationOptions opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
    opts.transformIdentifiers = true;
    opts.transformIdentifiersCallback = IdentifierQueryShapeParseNode::applyHmacForTest;

    auto queryShape = handle.getQueryShape(opts);
    ASSERT_BSONOBJ_EQ(BSON(IdentifierQueryShapeParseNode::kStageName
                           << BSON(IdentifierQueryShapeParseNode::kIndexFieldName
                                   << IdentifierQueryShapeParseNode::applyHmacForTest(
                                          IdentifierQueryShapeParseNode::kIndexValue))),
                      queryShape);
}

}  // namespace
}  // namespace mongo
