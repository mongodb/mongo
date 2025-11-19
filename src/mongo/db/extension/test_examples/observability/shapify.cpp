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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/query_shape_opts_handle.h"
#include "mongo/db/extension/sdk/test_extension_util.h"

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
static constexpr std::string kShapifyStageName = "$shapify";

/**
 * Extension desugar stage with a non-default query shape implementation. Syntax:
 *
 * {$shapifyDesugar: {random object>}}
 *
 * ShapifyDesugar will expand into multiple $shapify stages for query shape testing purposes.
 */
static constexpr std::string kShapifyDesugarStageName = "$shapifyDesugar";

class ShapifyExecAggStage : public sdk::ExecAggStage {
public:
    ShapifyExecAggStage() : sdk::ExecAggStage(kShapifyDesugarStageName) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& execCtx,
        const MongoExtensionExecAggStage* execStage,
        ::MongoExtensionGetNextRequestType requestType) override {
        return mongo::extension::ExtensionGetNextResult::pauseExecution();
    }

    void open() override {}

    void reopen() override {}

    void close() override {}
};

class ShapifyLogicalStage : public sdk::LogicalAggStage {
public:
    ShapifyLogicalStage(BSONObj input) : _input(input) {}

    BSONObj serialize() const override {
        return BSON(kShapifyStageName << _input);
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON(kShapifyStageName << _input);
    }

    std::unique_ptr<extension::sdk::ExecAggStage> compile() const override {
        return std::make_unique<ShapifyExecAggStage>();
    }

private:
    BSONObj _input;
};

class ShapifyAstNode : public sdk::AggStageAstNode {
public:
    ShapifyAstNode(BSONObj input) : sdk::AggStageAstNode(kShapifyStageName), _input(input) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<ShapifyLogicalStage>(_input);
    }

private:
    BSONObj _input;
};

class ShapifyParseNode : public sdk::AggStageParseNode {
public:
    ShapifyParseNode(BSONObj input) : sdk::AggStageParseNode(kShapifyStageName), _input(input) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(getExpandedSize());
        expanded.emplace_back(
            new sdk::ExtensionAggStageAstNode(std::make_unique<ShapifyAstNode>(_input)));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        sdk::QueryShapeOptsHandle ctxHandle(ctx);
        BSONObjBuilder builder;

        buildQueryShape(ctxHandle, _input, builder);

        return BSON(kShapifyStageName << builder.obj());
    }

private:
    void buildQueryShape(sdk::QueryShapeOptsHandle ctxHandle,
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

                builder.append(fieldName, ctxHandle.serializeFieldPath(elt.String()));
            } else if (str::startsWith(fieldName, "ident_")) {
                sdk_uassert(11173602,
                            (str::stream() << "ident field must be of type string, but found type "
                                           << elt.type()),
                            elt.type() == BSONType::string);

                builder.append(fieldName, ctxHandle.serializeIdentifier(elt.String()));
            } else {
                ctxHandle.appendLiteral(builder, fieldName, elt);
            }
        }
    }

    BSONObj _input;
};

class ShapifyStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = "$shapify";

    ShapifyStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName);

        return std::make_unique<ShapifyParseNode>(stageBson[kStageName].Obj().getOwned());
    }
};

class ShapifyDesugarParseNode : public sdk::AggStageParseNode {
public:
    ShapifyDesugarParseNode(BSONObj input)
        : sdk::AggStageParseNode(kShapifyDesugarStageName), _input(input) {}

    size_t getExpandedSize() const override {
        return 3;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(getExpandedSize());
        BSONObj shapifySpec =
            BSON(kShapifyStageName << BSON("int" << 1 << "ident_name" << "Alice"));
        expanded.emplace_back(
            new sdk::ExtensionAggStageParseNode(std::make_unique<ShapifyParseNode>(shapifySpec)));
        expanded.emplace_back(
            new sdk::ExtensionAggStageParseNode(std::make_unique<ShapifyParseNode>(shapifySpec)));
        expanded.emplace_back(
            new sdk::ExtensionAggStageParseNode(std::make_unique<ShapifyParseNode>(shapifySpec)));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSON(kShapifyDesugarStageName << _input);
    }

private:
    BSONObj _input;
};

class ShapifyDesugarStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kShapifyDesugarStageName);

    ShapifyDesugarStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName);

        return std::make_unique<ShapifyDesugarParseNode>(stageBson[kStageName].Obj().getOwned());
    }
};


class ShapifyExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<ShapifyStageDescriptor>(portal);
        _registerStage<ShapifyDesugarStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(ShapifyExtension)
DEFINE_GET_EXTENSION()
