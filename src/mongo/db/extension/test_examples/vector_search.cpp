// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

#include <string_view>

namespace sdk = mongo::extension::sdk;

/**
 * Parse node for $vectorSearch that desugars into $_extensionVectorSearch.
 */
class VectorSearchParseNode : public sdk::AggStageParseNode {
public:
    VectorSearchParseNode() : sdk::AggStageParseNode("$vectorSearch") {}

    VectorSearchParseNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::AggStageParseNode(stageName), _arguments(arguments.getOwned()) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        // Extension $vectorSearch will also eventually desugar into an ID lookup stage, but it is
        // not necessary for this toy example (only used for testing).
        expanded.reserve(1);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNodeAdapter(
            std::make_unique<sdk::shared_test_stages::TransformAggStageAstNode>(
                "$_extensionVectorSearch", _arguments)));
        return expanded;
    }

    mongo::BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        return mongo::BSONObj();
    }

    mongo::BSONObj toBsonForLog() const override {
        return BSON(_name << _arguments);
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<VectorSearchParseNode>(getName(), _arguments);
    }

private:
    mongo::BSONObj _arguments;
};

/**
 * $vectorSearch is stage used to imitate overriding the existing $vectorSearch implementation
 * with an extension stage. It desugars into $_extensionVectorSearch.
 */
using VectorSearchStageDescriptor =
    sdk::TestStageDescriptor<"$vectorSearch", VectorSearchParseNode>;

/**
 * Stage descriptor for $_extensionVectorSearch. Even though users don't use $_extensionVectorSearch
 * directly, we must register this stage descriptor for the sharded case, where the mongos
 * serializes the pipeline and sends it to the shards, and when merging explain output from shards.
 */
using ExtensionVectorSearchStageDescriptor =
    sdk::TestStageDescriptor<"$_extensionVectorSearch",
                             sdk::shared_test_stages::TransformAggStageParseNode>;

class VectorSearchExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<VectorSearchStageDescriptor>(portal);
        _registerStage<ExtensionVectorSearchStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(VectorSearchExtension)
DEFINE_GET_EXTENSION()
