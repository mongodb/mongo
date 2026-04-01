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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/db/extension/shared/get_next_result.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * Test extension that validates the view pipeline contains only $match, $addFields, or $set
 * stages. Uasserts if any other stage is present. Also stores the pipeline and references it
 * in getNext() to verify ownership/lifetime (see view_pipeline_validator_extension.js).
 */

namespace {
bool isAllowedViewPipelineStage(StringData stageName) {
    return stageName == "$match" || stageName == "$addFields" || stageName == "$set";
}
}  // namespace

class ValidateViewPipelineExecStage : public sdk::ExecAggStageTransform {
public:
    ValidateViewPipelineExecStage(std::string_view stageName,
                                  const BSONObj& arguments,
                                  std::vector<BSONObj> storedViewPipeline)
        : sdk::ExecAggStageTransform(stageName),
          _arguments(arguments.getOwned()),
          _storedViewPipeline(std::move(storedViewPipeline)) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const mongo::extension::sdk::QueryExecutionContextHandle& execCtx,
        ::MongoExtensionExecAggStage* execStage) override {
        auto input = _getSource()->getNext(execCtx.get());
        if (input.code != extension::GetNextCode::kAdvanced) {
            return input;
        }
        sdk_tassert(11905601, "Expected result document", input.resultDocument.has_value());
        BSONObjBuilder bob(input.resultDocument->getUnownedBSONObj());
        bob.append("storedViewPipelineLength", static_cast<int>(_storedViewPipeline.size()));
        BSONArrayBuilder stagesArrBuilder;
        for (const auto& stage : _storedViewPipeline) {
            if (!stage.isEmpty()) {
                stagesArrBuilder.append(stage.firstElement().fieldNameStringData());
            }
        }
        bob.append("viewPipelineStages", stagesArrBuilder.arr());
        return extension::ExtensionGetNextResult::advanced(
            extension::ExtensionBSONObj::makeAsByteBuf(bob.done()));
    }

    void open() override {}
    void reopen() override {}
    void close() override {}
    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

private:
    const BSONObj _arguments;
    const std::vector<BSONObj> _storedViewPipeline;
};

class ValidateViewPipelineLogicalStage : public sdk::LogicalAggStage {
public:
    ValidateViewPipelineLogicalStage(std::string_view stageName,
                                     const BSONObj& arguments,
                                     std::vector<BSONObj> storedViewPipeline)
        : sdk::LogicalAggStage(stageName),
          _arguments(arguments.getOwned()),
          _storedViewPipeline(std::move(storedViewPipeline)) {}

    BSONObj serialize() const override {
        BSONObjBuilder bob;
        bob.appendElements(_arguments);
        if (!_storedViewPipeline.empty()) {
            BSONArrayBuilder arrBuilder(bob.subarrayStart("_viewPipeline"));
            for (const auto& stage : _storedViewPipeline) {
                arrBuilder.append(stage);
            }
            arrBuilder.done();
        }
        return BSON(_name << bob.done());
    }
    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON(_name << _arguments);
    }
    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<ValidateViewPipelineExecStage>(
            _name, _arguments, _storedViewPipeline);
    }
    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        return boost::none;
    }
    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<ValidateViewPipelineLogicalStage>(
            _name, _arguments, _storedViewPipeline);
    }

private:
    const BSONObj _arguments;
    const std::vector<BSONObj> _storedViewPipeline;
};

class ValidateViewPipelineAstNode : public sdk::AggStageAstNode {
public:
    ValidateViewPipelineAstNode(std::string_view stageName, const BSONObj& arguments)
        : sdk::AggStageAstNode(stageName), _arguments(arguments.getOwned()) {
        initStoredViewPipelineFromSerialized(arguments);
    }

    ValidateViewPipelineAstNode(std::string_view stageName,
                                const BSONObj& arguments,
                                std::vector<BSONObj> storedViewPipeline)
        : sdk::AggStageAstNode(stageName),
          _arguments(arguments.getOwned()),
          _storedViewPipeline(std::move(storedViewPipeline)) {}

    void bindViewInfo(const sdk::ViewInfo& viewInfo) override {
        _storedViewPipeline.clear();
        for (const auto& stage : viewInfo.viewPipeline()) {
            if (stage.isEmpty()) {
                continue;
            }
            StringData stageName = stage.firstElement().fieldNameStringData();
            if (!isAllowedViewPipelineStage(stageName)) {
                sdk_uasserted(11905602,
                              "View pipeline only permits $match, $addFields, and $set stages.");
            }
            _storedViewPipeline.push_back(stage.getOwned());
        }
    }

    std::unique_ptr<sdk::LogicalAggStage> bind(
        const ::MongoExtensionCatalogContext& catalogContext) const override {
        return std::make_unique<ValidateViewPipelineLogicalStage>(
            getName(), _arguments, _storedViewPipeline);
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<ValidateViewPipelineAstNode>(
            getName(), _arguments, _storedViewPipeline);
    }

private:
    void initStoredViewPipelineFromSerialized(const BSONObj& arguments) {
        BSONElement viewPipelineElem = arguments.getField("_viewPipeline");
        if (viewPipelineElem.type() != BSONType::array) {
            return;
        }
        for (const auto& elem : viewPipelineElem.Obj()) {
            if (elem.type() == BSONType::object) {
                _storedViewPipeline.push_back(elem.Obj().getOwned());
            }
        }
    }

    const BSONObj _arguments;
    std::vector<BSONObj> _storedViewPipeline;
};

DEFAULT_PARSE_NODE(ValidateViewPipeline)

/**
 * Custom descriptor: allow {} (user form) or {_viewPipeline: <array>} (serialized form for shards).
 * Must not use ExpectEmptyStageDefinition=true so shards can receive the serialized spec.
 */
class ValidateViewPipelineStageDescriptor
    : public sdk::
          TestStageDescriptor<"$validateViewPipeline", ValidateViewPipelineParseNode, false> {
public:
    void validate(const BSONObj& arguments) const override {
        if (arguments.isEmpty()) {
            return;  // User form: {}
        }
        BSONElement viewPipelineElem = arguments.getField("_viewPipeline");
        if (viewPipelineElem.type() == BSONType::array) {
            return;  // Serialized form: {_viewPipeline: [...]}
        }
        sdk_uasserted(11905603,
                      "$validateViewPipeline stage definition must be {} or serialized form");
    }
};

DEFAULT_EXTENSION(ValidateViewPipeline)

REGISTER_EXTENSION(ValidateViewPipelineExtension)
DEFINE_GET_EXTENSION()
