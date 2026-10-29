// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::extension::sdk::shared_test_stages {

constexpr char kTransformName[] = "$transformStage";

/**
 * This file defines basic classes for all parts of the query lifecyle for a transform stage. Custom
 * stages may parse, expand, etc to a transform class at any point in their lifecycle when custom
 * behavior has completed.
 *
 * Note: transform classes provide default constructors and make() methods to support easy
 * construction in unit tests, but the non-default constructor should always be used in integration
 * tests or any setting where the end-to-end query lifecycle is being exercised.
 */

class TransformExecAggStage : public TestExecStage {
public:
    TransformExecAggStage() : TestExecStage(kTransformName, mongo::BSONObj()) {}

    TransformExecAggStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : TestExecStage(stageName, arguments) {}

    static std::unique_ptr<sdk::ExecAggStageBase> make() {
        return std::make_unique<TransformExecAggStage>();
    }
};

class TransformLogicalAggStage : public TestLogicalStage<TransformExecAggStage> {
public:
    TransformLogicalAggStage() : TestLogicalStage(kTransformName, mongo::BSONObj()) {}

    TransformLogicalAggStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : TestLogicalStage(stageName, arguments) {}

    std::unique_ptr<extension::sdk::LogicalAggStage> clone() const override {
        return std::make_unique<TransformLogicalAggStage>(_name, _arguments);
    }

    static std::unique_ptr<sdk::LogicalAggStage> make() {
        return std::make_unique<TransformLogicalAggStage>();
    }
};

class TransformAggStageAstNode : public TestAstNode<TransformLogicalAggStage> {
public:
    TransformAggStageAstNode() : TestAstNode(kTransformName, mongo::BSONObj()) {}

    TransformAggStageAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : TestAstNode(stageName, arguments) {}

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<TransformAggStageAstNode>(getName(), _arguments);
    }

    static std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<TransformAggStageAstNode>();
    }
};

class TransformAggStageParseNode : public TestParseNode<TransformAggStageAstNode> {
public:
    TransformAggStageParseNode() : TestParseNode(kTransformName, mongo::BSONObj()) {}

    TransformAggStageParseNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : TestParseNode(stageName, arguments) {}

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<TransformAggStageParseNode>(getName(), _arguments);
    }

    static std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<TransformAggStageParseNode>();
    }
};

class TransformAggStageDescriptor
    : public TestStageDescriptor<kTransformName, TransformAggStageParseNode> {
public:
    void validate(const BSONObj& arguments) const override {
        uassert(10596407,
                "Failed to parse $transformStage, missing boolean field \"foo\"",
                arguments.hasField("foo") && arguments.getField("foo").isBoolean());
    }

    static std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<TransformAggStageDescriptor>();
    }
};

constexpr char kInternalTransformName[] = "$_internalTransformStage";

/**
 * An internal-only variant of the transform stage: reuses the transform chain but declares itself
 * internal-only so it is rejected from user pipelines.
 */
class InternalTransformAggStageDescriptor
    : public TestStageDescriptor<kInternalTransformName, TransformAggStageParseNode> {
public:
    ::MongoExtensionClientType getClientType() const override {
        return ::kMongoExtensionClientTypeInternal;
    }

    static std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<InternalTransformAggStageDescriptor>();
    }
};

}  // namespace mongo::extension::sdk::shared_test_stages
