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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"

namespace mongo::extension::sdk::shared_test_stages {

/**
 * Test scaffolding for the Aggregation Extension SDK.
 *
 * Provides aggregation stages and their companion types used by unit tests
 * to exercise the SDK/host plumbing end to end.
 *
 * Referenced by sdk/tests/aggregation_stage_test.cpp and host/document_source_extension_test.cpp.
 */
static constexpr std::string_view kNoOpName = "$noOp";

class NoOpLogicalAggStage : public sdk::LogicalAggStage {
public:
    NoOpLogicalAggStage() {}
};

class NoOpAggStageAstNode : public sdk::AggStageAstNode {
public:
    NoOpAggStageAstNode() : sdk::AggStageAstNode(kNoOpName) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<NoOpAggStageAstNode>();
    }
};

class NoOpAggStageParseNode : public sdk::AggStageParseNode {
public:
    NoOpAggStageParseNode() : sdk::AggStageParseNode(kNoOpName) {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(
            new sdk::ExtensionAggStageAstNode(std::make_unique<NoOpAggStageAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<NoOpAggStageParseNode>();
    }
};

class NoOpAggStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kNoOpName);

    NoOpAggStageDescriptor()
        : sdk::AggStageDescriptor(kStageName, MongoExtensionAggStageType::kNoOp) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {

        uassert(10596406,
                "Failed to parse $noOpExtension, $noOpExtension expects an object.",
                stageBson.hasField(kStageName) && stageBson.getField(kStageName).isABSONObj());
        auto stageDefinition = stageBson.getField(kStageName).Obj();
        uassert(10596407,
                "Failed to parse $noOpExtension, missing boolean field \"foo\"",
                stageDefinition.hasField("foo") && stageDefinition.getField("foo").isBoolean());
        return std::make_unique<NoOpAggStageParseNode>();
    }

    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<NoOpAggStageDescriptor>();
    }
};

}  // namespace mongo::extension::sdk::shared_test_stages
