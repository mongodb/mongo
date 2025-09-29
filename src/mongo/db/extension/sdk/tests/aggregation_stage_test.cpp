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

class NoOpParseNode : public extension::sdk::AggregationStageParseNode {
public:
    BSONArray expand() const override {
        return {};
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageParseNode> make() {
        return std::make_unique<NoOpParseNode>();
    }
};

class NoOpAggregationStageAstNode : public extension::sdk::AggregationStageAstNode {
public:
    std::unique_ptr<extension::sdk::LogicalAggregationStage> bind() const override {
        return std::make_unique<NoOpLogicalAggregationStage>();
    }


    static inline std::unique_ptr<extension::sdk::AggregationStageAstNode> make() {
        return std::make_unique<NoOpAggregationStageAstNode>();
    }
};

class DesugarAsMatchAndLimitDescriptor : public extension::sdk::AggregationStageDescriptor {
public:
    static inline const std::string kStageName = "$matchLimitDesugarExtension";

    DesugarAsMatchAndLimitDescriptor()
        : extension::sdk::AggregationStageDescriptor(kStageName,
                                                     MongoExtensionAggregationStageType::kDesugar) {
    }

    std::unique_ptr<extension::sdk::LogicalAggregationStage> parse(
        BSONObj stageBson) const override {
        return std::make_unique<NoOpLogicalAggregationStage>();
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageDescriptor> make() {
        return std::make_unique<DesugarAsMatchAndLimitDescriptor>();
    }
};

class DesugarAsMatchAndLimitParseNode : public extension::sdk::AggregationStageParseNode {
public:
    BSONArray expand() const override {
        return BSON_ARRAY(BSON("$match" << BSONObj()) << BSON("$limit" << 1));
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageParseNode> make() {
        return std::make_unique<DesugarAsMatchAndLimitParseNode>();
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
    BSONArray expand() const override {
        return {};
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageParseNode> make() {
        return std::make_unique<DesugarToEmptyParseNode>();
    }
};

class BadDesugarArrayElementsDescriptor : public extension::sdk::AggregationStageDescriptor {
public:
    static inline const std::string kStageName = "$badArrayEltsDesugarExtension";

    BadDesugarArrayElementsDescriptor()
        : extension::sdk::AggregationStageDescriptor(kStageName,
                                                     MongoExtensionAggregationStageType::kDesugar) {
    }

    std::unique_ptr<extension::sdk::LogicalAggregationStage> parse(
        BSONObj stageBson) const override {
        return std::make_unique<NoOpLogicalAggregationStage>();
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageDescriptor> make() {
        return std::make_unique<BadDesugarArrayElementsDescriptor>();
    }
};

class BadDesugarArrayElementsParseNode : public extension::sdk::AggregationStageParseNode {
public:
    BSONArray expand() const override {
        return BSON_ARRAY(1);
    }

    static inline std::unique_ptr<extension::sdk::AggregationStageParseNode> make() {
        return std::make_unique<BadDesugarArrayElementsParseNode>();
    }
};

class ParseNodeVTableTest : public unittest::Test {
public:
    // This special handle class is only used within this fixture so that we can unit test the
    // assertVTableConstraints functionality of the handle.
    class TestParseNodeVTableHandle
        : public extension::host_adapter::ExtensionAggregationStageParseNodeHandle {
    public:
        TestParseNodeVTableHandle(
            absl::Nonnull<::MongoExtensionAggregationStageParseNode*> parseNode)
            : extension::host_adapter::ExtensionAggregationStageParseNodeHandle(parseNode) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

class AstNodeVTableTest : public unittest::Test {
public:
    class TestAstNodeVTableHandle
        : public extension::host_adapter::ExtensionAggregationStageAstNodeHandle {
    public:
        TestAstNodeVTableHandle(absl::Nonnull<::MongoExtensionAggregationStageAstNode*> astNode)
            : extension::host_adapter::ExtensionAggregationStageAstNodeHandle(astNode) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

TEST(AggregationStageTest, DesugarStaticDescriptorTest) {
    auto matchLimitDesugarStaticDescriptor = extension::sdk::ExtensionAggregationStageDescriptor{
        DesugarAsMatchAndLimitDescriptor::make()};
    auto handle = extension::host_adapter::ExtensionAggregationStageDescriptorHandle{
        &matchLimitDesugarStaticDescriptor};

    ASSERT_EQUALS(handle.getName(), DesugarAsMatchAndLimitDescriptor::kStageName);
    ASSERT_EQUALS(handle.getType(), ::MongoExtensionAggregationStageType::kDesugar);
}

TEST(AggregationStageTest, MatchLimitDesugarExpansionSucceedsTest) {
    auto matchLimitDesugarParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(
            DesugarAsMatchAndLimitParseNode::make());
    auto handle = extension::host_adapter::ExtensionAggregationStageParseNodeHandle{
        matchLimitDesugarParseNode.release()};

    auto vec = handle.getExpandedPipelineVec();
    ASSERT_EQ(vec.size(), 2U);
    ASSERT_TRUE(vec[0].hasField("$match"));
    ASSERT_TRUE(vec[1].hasField("$limit"));
    ASSERT_EQ(vec[1].getIntField("$limit"), 1) << vec[1].toString();
}

TEST(AggregationStageTest, EmptyDesugarExpansionSucceedsTest) {
    auto emptyDesugarParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(
            DesugarToEmptyParseNode::make());
    auto handle = extension::host_adapter::ExtensionAggregationStageParseNodeHandle{
        emptyDesugarParseNode.release()};

    auto vec = handle.getExpandedPipelineVec();
    ASSERT_EQ(vec.size(), 0U);
}

TEST(AggregationStageTest, BadArrayElementsDesugarExpansionFails) {
    auto badArrayElementsDesugarParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(
            BadDesugarArrayElementsParseNode::make());
    auto handle = extension::host_adapter::ExtensionAggregationStageParseNodeHandle{
        badArrayElementsDesugarParseNode.release()};

    ASSERT_THROWS_CODE(handle.getExpandedPipelineVec(), DBException, ErrorCodes::TypeMismatch);
}

DEATH_TEST_F(ParseNodeVTableTest, InvalidParseNodeVTableFailsGetQueryShape, "10977601") {
    auto noOpParseNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageParseNode>(NoOpParseNode::make());
    auto handle = TestParseNodeVTableHandle{noOpParseNode.release()};

    auto vtable = handle.vtable();
    vtable.get_query_shape = nullptr;
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

TEST(AggregationStageTest, NoOpAggregationStageAstNodeTest) {
    auto noOpAggregationStageAstNode =
        std::make_unique<extension::sdk::ExtensionAggregationStageAstNode>(
            NoOpAggregationStageAstNode::make());
    auto handle = extension::host_adapter::ExtensionAggregationStageAstNodeHandle{
        noOpAggregationStageAstNode.release()};

    [[maybe_unused]] auto logicalStageHandle = handle.bind();
}

DEATH_TEST_F(AstNodeVTableTest, InvalidAstNodeVTable, "11113700") {
    auto noOpAstNode = std::make_unique<extension::sdk::ExtensionAggregationStageAstNode>(
        NoOpAggregationStageAstNode::make());
    auto handle = TestAstNodeVTableHandle{noOpAstNode.release()};

    auto vtable = handle.vtable();
    vtable.bind = nullptr;
    handle.assertVTableConstraints(vtable);
};
}  // namespace
}  // namespace mongo
