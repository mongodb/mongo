// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

#include <string_view>

namespace sdk = mongo::extension::sdk;
using namespace mongo;


class FooAstNode : public sdk::TestAstNode<sdk::shared_test_stages::TransformLogicalAggStage> {
public:
    FooAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestAstNode<sdk::shared_test_stages::TransformLogicalAggStage>(stageName,
                                                                              arguments) {}

    mongo::BSONObj getProperties() const override {
        mongo::extension::MongoExtensionStaticProperties properties;
        properties.setAllowedInUnionWith(false);

        // Right now these constraints are hardcoded to 'kNotAllowed' so we want to test that even
        // if a developer tried to enable them in the extension it wouldn't work.

        properties.setAllowedInLookup(false);
        // TODO SERVER-117260 Change this to false.
        properties.setAllowedInFacet(true);

        mongo::BSONObjBuilder builder;
        properties.serialize(&builder);
        return builder.obj();
    }


    std::unique_ptr<AggStageAstNode> clone() const override {
        return std::make_unique<FooAstNode>(_name, _arguments);
    }
};

DEFAULT_PARSE_NODE(Foo);

/**
 * $testFoo is a transform stage. It is not allowed to run in subpipelines.
 *
 * The stage definition must be empty, like {$testFoo: {}}, or it will fail to parse.
 */
using FooStageDescriptor =
    sdk::TestStageDescriptor<"$testFoo", FooParseNode, true /* ExpectEmptyStageDefinition */>;

/**
 * $desugarFoo is a desugar-only stage that expands into $testFoo. Because FooAstNode reports
 * allowedInLookup=false, LiteParsedExpandable::isAllowedInLookupPipeline() returns false via its
 * all_of check, so $desugarFoo is also rejected in $lookup subpipelines. Used to test that the
 * desugar path correctly propagates $lookup restrictions from expanded stages.
 */
class DesugarFooParseNode : public sdk::AggStageParseNode {
public:
    DesugarFooParseNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::AggStageParseNode(stageName), _arguments(arguments.getOwned()) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> out;
        out.reserve(1);
        out.emplace_back(new sdk::ExtensionAggStageAstNodeAdapter(
            std::make_unique<FooAstNode>("$testFoo", BSONObj())));
        return out;
    }

    mongo::BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        return BSON(_name << _arguments);
    }

    mongo::BSONObj toBsonForLog() const override {
        return BSON(_name << _arguments);
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<DesugarFooParseNode>(_name, _arguments);
    }

private:
    const mongo::BSONObj _arguments;
};

using DesugarFooStageDescriptor = sdk::
    TestStageDescriptor<"$desugarFoo", DesugarFooParseNode, true /* ExpectEmptyStageDefinition */>;

class FooExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<FooStageDescriptor>(portal);
        _registerStage<DesugarFooStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(FooExtension)
DEFINE_GET_EXTENSION()
