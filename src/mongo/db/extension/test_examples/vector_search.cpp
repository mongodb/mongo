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
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

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
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<sdk::shared_test_stages::TransformAggStageAstNode>(
                "$_extensionVectorSearch", _arguments)));
        return expanded;
    }

    mongo::BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return mongo::BSONObj();
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
