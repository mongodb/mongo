/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/stage_builder/sbe/analysis.h"

namespace mongo::stage_builder {
class PlanStageSlots;
struct ProjectActions;

using ProjectActionType = sbe::MakeObjSpec::ActionType;

class ProjectAction {
public:
    struct Keep {
        Keep clone() const {
            return Keep{};
        }
    };

    struct Drop {
        Drop clone() const {
            return Drop{};
        }
    };

    struct SetArg {
        SetArg clone() const {
            return SetArg{arg.clone()};
        }
        SbExpr arg;
    };

    struct AddArg {
        AddArg clone() const {
            return AddArg{arg.clone()};
        }
        SbExpr arg;
    };

    struct LambdaArg {
        LambdaArg clone() const {
            return LambdaArg{bodyExpr.clone(), frameId, returnsNothingOnMissingInput};
        }

        SbExpr bodyExpr;
        sbe::FrameId frameId;
        bool returnsNothingOnMissingInput;
    };

    struct MakeObj {
        MakeObj clone() const;

        std::unique_ptr<ProjectActions> projActions;
    };

    using VariantType = std::variant<Keep, Drop, SetArg, AddArg, LambdaArg, MakeObj>;

    ProjectAction() = default;

    ProjectAction(Keep) : _data(Keep{}) {}
    ProjectAction(Drop) : _data(Drop{}) {}
    ProjectAction(SetArg setArg) : _data(std::move(setArg)) {}
    ProjectAction(AddArg addArg) : _data(std::move(addArg)) {}
    ProjectAction(LambdaArg lambdaArg) : _data(std::move(lambdaArg)) {}
    ProjectAction(MakeObj makeObj) : _data(std::move(makeObj)) {}

    ProjectAction clone() const;

    ProjectActionType type() const;

    VariantType& getData() {
        return _data;
    }
    const VariantType& getData() const {
        return _data;
    }

    bool isKeep() const {
        return holds_alternative<Keep>(_data);
    }
    bool isDrop() const {
        return holds_alternative<Drop>(_data);
    }
    bool isSetArg() const {
        return holds_alternative<SetArg>(_data);
    }
    bool isAddArg() const {
        return holds_alternative<AddArg>(_data);
    }
    bool isLambdaArg() const {
        return holds_alternative<LambdaArg>(_data);
    }
    bool isMakeObj() const {
        return holds_alternative<MakeObj>(_data);
    }

    SetArg& getSetArg() {
        return get<SetArg>(_data);
    }
    const SetArg& getSetArg() const {
        return get<SetArg>(_data);
    }
    AddArg& getAddArg() {
        return get<AddArg>(_data);
    }
    const AddArg& getAddArg() const {
        return get<AddArg>(_data);
    }
    LambdaArg& getLambdaArg() {
        return get<LambdaArg>(_data);
    }
    const LambdaArg& getLambdaArg() const {
        return get<LambdaArg>(_data);
    }
    MakeObj& getMakeObj() {
        return get<MakeObj>(_data);
    }
    const MakeObj& getMakeObj() const {
        return get<MakeObj>(_data);
    }

private:
    VariantType _data;
};

/**
 * This class represents a high-level plan for how to apply a projection. The plan is encoded
 * as a set of actions, with each action being represented by the 'ProjectAction' class.
 *
 * The ProjectActions class can be used to build a MakeObjSpec object, which in turn can be
 * used with the makeBsonObj() VM function.
 *
 * While this class closely mirrors MakeObjSpec, it has some important differences. First, this
 * class is mutable, whereas MakeObjSpec is immutable. Also, this class holds the corresponding
 * SbExprs for any SetArg, AddArg, and LambdaArg actions, vs. the MakeObjSpec class which does not
 * contain SbExprs and instead only contains "arg indexes" for SetArg/AddArg/LambdaArg actions.
 */
struct ProjectActions {
    using NonObjInputBehavior = sbe::MakeObjSpec::NonObjInputBehavior;

    ProjectActions() = default;

    ProjectActions(FieldListScope fieldsScope,
                   std::vector<std::string> fields,
                   std::vector<ProjectAction> actions,
                   NonObjInputBehavior nonObjInputBehavior = NonObjInputBehavior::kNewObj,
                   boost::optional<int32_t> traversalDepth = boost::none)
        : fieldsScope(fieldsScope),
          fields(std::move(fields)),
          actions(std::move(actions)),
          nonObjInputBehavior(nonObjInputBehavior),
          traversalDepth(traversalDepth) {}

    bool operator==(const ProjectActions& other) const = delete;

    ProjectActions clone() const {
        std::vector<ProjectAction> clonedActions;

        clonedActions.reserve(actions.size());
        for (const auto& action : actions) {
            clonedActions.emplace_back(action.clone());
        }

        return ProjectActions{
            fieldsScope, fields, std::move(clonedActions), nonObjInputBehavior, traversalDepth};
    }

