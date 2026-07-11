// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/query_shape_opts_handle.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

#include <string_view>

namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * Stage with a non-default query shape implementation. Syntax:
 *
 * {$shapify: {<object to shapify>}}
 *
 * Shapify will generate a query shape as follows:
 * - Field names prefixed with "path_" -> field path
 * - Field names prefixed with "ident_" -> identifier
 * - Field names prefixed with "obj_" -> traverse sub object and serialize each field
 * - Default -> literal
 */
class ShapifyParseNode
    : public sdk::TestParseNode<sdk::shared_test_stages::TransformAggStageAstNode> {
public:
    ShapifyParseNode(const BSONObj& input) : ShapifyParseNode("$shapify", input) {}

    ShapifyParseNode(std::string_view stageName, const BSONObj& input)
        : sdk::TestParseNode<sdk::shared_test_stages::TransformAggStageAstNode>(stageName, input) {}

    BSONObj getQueryShape(const sdk::QueryShapeOptsHandle& ctxHandle) const override {
        BSONObjBuilder builder;

        buildQueryShape(ctxHandle, _arguments, builder);

        return BSON(_name << builder.obj());
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<ShapifyParseNode>(getName(), _arguments);
    }

private:
    void buildQueryShape(const sdk::QueryShapeOptsHandle& ctxHandle,
                         BSONObj input,
                         BSONObjBuilder& builder) const {
        for (const auto& elt : input) {
            const auto& fieldName = elt.fieldName();
            if (str::startsWith(fieldName, "obj_")) {
                sdk_uassert(11173600,
                            (str::stream()
                             << "obj field must be of type object, but found type " << elt.type()),
                            elt.type() == BSONType::object);

                BSONObjBuilder subobjBuilder = builder.subobjStart(fieldName);
                buildQueryShape(ctxHandle, elt.Obj(), subobjBuilder);
            } else if (str::startsWith(fieldName, "path_")) {
                sdk_uassert(11173601,
                            (str::stream()
                             << "path field must be of type string, but found type " << elt.type()),
                            elt.type() == BSONType::string);

                builder.append(fieldName, ctxHandle->serializeFieldPath(elt.String()));
            } else if (str::startsWith(fieldName, "ident_")) {
                sdk_uassert(11173602,
                            (str::stream() << "ident field must be of type string, but found type "
                                           << elt.type()),
                            elt.type() == BSONType::string);

                builder.append(fieldName, ctxHandle->serializeIdentifier(elt.String()));
            } else {
                ctxHandle->appendLiteral(builder, fieldName, elt);
            }
        }
    }
};

/**
 * Extension desugar stage with a non-default query shape implementation. Syntax:
 *
 * {$shapifyDesugar: {random object>}}
 *
 * ShapifyDesugar will expand into multiple $shapify stages for query shape testing purposes.
 */
class ShapifyDesugarParseNode
    : public sdk::TestParseNode<sdk::shared_test_stages::TransformAggStageAstNode> {
public:
    ShapifyDesugarParseNode(std::string_view stageName, const BSONObj& input)
        : sdk::TestParseNode<sdk::shared_test_stages::TransformAggStageAstNode>(stageName, input) {}

    size_t getExpandedSize() const override {
        return 3;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(getExpandedSize());
        BSONObj shapifySpec = BSON("int" << 1 << "ident_name" << "Alice");
        expanded.emplace_back(new sdk::ExtensionAggStageParseNodeAdapter(
            std::make_unique<ShapifyParseNode>(shapifySpec)));
        expanded.emplace_back(new sdk::ExtensionAggStageParseNodeAdapter(
            std::make_unique<ShapifyParseNode>(shapifySpec)));
        expanded.emplace_back(new sdk::ExtensionAggStageParseNodeAdapter(
            std::make_unique<ShapifyParseNode>(shapifySpec)));
        return expanded;
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<ShapifyDesugarParseNode>(getName(), _arguments);
    }
};

using ShapifyStageDescriptor = sdk::TestStageDescriptor<"$shapify", ShapifyParseNode>;
using ShapifyDesugarStageDescriptor =
    sdk::TestStageDescriptor<"$shapifyDesugar", ShapifyDesugarParseNode>;

class ShapifyExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<ShapifyStageDescriptor>(portal);
        _registerStage<ShapifyDesugarStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(ShapifyExtension)
DEFINE_GET_EXTENSION()
