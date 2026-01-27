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

        // TODO SERVER-117259 Change this to false.
        properties.setAllowedInLookup(true);
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

DEFAULT_EXTENSION(Foo)
REGISTER_EXTENSION(FooExtension)
DEFINE_GET_EXTENSION()
