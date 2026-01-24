/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"
namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * Test extension that disallows views by uasserting in its bindViewInfo implementation.
 */

DEFAULT_EXEC_STAGE(DisallowView)
DEFAULT_LOGICAL_STAGE(DisallowView)

class DisallowViewAstNode : public sdk::AggStageAstNode {
public:
    DisallowViewAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::AggStageAstNode(stageName), _arguments(arguments.getOwned()) {}

    void bindViewInfo(std::string_view viewName) const override {
        std::string message = str::stream()
            << "Stage " << std::string(getName())
            << " does not support views. Attempted to use in view: " << std::string(viewName);
        sdk_uasserted(11507700, message);
    }

    std::unique_ptr<sdk::LogicalAggStage> bind(
        const ::MongoExtensionCatalogContext& catalogContext) const override {
        return std::make_unique<DisallowViewLogicalStage>(getName(), _arguments);
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<DisallowViewAstNode>(getName(), _arguments);
    }

private:
    const mongo::BSONObj _arguments;
};

DEFAULT_PARSE_NODE(DisallowView)

using DisallowViewStageDescriptor =
    sdk::TestStageDescriptor<"$disallowViews", DisallowViewParseNode, true>;

DEFAULT_EXTENSION(DisallowView)

REGISTER_EXTENSION(DisallowViewExtension)
DEFINE_GET_EXTENSION()
