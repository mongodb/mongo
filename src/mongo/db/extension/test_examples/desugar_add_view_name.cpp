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
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/log_util.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/db/extension/shared/get_next_result.h"
namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * Test extension that desugars into $addViewName followed by a stage that returns
 * FirstStageViewApplicationPolicy::kDoNothing.
 */

class AddViewNameExecStage : public sdk::ExecAggStageTransform {
public:
    AddViewNameExecStage(std::string_view stageName,
                         const mongo::BSONObj& arguments,
                         std::string viewName)
        : sdk::ExecAggStageTransform(stageName),
          _arguments(arguments.getOwned()),
          _viewName(std::move(viewName)) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const mongo::extension::sdk::QueryExecutionContextHandle& execCtx,
        ::MongoExtensionExecAggStage* execStage) override {
        auto input = _getSource()->getNext(execCtx.get());

        if (input.code == mongo::extension::GetNextCode::kPauseExecution) {
            return mongo::extension::ExtensionGetNextResult::pauseExecution();
        }
        if (input.code == mongo::extension::GetNextCode::kEOF) {
            return mongo::extension::ExtensionGetNextResult::eof();
        }

        mongo::BSONObj inputDoc = input.resultDocument->getUnownedBSONObj();
        mongo::BSONObjBuilder bob;
        bob.appendElements(inputDoc);
        bob.append("viewName", _viewName);

        auto resultDoc = mongo::extension::ExtensionBSONObj::makeAsByteBuf(bob.done());
        if (input.resultMetadata.has_value()) {
            return mongo::extension::ExtensionGetNextResult::advanced(
                std::move(resultDoc),
                mongo::extension::ExtensionBSONObj::makeAsByteBuf(
                    input.resultMetadata->getUnownedBSONObj()));
        } else {
            return mongo::extension::ExtensionGetNextResult::advanced(std::move(resultDoc));
        }
    }

    void open() override {}
    void reopen() override {}
    void close() override {}

    mongo::BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        mongo::BSONObjBuilder bob;
        bob.append("viewName", _viewName);
        return bob.obj();
    }

    std::string getViewName() const {
        return _viewName;
    }

private:
    const mongo::BSONObj _arguments;
    const std::string _viewName;
};

class AddViewNameLogicalStage : public sdk::LogicalAggStage {
public:
    AddViewNameLogicalStage(std::string_view stageName,
                            const mongo::BSONObj& arguments,
                            std::string viewName)
        : sdk::LogicalAggStage(stageName),
          _arguments(arguments.getOwned()),
          _viewName(std::move(viewName)) {}

    mongo::BSONObj serialize() const override {
        mongo::BSONObjBuilder bob;
        mongo::BSONObjBuilder stageBuilder;
        stageBuilder.appendElements(_arguments);
        // Include view name in serialization if it was set via bindViewInfo() so that the shard
        // receives BSON with the view info after the view has been handled on the router.
        if (!_viewName.empty()) {
            stageBuilder.append("_viewName", _viewName);
        }
        bob.append(_name, stageBuilder.obj());
        return bob.obj();
    }

    mongo::BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        mongo::BSONObjBuilder bob;
        bob.append(_name, _arguments);
        return bob.obj();
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<AddViewNameExecStage>(_name, _arguments, _viewName);
    }

    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        return boost::none;
    }

    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<AddViewNameLogicalStage>(_name, _arguments, _viewName);
    }

private:
    const mongo::BSONObj _arguments;
    const std::string _viewName;
};

class AddViewNameAstNode : public sdk::AggStageAstNode {
public:
    AddViewNameAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::AggStageAstNode(stageName), _arguments([&]() {
              // Extract view name from BSON if it was serialized (e.g., when coming from mongos).
              // Store the original arguments without _viewName (which is an internal field).
              mongo::BSONObjBuilder argsBuilder;
              for (auto elem : arguments) {
                  if (elem.fieldNameStringData() != "_viewName") {
                      argsBuilder.append(elem);
                  }
              }
              return argsBuilder.obj();
          }()) {
        // Extract _viewName separately if it exists.
        auto elem = arguments.getField("_viewName");
        if (!elem.eoo() && elem.type() == BSONType::string) {
            _viewName = elem.str();
        }
    }

