// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

#include <string_view>

namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * $trackDepsSource is a source stage used to test apply_pipeline_suffix_dependencies.
 *
 * Arguments (all optional):
 *   meta: <string>  — a metadata field name to track via needsMetadata()
 *   var:  <string>  — a builtin variable name to track via needsVariable()
 *
 * Emits one document:
 *   {_id: 0,
 *    neededMeta: <bool>,
 *    neededVar: <bool>,
 *    neededWholeDoc: <bool>,
 *    neededFields: <array|null>}
 * reflecting whether the downstream pipeline referenced the specified metadata field, variable,
 * or the full document, and which specific fields are needed.
 */

class TrackDepsExecStage : public sdk::ExecAggStageSource {
public:
    TrackDepsExecStage(std::string_view stageName,
                       bool neededMeta,
                       bool neededVar,
                       bool neededWholeDoc,
                       boost::optional<BSONObj> neededFields)
        : sdk::ExecAggStageSource(stageName),
          _neededMeta(neededMeta),
          _neededVar(neededVar),
          _neededWholeDoc(neededWholeDoc),
          _neededFields(std::move(neededFields)) {}

    extension::ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle&,
                                              ::MongoExtensionExecAggStage*) override {
        if (_emitted) {
            return extension::ExtensionGetNextResult::eof();
        }
        _emitted = true;
        BSONObjBuilder builder;
        builder.append("_id", 0);
        builder.appendBool("neededMeta", _neededMeta);
        builder.appendBool("neededVar", _neededVar);
        builder.appendBool("neededWholeDoc", _neededWholeDoc);
        if (_neededFields) {
            builder.appendArray("neededFields", *_neededFields);
        } else {
            builder.appendNull("neededFields");
        }
        return extension::ExtensionGetNextResult::advanced(
            extension::ExtensionBSONObj::makeAsByteBuf(builder.obj()));
    }

    void open() override {}
    void reopen() override {}
    void close() override {}

    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity) const override {
        return BSONObj();
    }

private:
    const bool _neededMeta;
    const bool _neededVar;
    const bool _neededWholeDoc;
    const boost::optional<BSONObj> _neededFields;
    bool _emitted = false;
};

class TrackDepsLogicalStage : public sdk::LogicalAggStage {
public:
    TrackDepsLogicalStage(std::string_view stageName, const BSONObj& args)
        : sdk::LogicalAggStage(stageName),
          _meta(args["meta"].str()),
          _var(args["var"].str()),
          _neededMeta(args["neededMeta"].booleanSafe()),
          _neededVar(args["neededVar"].booleanSafe()),
          _neededWholeDoc(args["neededWholeDoc"].booleanSafe()) {
        if (auto neededFieldsElem = args["neededFields"]) {
            if (!neededFieldsElem.isNull()) {
                _neededFields = neededFieldsElem.Obj().getOwned();
            }
        }
    }

    BSONObj serialize() const override {
        BSONObjBuilder spec;
        if (!_meta.empty())
            spec.append("meta", _meta);
        if (!_var.empty())
            spec.append("var", _var);
        spec.appendBool("neededMeta", _neededMeta);
        spec.appendBool("neededVar", _neededVar);
        spec.appendBool("neededWholeDoc", _neededWholeDoc);
        if (_neededFields) {
            spec.appendArray("neededFields", *_neededFields);
        } else {
            spec.appendNull("neededFields");
        }
        return BSON(_name << spec.obj());
    }

    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity) const override {
        return serialize();
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<TrackDepsExecStage>(
            _name, _neededMeta, _neededVar, _neededWholeDoc, _neededFields);
    }

    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<TrackDepsLogicalStage>(_name, serialize()[_name].Obj());
    }

    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        extension::sdk::DistributedPlanLogic dpl;
        {
            std::vector<extension::VariantDPLHandle> pipeline;
            pipeline.emplace_back(extension::LogicalAggStageHandle{
                new sdk::ExtensionLogicalAggStageAdapter(clone())});
            dpl.shardsPipeline = sdk::DPLArrayContainer(std::move(pipeline));
        }
        return dpl;
    }

    void applyPipelineSuffixDependencies(
        const extension::PipelineDependenciesHandle& deps) override {
        if (!_meta.empty())
            _neededMeta = deps->needsMetadata(_meta);
        if (!_var.empty())
            _neededVar = deps->needsVariable(_var);
        _neededWholeDoc = deps->needsWholeDocument();
        _neededFields = deps->getNeededFields();
    }

private:
    const std::string _meta;
    const std::string _var;
    bool _neededMeta;
    bool _neededVar;
    bool _neededWholeDoc;
    boost::optional<BSONObj> _neededFields;
};

class TrackDepsAstNode : public sdk::TestAstNode<TrackDepsLogicalStage> {
public:
    TrackDepsAstNode(std::string_view stageName, const BSONObj& arguments)
        : sdk::TestAstNode<TrackDepsLogicalStage>(stageName, arguments) {}

    BSONObj getProperties() const override {
        extension::MongoExtensionStaticProperties properties;
        properties.setPosition(extension::MongoExtensionPositionRequirementEnum::kFirst);
        properties.setRequiresInputDocSource(false);
        properties.setAllowedInFacet(false);
        BSONObjBuilder builder;
        properties.serialize(&builder);
        return builder.obj();
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<TrackDepsAstNode>(getName(), _arguments);
    }
};

DEFAULT_PARSE_NODE(TrackDeps)

using TrackDepsSourceStageDescriptor =
    sdk::TestStageDescriptor<"$trackDepsSource", TrackDepsParseNode>;

/**
 * $trackDepsTransform is a transform stage that overrides applyPipelineSuffixDependencies.
 * It verifies that applyPipelineSuffixDependencies is not invoked for transform stages.
 *
 * The stage definition must be empty: {$trackDepsTransform: {}}
 *
 * The stage is a pure passthrough at execution time. Whether applyPipelineSuffixDependencies
 * was invoked is observable via the serialized/explain output: {depsCallbackCalled: <bool>}.
 */

class TrackDepsTransformLogicalStage : public sdk::LogicalAggStage {
public:
    TrackDepsTransformLogicalStage(std::string_view stageName, const BSONObj& args)
        : sdk::LogicalAggStage(stageName),
          _depsCallbackCalled(args["depsCallbackCalled"].booleanSafe()) {}

    BSONObj serialize() const override {
        return BSON(_name << BSON("depsCallbackCalled" << _depsCallbackCalled));
    }

    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity) const override {
        return serialize();
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<sdk::TestExecStage>(_name, BSONObj());
    }

    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        auto cloned = std::make_unique<TrackDepsTransformLogicalStage>(_name, BSONObj());
        cloned->_depsCallbackCalled = _depsCallbackCalled;
        return cloned;
    }

    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        return boost::none;
    }

    void applyPipelineSuffixDependencies(
        const extension::PipelineDependenciesHandle& deps) override {
        _depsCallbackCalled = true;
    }

private:
    bool _depsCallbackCalled = false;
};

DEFAULT_AST_NODE(TrackDepsTransform)
DEFAULT_PARSE_NODE(TrackDepsTransform)

using TrackDepsTransformStageDescriptor =
    sdk::TestStageDescriptor<"$trackDepsTransform", TrackDepsTransformParseNode>;

class TrackDepsExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<TrackDepsSourceStageDescriptor>(portal);
        _registerStage<TrackDepsTransformStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(TrackDepsExtension)
DEFINE_GET_EXTENSION()
