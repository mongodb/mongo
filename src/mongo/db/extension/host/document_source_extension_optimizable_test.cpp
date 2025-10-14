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

#include "mongo/db/extension/host/document_source_extension_optimizable.h"

#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension {

static constexpr std::string_view kFirstStageNotAstName = "$firstStageNotAst";
static constexpr std::string_view kExpandSizeTooBigName = "$expandSizeTooBig";
class FirstStageNotAstParseNode : public sdk::AggStageParseNode {
public:
    static inline const std::string kStageName = std::string(kFirstStageNotAstName);
    FirstStageNotAstParseNode() : sdk::AggStageParseNode(kStageName) {}

    static constexpr size_t kExpansionSize = 1;
    size_t getExpandedSize() const override {
        return kExpansionSize;
    }
    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new sdk::ExtensionAggStageParseNode(
            std::make_unique<sdk::shared_test_stages::NoOpAggStageParseNode>()));
        return expanded;
    }
    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }
};
class FirstStageNotAstStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kFirstStageNotAstName);
    FirstStageNotAstStageDescriptor()
        : sdk::AggStageDescriptor(kStageName, MongoExtensionAggStageType::kNoOp) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        return std::make_unique<FirstStageNotAstParseNode>();
    }
    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<FirstStageNotAstStageDescriptor>();
    }
};
class ExpandSizeTooBigParseNode : public sdk::AggStageParseNode {
public:
    static inline const std::string kStageName = std::string(kExpandSizeTooBigName);
    ExpandSizeTooBigParseNode() : extension::sdk::AggStageParseNode(kStageName) {}

    static constexpr size_t kExpansionSize = 2;
    size_t getExpandedSize() const override {
        return kExpansionSize;
    }
    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new extension::sdk::ExtensionAggStageAstNode(
            sdk::shared_test_stages::NoOpAggStageAstNode::make()));
        expanded.emplace_back(new extension::sdk::ExtensionAggStageAstNode(
            sdk::shared_test_stages::NoOpAggStageAstNode::make()));
        return expanded;
    }
    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }
};
class ExpandSizeTooBigStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kExpandSizeTooBigName);
    ExpandSizeTooBigStageDescriptor()
        : sdk::AggStageDescriptor(kStageName, MongoExtensionAggStageType::kNoOp) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        return std::make_unique<ExpandSizeTooBigParseNode>();
    }
    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<ExpandSizeTooBigStageDescriptor>();
    }
};

class DocumentSourceExtensionOptimizableTest : public AggregationContextFixture {
public:
    DocumentSourceExtensionOptimizableTest() : DocumentSourceExtensionOptimizableTest(_nss) {}
    explicit DocumentSourceExtensionOptimizableTest(NamespaceString nsString)
        : AggregationContextFixture(std::move(nsString)) {};

protected:
    static inline NamespaceString _nss = NamespaceString::createNamespaceString_forTest(
        boost::none, "document_source_extension_optimizable_test");

    sdk::ExtensionAggStageDescriptor _expandSizeTooBigStageDescriptor{
        ExpandSizeTooBigStageDescriptor::make()};
    sdk::ExtensionAggStageDescriptor _firstStageNotAstStageDescriptor{
        FirstStageNotAstStageDescriptor::make()};
    sdk::ExtensionAggStageDescriptor _noOpAggregationStageDescriptor{
        sdk::shared_test_stages::NoOpAggStageDescriptor::make()};
};

DEATH_TEST_F(DocumentSourceExtensionOptimizableTest, expandedSizeNotOneFails, "11164400") {
    [[maybe_unused]] auto extensionOptimizable = host::DocumentSourceExtensionOptimizable(
        ExpandSizeTooBigStageDescriptor::kStageName,
        getExpCtx(),
        DocumentSource::allocateId(ExpandSizeTooBigStageDescriptor::kStageName),
        BSON(ExpandSizeTooBigStageDescriptor::kStageName << BSON("foo" << true)),
        host_connector::AggStageDescriptorHandle(&_expandSizeTooBigStageDescriptor));
}

DEATH_TEST_F(DocumentSourceExtensionOptimizableTest, expandToParseNodeFails, "11164401") {
    [[maybe_unused]] auto extensionOptimizable = host::DocumentSourceExtensionOptimizable(
        FirstStageNotAstStageDescriptor::kStageName,
        getExpCtx(),
        DocumentSource::allocateId(FirstStageNotAstStageDescriptor::kStageName),
        BSON(FirstStageNotAstStageDescriptor::kStageName << BSON("foo" << true)),
        host_connector::AggStageDescriptorHandle(&_firstStageNotAstStageDescriptor));
}

TEST_F(DocumentSourceExtensionOptimizableTest, noOpConstructionSucceeds) {
    auto optimizable = host::DocumentSourceExtensionOptimizable(
        sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName,
        getExpCtx(),
        DocumentSource::allocateId(sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName),
        BSON(sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName << BSON("foo" << true)),
        host_connector::AggStageDescriptorHandle(&_noOpAggregationStageDescriptor));

    ASSERT_EQ(std::string(optimizable.getSourceName()),
              sdk::shared_test_stages::NoOpAggStageDescriptor::kStageName);
}
}  // namespace mongo::extension