    void bindViewInfo(std::string_view viewName) const override {
        _viewName = std::string(viewName);
    }

    MongoExtensionFirstStageViewApplicationPolicy getFirstStageViewApplicationPolicy()
        const override {
        return MongoExtensionFirstStageViewApplicationPolicy::kDoNothing;
    }

    std::unique_ptr<sdk::LogicalAggStage> bind(
        const ::MongoExtensionCatalogContext& catalogContext) const override {
        return std::make_unique<AddViewNameLogicalStage>(getName(), _arguments, _viewName);
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        auto cloned = std::make_unique<AddViewNameAstNode>(getName(), _arguments);
        cloned->_viewName = _viewName;
        return cloned;
    }

private:
    const mongo::BSONObj _arguments;
    mutable std::string _viewName;
};

DEFAULT_PARSE_NODE(AddViewName)

using AddViewNameStageDescriptor =
    sdk::TestStageDescriptor<"$addViewName", AddViewNameParseNode, false>;

DEFAULT_EXEC_STAGE(DoNothingViewPolicy)
DEFAULT_LOGICAL_STAGE(DoNothingViewPolicy)

class DoNothingViewPolicyAstNode : public sdk::AggStageAstNode {
public:
    DoNothingViewPolicyAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::AggStageAstNode(stageName), _arguments(arguments.getOwned()) {}

    MongoExtensionFirstStageViewApplicationPolicy getFirstStageViewApplicationPolicy()
        const override {
        return MongoExtensionFirstStageViewApplicationPolicy::kDoNothing;
    }

    std::unique_ptr<sdk::LogicalAggStage> bind(
        const ::MongoExtensionCatalogContext& catalogContext) const override {
        return std::make_unique<DoNothingViewPolicyLogicalStage>(getName(), _arguments);
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<DoNothingViewPolicyAstNode>(getName(), _arguments);
    }

private:
    const mongo::BSONObj _arguments;
};

DEFAULT_PARSE_NODE(DoNothingViewPolicy)

using DoNothingViewPolicyStageDescriptor =
    sdk::TestStageDescriptor<"$doNothingViewPolicy", DoNothingViewPolicyParseNode, true>;

class DesugarAddViewNameParseNode : public sdk::AggStageParseNode {
public:
    DesugarAddViewNameParseNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::AggStageParseNode(stageName), _arguments(arguments.getOwned()) {}

    size_t getExpandedSize() const override {
        return 2;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(2);

        // First stage: $addViewName (as ExtensionAggStageParseNode)
        expanded.emplace_back(new sdk::ExtensionAggStageParseNode(
            std::make_unique<AddViewNameParseNode>("$addViewName", BSONObj())));

        // Second stage: DoNothingViewPolicy stage that returns kDoNothing
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<DoNothingViewPolicyAstNode>("$doNothingViewPolicy", _arguments)));

        return expanded;
    }

    mongo::BSONObj getQueryShape(const sdk::QueryShapeOptsHandle& ctx) const override {
        return BSON(_name << _arguments);
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<DesugarAddViewNameParseNode>(_name, _arguments);
    }

private:
    const mongo::BSONObj _arguments;
};

using DesugarAddViewNameStageDescriptor =
    sdk::TestStageDescriptor<"$desugarAddViewName", DesugarAddViewNameParseNode, true>;

class DesugarAddViewNameExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<DesugarAddViewNameStageDescriptor>(portal);
        _registerStage<DoNothingViewPolicyStageDescriptor>(portal);
        _registerStage<AddViewNameStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(DesugarAddViewNameExtension)
DEFINE_GET_EXTENSION()
