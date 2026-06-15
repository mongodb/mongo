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