    bool isNoop() const {
        return fields.empty() && fieldsScope == FieldListScope::kOpen &&
            nonObjInputBehavior == NonObjInputBehavior::kReturnInput;
    }

    FieldListScope fieldsScope = FieldListScope::kOpen;
    std::vector<std::string> fields;
    std::vector<ProjectAction> actions;
    NonObjInputBehavior nonObjInputBehavior = NonObjInputBehavior::kNewObj;
    boost::optional<int32_t> traversalDepth = boost::none;
};

/**
 * This function takes a PlanStageSlots object ('slots') and a projection (specified by 'projType',
 * 'paths', 'nodes', and 'traversalDepth') and produces a plan for how to apply the projection,
 * represented as a pair of ProjectActions.
 *
 * For most projections, the entire plan can be represented using a single ProjectionActions object,
 * in which case the second component of the pair will be boost::none.
 *
 * For projections containing the $slice operator, the entire plan is split up into two parts: the
 * first ProjectActions object will handle everything excluding the $slice operations, and then the
 * second ProjectActions object do whatever is needed for the $slice operations.
 */
std::pair<ProjectActions, boost::optional<ProjectActions>> evaluateProjection(
    StageBuilderState& state,
    projection_ast::ProjectType projType,
    std::vector<std::string> paths,
    std::vector<ProjectNode> nodes,
    const PlanStageSlots* slots,
    boost::optional<int32_t> traversalDepth);

/**
 * This function takes a PlanStageSlots object ('slots') and a FieldEffects ('effects') and produces
 * a plan for how to apply 'effects' to an object, retrieving the new values for each field from
 * 'slots'.
 */
ProjectActions evaluateFieldEffects(StageBuilderState& state,
                                    const FieldEffects& effects,
                                    const PlanStageSlots& slots);

/**
 * This function takes a plan for applying a projection ('pa') and an input object ('expr') and it
 * returns an expression that produces the updated object.
 */
SbExpr generateObjectExpr(StageBuilderState& state,
                          ProjectActions pa,
                          SbExpr expr,
                          bool shouldProduceBson = true);

/**
 * This function takes a plan for applying a projection ('pa') to an entire object, the name of
 * an individual field ('singleField'), and the input value for that field ('expr'), and it returns
 * an expression that applies the applicable parts of the projection to the specified field and
 * produces the updated value for that field.
 */
SbExpr generateSingleFieldExpr(StageBuilderState& state,
                               ProjectActions pa,
                               SbExpr expr,
                               const std::string& singleField,
                               bool shouldProduceBson = true);

/**
 * Generates an SbExpr applies a projection to a document. The 'inputExpr' parameter provides the
 * input document. 'slots' can optionaly be provided as well so that generateExpression() can make
 * use of kField slots when appropriate.
 */
SbExpr generateProjection(StageBuilderState& state,
                          const projection_ast::Projection* projection,
                          SbExpr inputExpr,
                          const PlanStageSlots* slots = nullptr,
                          boost::optional<int32_t> traversalDepth = boost::none,
                          bool shouldProduceBson = true);

SbExpr generateProjection(StageBuilderState& state,
                          projection_ast::ProjectType projType,
                          std::vector<std::string> paths,
                          std::vector<ProjectNode> nodes,
                          SbExpr inputExpr,
                          const PlanStageSlots* slots = nullptr,
                          boost::optional<int32_t> traversalDepth = boost::none,
                          bool shouldProduceBson = true);

/**
 * Generates an SbExpr that applies the applicable parts of a projection to a single field (as given
 * by 'singleField') from a document. The 'inputExpr' parameter provides the input value for the
 * specified field. 'slots' can optionaly be provided as well so that generateExpression() can make
 * use of kField slots when appropriate.
 */
SbExpr generateSingleFieldProjection(StageBuilderState& state,
                                     const projection_ast::Projection* projection,
                                     SbExpr inputExpr,
                                     const PlanStageSlots* slots,
                                     const std::string& singleField,
                                     boost::optional<int32_t> traversalDepth = boost::none,
                                     bool shouldProduceBson = true);

SbExpr generateSingleFieldProjection(StageBuilderState& state,
                                     projection_ast::ProjectType projType,
                                     const std::vector<std::string>& paths,
                                     const std::vector<ProjectNode>& nodes,
                                     SbExpr inputExpr,
                                     const PlanStageSlots* slots,
                                     const std::string& singleField,
                                     boost::optional<int32_t> traversalDepth = boost::none,
                                     bool shouldProduceBson = true);

/**
 * Generates an SbExpr applies 'effects' to a document, retrieving the new values for each field
 * from 'slots'. The 'inputExpr' parameter provides the input document.
 */
SbExpr generateProjectionFromEffects(StageBuilderState& state,
                                     const FieldEffects& effects,
                                     SbExpr inputExpr,
                                     const PlanStageSlots& slots,
                                     bool shouldProduceBson = true);
}  // namespace mongo::stage_builder
