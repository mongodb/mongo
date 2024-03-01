/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/abt/abt_lower.h"

#include <absl/container/inlined_vector.h>
#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/abt/slots_provider.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/exchange.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/merge_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/spool.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/abt/match_expression_visitor.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/utils/path_utils.h"
#include "mongo/db/query/optimizer/utils/reftracker_utils.h"
#include "mongo/db/query/optimizer/utils/strong_alias.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

namespace mongo::optimizer {

static sbe::EExpression::Vector toInlinedVector(
    std::vector<std::unique_ptr<sbe::EExpression>> args) {
    sbe::EExpression::Vector inlined;
    for (auto&& arg : args) {
        inlined.emplace_back(std::move(arg));
    }
    return inlined;
}

std::unique_ptr<sbe::EExpression> VarResolver::operator()(const ProjectionName& name) const {
    if (_slotMap) {
        if (auto it = _slotMap->find(name); it != _slotMap->end()) {
            return sbe::makeE<sbe::EVariable>(it->second);
        }
    }

    if (_lowerFn) {
        return _lowerFn(name);
    }

    return {};
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::optimize(const ABT& n) {
    return algebra::transport<false>(n, *this);
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(const Constant& c) {
    auto [tag, val] = c.get();
    auto [copyTag, copyVal] = sbe::value::copyValue(tag, val);
    sbe::value::ValueGuard guard(copyTag, copyVal);

    auto result = sbe::makeE<sbe::EConstant>(copyTag, copyVal);

    guard.reset();
    return result;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(const Source&) {
    tasserted(6624202, "not yet implemented");
    return nullptr;
}

void SBEExpressionLowering::prepare(const Let& let) {
    // Assign a frame ID for the local variable bound by this Let expression.
    _letMap[&let] = ++_frameCounter;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const Let& let, std::unique_ptr<sbe::EExpression> bind, std::unique_ptr<sbe::EExpression> in) {
    auto it = _letMap.find(&let);
    tassert(6624206, "incorrect let map", it != _letMap.end());
    auto frameId = it->second;
    _letMap.erase(it);

    // ABT let binds only a single variable. When we extend it to support multiple binds then we
    // have to revisit how we map variable names to sbe slot ids.
    return sbe::makeE<sbe::ELocalBind>(frameId, sbe::makeEs(std::move(bind)), std::move(in));
}

void SBEExpressionLowering::prepare(const LambdaAbstraction& lam) {
    // Assign a frame ID for the local variable bound by this LambdaAbstraction.
    _lambdaMap[&lam] = ++_frameCounter;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const LambdaAbstraction& lam, std::unique_ptr<sbe::EExpression> body) {
    auto it = _lambdaMap.find(&lam);
    tassert(6624207, "incorrect lambda map", it != _lambdaMap.end());
    auto frameId = it->second;
    _lambdaMap.erase(it);

    return sbe::makeE<sbe::ELocalLambda>(frameId, std::move(body));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const LambdaApplication&,
    std::unique_ptr<sbe::EExpression> lam,
    std::unique_ptr<sbe::EExpression> arg) {
    // lambda applications are not directly supported by SBE (yet) and must not be present.
    tasserted(6624208, "lambda application is not implemented");
    return nullptr;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(const Variable& var) {
    auto def = _env.getDefinition(var);

    if (!def.definedBy.empty()) {
        // If this variable was defined by a Let expression, use the frame ID that was defined in
        // the prepare() step for the Let.
        if (auto let = def.definedBy.cast<Let>(); let) {
            auto it = _letMap.find(let);
            tassert(6624203, "incorrect let map", it != _letMap.end());

            return sbe::makeE<sbe::EVariable>(it->second, 0, _env.isLastRef(var));
        } else if (auto lam = def.definedBy.cast<LambdaAbstraction>(); lam) {
            // Similarly if the variable was defined by a lambda abstraction, use a frame ID rather
            // than a slot.
            auto it = _lambdaMap.find(lam);
            tassert(6624204, "incorrect lambda map", it != _lambdaMap.end());

            return sbe::makeE<sbe::EVariable>(it->second, 0, _env.isLastRef(var));
        }
    }

    // If variable was not defined in the scope of the local expression via a Let or
    // LambdaAbstraction, it must be a reference that will be in the slotMap.
    if (auto expr = _varResolver(var.name())) {
        return expr;
    }

    tasserted(6624205, str::stream() << "undefined variable: " << var.name());
    return nullptr;
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const BinaryOp& op,
    std::unique_ptr<sbe::EExpression> lhs,
    std::unique_ptr<sbe::EExpression> rhs) {

    if (op.op() == Operations::EqMember) {
        // We directly translate BinaryOp [EqMember] to the SBE function isMember.
        sbe::EExpression::Vector isMemberArgs;
        isMemberArgs.push_back(std::move(lhs));
        isMemberArgs.push_back(std::move(rhs));

        return sbe::makeE<sbe::EFunction>("isMember", std::move(isMemberArgs));
    }

    sbe::EPrimBinary::Op sbeOp = getEPrimBinaryOp(op.op());

    if (sbe::EPrimBinary::isComparisonOp(sbeOp)) {
        boost::optional<sbe::value::SlotId> collatorSlot =
            _providedSlots.getSlotIfExists("collator"_sd);

        auto collationExpr = [&]() -> std::unique_ptr<sbe::EExpression> {
            if (collatorSlot) {
                return sbe::makeE<sbe::EVariable>(*collatorSlot);
            }
            return nullptr;
        };

        switch (_comparisonOpSemantics) {
            case ComparisonOpSemantics::kTypeBracketing:
                // If binary operations are type bracketed, then we can translate this comparison
                // directly to SBE's type bracketed comparison operator.
                return sbe::makeE<sbe::EPrimBinary>(
                    sbeOp, std::move(lhs), std::move(rhs), collationExpr());
            case ComparisonOpSemantics::kTotalOrder:
                // If binary operations have a total order, then we generate the comparison using a
                // cmp3w expression to achieve the desired semantics. For example, a < b will
                // generate lt(cmp3w(a, b), 0).
                return sbe::makeE<sbe::EPrimBinary>(
                    sbeOp,
                    sbe::makeE<sbe::EPrimBinary>(
                        sbe::EPrimBinary::cmp3w, std::move(lhs), std::move(rhs), collationExpr()),
                    sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, 0),
                    collationExpr());
        }
    }

    return sbe::makeE<sbe::EPrimBinary>(sbeOp, std::move(lhs), std::move(rhs));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const UnaryOp& op, std::unique_ptr<sbe::EExpression> arg) {

    sbe::EPrimUnary::Op sbeOp = getEPrimUnaryOp(op.op());

    return sbe::makeE<sbe::EPrimUnary>(sbeOp, std::move(arg));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const If&,
    std::unique_ptr<sbe::EExpression> cond,
    std::unique_ptr<sbe::EExpression> thenBranch,
    std::unique_ptr<sbe::EExpression> elseBranch) {
    return sbe::makeE<sbe::EIf>(std::move(cond), std::move(thenBranch), std::move(elseBranch));
}

std::unique_ptr<sbe::EExpression> makeFillEmptyNull(std::unique_ptr<sbe::EExpression> e) {
    return sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::fillEmpty,
                                        std::move(e),
                                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0));
}

/*
 * In the ABT, the shard filtering operation is represented by a FunctionCall node with n
 * arguments, in which each argument is a projection of the value of one field of the
 * shard key (which has n fields). In the SBE plan, the shard filtering is represented by a
 * 2-argument function called shardFilter. The first argument is the slotID of a slot that
 * contains a ShardFilterer instance. The second argument to shardFilter is a function
 * (makeBsonObj) that takes a spec for the construction of an object which evaluates to the
 * shard key (e.g. the output of the function is {a:1, b:1, ...}).
 */
std::unique_ptr<sbe::EExpression> SBEExpressionLowering::handleShardFilterFunctionCall(
    const FunctionCall& fn,
    std::vector<std::unique_ptr<sbe::EExpression>>& args,
    std::string name) {
    tassert(7814401, "NodeProps must not be nullptr in shardFilter lowering", _np);
    tassert(7814402, "Metadata must not be nullptr in shardFilter lowering", _metadata);

    // First, get the paths to the shard key fields.
    auto indexingAvailabilityProp =
        getPropertyConst<properties::IndexingAvailability>(_np->_logicalProps);
    const std::string& scanDefName = indexingAvailabilityProp.getScanDefName();
    tassert(7814403,
            "The metadata must contain the scan definition specified by the "
            "IndexingAvailability property in order to perform shard filtering",
            _metadata->_scanDefs.contains(scanDefName));
    const auto& shardKeyPaths = _metadata->_scanDefs.at(scanDefName).shardingMetadata().shardKey();

    // Specify a BSONObj which will contain the shard key values.
    tassert(7814404,
            "The number of fields passed to shardFilter does not match the number of fields in "
            "the shard key",
            fn.nodes().size() == shardKeyPaths.size());
    std::vector<std::string> fields;
    std::vector<sbe::MakeObjSpec::FieldAction> fieldActions;
    sbe::EExpression::Vector projectValues;

    size_t argIdx = 0;
    for (auto& i : shardKeyPaths) {
        fields.emplace_back(PathStringify::stringify(i._path));
        fieldActions.emplace_back(argIdx);
        ++argIdx;
    }

    // Each argument corresponds to one component of the shard key. This loop lowers an expression
    // for each component. The ShardFilterer expects the BSONObj of the shard key to have values for
    // each component of the shard key; since shard components may be missing, we must wrap the
    // expression in a fillEmpty to coerce a missing shard key component to an explicit null. For
    // example, if the shard key is {a: 1, b: 1} and the document is {b: 123}, the object we will
    // generate is {a: null, b: 123}.
    for (const ABT& node : fn.nodes()) {
        projectValues.push_back(makeFillEmptyNull(this->optimize(node)));
    }

    auto fieldsScope = FieldListScope::kOpen;
    auto makeObjSpec =
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::makeObjSpec,
                                   sbe::value::bitcastFrom<sbe::MakeObjSpec*>(new sbe::MakeObjSpec(
                                       fieldsScope, std::move(fields), std::move(fieldActions))));

    auto makeObjRoot = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0);
    auto hasInputFieldsExpr = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                                         sbe::value::bitcastFrom<bool>(false));
    sbe::EExpression::Vector makeObjArgs;
    makeObjArgs.reserve(3 + projectValues.size());
    makeObjArgs.push_back(std::move(makeObjSpec));
    makeObjArgs.push_back(std::move(makeObjRoot));
    makeObjArgs.push_back(std::move(hasInputFieldsExpr));
    std::move(projectValues.begin(), projectValues.end(), std::back_inserter(makeObjArgs));

    auto shardKeyBSONObjExpression =
        sbe::makeE<sbe::EFunction>("makeBsonObj", std::move(makeObjArgs));

    // Prepare the FunctionCall expression.
    sbe::EExpression::Vector argVector;
    argVector.push_back(sbe::makeE<sbe::EVariable>(_providedSlots.getSlot(kshardFiltererSlotName)));
    argVector.push_back(std::move(shardKeyBSONObjExpression));
    return sbe::makeE<sbe::EFunction>(name, std::move(argVector));
}

std::unique_ptr<sbe::EExpression> SBEExpressionLowering::transport(
    const FunctionCall& fn, std::vector<std::unique_ptr<sbe::EExpression>> args) {
    auto name = fn.name();

    if (name == "fail") {
        uassert(6250200, "Invalid number of arguments to fail()", fn.nodes().size() == 2);
        const auto* codeConstPtr = fn.nodes().at(0).cast<Constant>();
        const auto* messageConstPtr = fn.nodes().at(1).cast<Constant>();

        uassert(6250201,
                "First argument to fail() must be a 32-bit integer constant",
                codeConstPtr != nullptr && codeConstPtr->isValueInt32());
        uassert(6250202,
                "Second argument to fail() must be a string constant",
                messageConstPtr != nullptr && messageConstPtr->isString());

        return sbe::makeE<sbe::EFail>(static_cast<ErrorCodes::Error>(codeConstPtr->getValueInt32()),
                                      messageConstPtr->getString());
    }

    if (name == "convert") {
        uassert(6250203, "Invalid number of arguments to convert()", fn.nodes().size() == 2);
        const auto* constPtr = fn.nodes().at(1).cast<Constant>();

        uassert(6250204,
                "Second argument to convert() must be a 32-bit integer constant",
                constPtr != nullptr && constPtr->isValueInt32());
        int32_t constVal = constPtr->getValueInt32();

        uassert(6250205,
                "Second argument to convert() must be a numeric type tag",
                constVal >= static_cast<int32_t>(std::numeric_limits<uint8_t>::min()) &&
                    constVal <= static_cast<int32_t>(std::numeric_limits<uint8_t>::max()) &&
                    sbe::value::isNumber(static_cast<sbe::value::TypeTags>(constVal)));

        return sbe::makeE<sbe::ENumericConvert>(std::move(args.at(0)),
                                                static_cast<sbe::value::TypeTags>(constVal));
    }

    if (name == "typeMatch") {
        uassert(6250206, "Invalid number of arguments to typeMatch()", fn.nodes().size() == 2);
        const auto* constPtr = fn.nodes().at(1).cast<Constant>();

        uassert(6250207,
                "Second argument to typeMatch() must be a 32-bit integer constant",
                constPtr != nullptr && constPtr->isValueInt32());

        return sbe::makeE<sbe::EFunction>(
            "typeMatch",
            sbe::makeEs(std::move(args.at(0)),
                        sbe::makeE<sbe::EConstant>(
                            sbe::value::TypeTags::NumberInt32,
                            sbe::value::bitcastFrom<int32_t>(constPtr->getValueInt32()))));
    }

    if (name == "shardFilter") {
        return handleShardFilterFunctionCall(fn, args, name);
    }

    if (name == kParameterFunctionName) {
        uassert(8128700, "Invalid number of arguments to getParam()", fn.nodes().size() == 2);
        const auto* paramId = fn.nodes().at(0).cast<Constant>();
        auto paramIdVal = paramId->getValueInt32();

        auto slotId = [&]() {
            auto it = _inputParamToSlotMap.find(paramIdVal);
            if (it != _inputParamToSlotMap.end()) {
                // This input parameter id has already been tied to a particular runtime environment
                // slot. Just return that slot to the caller. This can happen if a query planning
                // optimization or rewrite chose to clone one of the input expressions from the
                // user's query.
                return it->second;
            }

            auto newSlotId = _providedSlots.registerSlot(
                sbe::value::TypeTags::Nothing, 0, false /* owned */, &_slotIdGenerator);
            _inputParamToSlotMap.emplace(paramIdVal, newSlotId);
            return newSlotId;
        }();

        return sbe::makeE<sbe::EVariable>(slotId);
    }

    // TODO - this is an open question how to do the name mappings.
    if (name == "$sum") {
        name = "sum";
    } else if (name == "$first") {
        name = "first";
    } else if (name == "$last") {
        name = "last";
    } else if (name == "$min") {
        name = "min";
    } else if (name == "$max") {
        name = "max";
    } else if (name == "$addToSet") {
        name = "addToSet";
    } else if (name == "$push") {
        name = "addToArray";
    }

    return sbe::makeE<sbe::EFunction>(name, toInlinedVector(std::move(args)));
}

sbe::value::SlotVector SBENodeLowering::convertProjectionsToSlots(
    const SlotVarMap& slotMap, const ProjectionNameVector& projectionNames) const {
    sbe::value::SlotVector result;
    for (const ProjectionName& projectionName : projectionNames) {
        auto it = slotMap.find(projectionName);
        tassert(6624211,
                str::stream() << "undefined variable: " << projectionName,
                it != slotMap.end());
        result.push_back(it->second);
    }
    return result;
}

sbe::value::SlotVector SBENodeLowering::convertRequiredProjectionsToSlots(
    const SlotVarMap& slotMap,
    const NodeProps& props,
    const sbe::value::SlotVector& toExclude) const {
    using namespace properties;

    sbe::value::SlotSet toExcludeSet;
    for (const auto slot : toExclude) {
        toExcludeSet.insert(slot);
    }

    sbe::value::SlotVector result;
    // Need to dedup here, because even if 'projections' is unique, 'slotMap' can map two
    // projections to the same slot. 'convertProjectionsToSlots' can't dedup because it preserves
    // the order of items in the vector.
    sbe::value::SlotSet seen;
    const auto& projections =
        getPropertyConst<ProjectionRequirement>(props._physicalProps).getProjections();
    for (const auto slot : convertProjectionsToSlots(slotMap, projections.getVector())) {
        if (toExcludeSet.count(slot) == 0 && seen.count(slot) == 0) {
            result.push_back(slot);
            seen.insert(slot);
        }
    }
    return result;
}

PlanNodeId SBENodeLowering::getPlanNodeId(const Node& node) const {
    if (auto it = _nodeToGroupPropsMap.find(&node); it != _nodeToGroupPropsMap.cend()) {
        return it->second._planNodeId;
    };
    return 0;
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::optimize(
    const ABT& n, SlotVarMap& slotMap, boost::optional<sbe::value::SlotId>& ridSlot) {
    return generateInternal(n, slotMap, ridSlot);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::generateInternal(
    const ABT& n, SlotVarMap& slotMap, boost::optional<sbe::value::SlotId>& ridSlot) {
    tassert(
        7239200, "Should not be lowering only logical ABT node", !n.cast<ExclusivelyLogicalNode>());
    return algebra::walk<true>(n, *this, slotMap, ridSlot);
}

void SBENodeLowering::mapProjToSlot(SlotVarMap& slotMap,
                                    const ProjectionName& projName,
                                    const sbe::value::SlotId slot,
                                    const bool canOverwrite) {
    const bool inserted = slotMap.insert_or_assign(projName, slot).second;
    if (!canOverwrite) {
        tassert(6624263, "Cannot overwrite slot map", inserted);
    }
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const RootNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& refs) {
    using namespace properties;
    auto input = generateInternal(child, slotMap, ridSlot);

    auto output = refs.cast<References>();
    tassert(6624212, "refs expected", output);

    SlotVarMap finalMap;
    for (auto& o : output->nodes()) {
        auto var = o.cast<Variable>();
        tassert(6624213, "var expected", var);
        if (auto it = slotMap.find(var->name()); it != slotMap.end()) {
            finalMap.emplace(var->name(), it->second);
        }
    }

    if (const auto& props = _nodeToGroupPropsMap.at(&n);
        hasProperty<ProjectionRequirement>(props._physicalProps)) {
        if (const auto& ridProjName = props._ridProjName) {
            // If we required rid on the Root node, populate ridSlot.
            const auto& projections =
                getPropertyConst<ProjectionRequirement>(props._physicalProps).getProjections();
            if (projections.find(*ridProjName)) {
                // Deliver the ridSlot separate from the slotMap.
                ridSlot = slotMap.at(*ridProjName);
                finalMap.erase(*ridProjName);
            }
        }
    }

    std::swap(slotMap, finalMap);
    return input;
}

void SBENodeLowering::extractAndLowerExpressions(const EvaluationNode& n,
                                                 SlotVarMap& slotMap,
                                                 sbe::SlotExprPairVector& projectsOut) {
    auto& binder = n.binder();
    auto& names = binder.names();
    auto& exprs = binder.exprs();

    const auto& groupProps = _nodeToGroupPropsMap.at(&n);
    for (size_t idx = 0; idx < exprs.size(); ++idx) {
        auto expr = lowerExpression(exprs[idx], slotMap, &groupProps);
        auto slot = _slotIdGenerator.generate();

        mapProjToSlot(slotMap, names[idx], slot);
        projectsOut.emplace_back(slot, std::move(expr));
    }
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const EvaluationNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& binds) {
    // If the evaluation node is only renaming a variable, do not place a project stage.
    if (auto varPtr = n.getProjection().cast<Variable>(); varPtr != nullptr) {
        auto input = generateInternal(child, slotMap, ridSlot);
        mapProjToSlot(slotMap, n.getProjectionName(), slotMap.at(varPtr->name()));
        return input;
    }

    // Dependency analysis: this node can be merged with its child into the same project stage if it
    // is independent from the child and the child is not a simple rename of a variable.
    if (child.is<EvaluationNode>()) {
        if (child.cast<EvaluationNode>()->getProjection().cast<Variable>() == nullptr) {
            ProjectionNameSet varRefs = mongo::optimizer::collectVariableReferences(abtn);
            const DefinitionsMap childDefs = _env.hasDefinitions(child.ref())
                ? _env.getDefinitions(child.ref())
                : DefinitionsMap{};
            bool foundDependency = false;
            for (const ProjectionName& varName : varRefs) {
                auto it = childDefs.find(varName);
                if (it != childDefs.cend() && it->second.definedBy.is<EvaluationNode>()) {
                    foundDependency = true;
                    break;
                }
            }
            if (!foundDependency) {
                // This node can be merged with its child.
                _evalMap.insert(std::make_pair(child.cast<EvaluationNode>(), &n));
                return generateInternal(child, slotMap, ridSlot);
            }
        }
    }

    auto input = generateInternal(child, slotMap, ridSlot);

    sbe::SlotExprPairVector projects;
    extractAndLowerExpressions(n, slotMap, projects);

    auto childEval = &n;
    auto it = _evalMap.find(childEval);
    while (it != _evalMap.end()) {
        auto parentEval = it->second;
        extractAndLowerExpressions(*parentEval, slotMap, projects);
        _evalMap.erase(it);
        childEval = parentEval;
        it = _evalMap.find(childEval);
    }

    return sbe::makeS<sbe::ProjectStage>(std::move(input), std::move(projects), getPlanNodeId(n));
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const FilterNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& filter) {
    auto input = generateInternal(child, slotMap, ridSlot);
    const auto& groupProps = _nodeToGroupPropsMap.at(&n);
    auto expr = lowerExpression(filter, slotMap, &groupProps);
    const PlanNodeId planNodeId = groupProps._planNodeId;

    // Check if the filter expression is 'constant' (i.e., does not depend on any variables); then
    // create FilterStage<true> if it is constant, or FilterStage<false> otherwise.
    bool isConstant = true;
    VariableEnvironment::walkVariables(filter, [&](const Variable&) { isConstant = false; });

    if (isConstant) {
        return sbe::makeS<sbe::FilterStage<true>>(std::move(input), std::move(expr), planNodeId);
    } else {
        return sbe::makeS<sbe::FilterStage<false>>(std::move(input), std::move(expr), planNodeId);
    }
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const LimitSkipNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child) {
    auto input = generateInternal(child, slotMap, ridSlot);

    return sbe::makeS<sbe::LimitSkipStage>(
        std::move(input),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                   sbe::value::bitcastFrom<int64_t>(n.getProperty().getLimit())),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                   sbe::value::bitcastFrom<int64_t>(n.getProperty().getSkip())),
        getPlanNodeId(n));
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const ExchangeNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& refs) {
    using namespace std::literals;
    using namespace properties;

    // The DOP is obtained from the child (number of producers).
    const auto& childProps = _nodeToGroupPropsMap.at(n.getChild().cast<Node>())._physicalProps;
    const auto& childDistribution = getPropertyConst<DistributionRequirement>(childProps);
    tassert(6624330,
            "Parent and child distributions are the same",
            !(childDistribution == n.getProperty()));

    const size_t localDOP =
        (childDistribution.getDistributionAndProjections()._type == DistributionType::Centralized)
        ? 1
        : _metadata._numberOfPartitions;
    tassert(6624215, "invalid DOP", localDOP >= 1);

    auto input = generateInternal(child, slotMap, ridSlot);

    // Initialized to arbitrary placeholder
    sbe::ExchangePolicy localPolicy{};
    std::unique_ptr<sbe::EExpression> partitionExpr;

    const auto& distribAndProjections = n.getProperty().getDistributionAndProjections();
    switch (distribAndProjections._type) {
        case DistributionType::Centralized:
        case DistributionType::Replicated:
            localPolicy = sbe::ExchangePolicy::broadcast;
            break;

        case DistributionType::RoundRobin:
            localPolicy = sbe::ExchangePolicy::roundrobin;
            break;

        case DistributionType::RangePartitioning:
            // We set 'localPolicy' to 'ExchangePolicy::rangepartition' here, but there is more
            // that we need to do to actually support the RangePartitioning distribution.
            // TODO SERVER-62523: Implement real support for the RangePartitioning distribution
            // and add some test coverage.
            localPolicy = sbe::ExchangePolicy::rangepartition;
            break;

        case DistributionType::HashPartitioning: {
            localPolicy = sbe::ExchangePolicy::hashpartition;
            std::vector<std::unique_ptr<sbe::EExpression>> args;
            for (const ProjectionName& proj : distribAndProjections._projectionNames) {
                auto it = slotMap.find(proj);
                tassert(6624216, str::stream() << "undefined var: " << proj, it != slotMap.end());

                args.emplace_back(sbe::makeE<sbe::EVariable>(it->second));
            }
            partitionExpr = sbe::makeE<sbe::EFunction>("hash"_sd, toInlinedVector(std::move(args)));
            break;
        }

        case DistributionType::UnknownPartitioning:
            tasserted(6624217, "Cannot partition into unknown distribution");

        default:
            MONGO_UNREACHABLE;
    }

    const auto& nodeProps = _nodeToGroupPropsMap.at(&n);
    auto fields = convertRequiredProjectionsToSlots(slotMap, nodeProps);

    return sbe::makeS<sbe::ExchangeConsumer>(std::move(input),
                                             localDOP,
                                             std::move(fields),
                                             localPolicy,
                                             std::move(partitionExpr),
                                             nullptr,
                                             nodeProps._planNodeId);
}

static sbe::value::SortDirection collationOpToSBESortDirection(const CollationOp collOp) {
    switch (collOp) {
        // TODO: is there a more efficient way to compute clustered collation op than sort?
        case CollationOp::Ascending:
        case CollationOp::Clustered:
            return sbe::value::SortDirection::Ascending;
        case CollationOp::Descending:
            return sbe::value::SortDirection::Descending;
    }
    MONGO_UNREACHABLE;
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const CollationNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& refs) {
    auto input = generateInternal(child, slotMap, ridSlot);

    sbe::value::SlotVector orderBySlots;
    std::vector<sbe::value::SortDirection> directions;
    for (const auto& entry : n.getProperty().getCollationSpec()) {
        auto it = slotMap.find(entry.first);

        tassert(6624219,
                str::stream() << "undefined orderBy var: " << entry.first,
                it != slotMap.end());
        orderBySlots.push_back(it->second);

        directions.push_back(collationOpToSBESortDirection(entry.second));
    }

    const auto& nodeProps = _nodeToGroupPropsMap.at(&n);
    const auto& physProps = nodeProps._physicalProps;

    std::unique_ptr<sbe::EExpression> limit = nullptr;
    if (properties::hasProperty<properties::LimitSkipRequirement>(physProps)) {
        const auto& limitSkipReq =
            properties::getPropertyConst<properties::LimitSkipRequirement>(physProps);
        tassert(6624221, "We should not have skip set here", limitSkipReq.getSkip() == 0);
        limit =
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                       sbe::value::bitcastFrom<int64_t>(limitSkipReq.getLimit()));
    }

    // TODO: obtain defaults for these.
    const size_t memoryLimit = 100 * (1ul << 20);  // 100MB
    const bool allowDiskUse = false;

    auto vals = convertRequiredProjectionsToSlots(slotMap, nodeProps, orderBySlots);
    return sbe::makeS<sbe::SortStage>(std::move(input),
                                      std::move(orderBySlots),
                                      std::move(directions),
                                      std::move(vals),
                                      std::move(limit),
                                      memoryLimit,
                                      allowDiskUse,
                                      _yieldPolicy,
                                      nodeProps._planNodeId);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const UniqueNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& refs) {
    auto input = generateInternal(child, slotMap, ridSlot);

    sbe::value::SlotVector keySlots;
    for (const ProjectionName& projectionName : n.getProjections()) {
        auto it = slotMap.find(projectionName);
        tassert(6624222,
                str::stream() << "undefined variable: " << projectionName,
                it != slotMap.end());
        keySlots.push_back(it->second);
    }

    return sbe::makeS<sbe::UniqueStage>(std::move(input), std::move(keySlots), getPlanNodeId(n));
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const SpoolProducerNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& filter,
                                                      const ABT& binder,
                                                      const ABT& refs) {
    auto input = generateInternal(child, slotMap, ridSlot);

    sbe::value::SlotVector vals;
    for (const ProjectionName& projectionName : n.binder().names()) {
        auto it = slotMap.find(projectionName);
        tassert(6624139,
                str::stream() << "undefined variable: " << projectionName,
                it != slotMap.end());
        vals.push_back(it->second);
    }

    const PlanNodeId planNodeId = getPlanNodeId(n);
    switch (n.getType()) {
        case SpoolProducerType::Eager:
            return sbe::makeS<sbe::SpoolEagerProducerStage>(
                std::move(input), n.getSpoolId(), std::move(vals), _yieldPolicy, planNodeId);

        case SpoolProducerType::Lazy: {
            auto expr = lowerExpression(filter, slotMap, &_nodeToGroupPropsMap.at(&n));
            return sbe::makeS<sbe::SpoolLazyProducerStage>(
                std::move(input), n.getSpoolId(), std::move(vals), std::move(expr), planNodeId);
        }
    }

    MONGO_UNREACHABLE;
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const SpoolConsumerNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& binder) {
    sbe::value::SlotVector vals;
    for (const ProjectionName& projectionName : n.binder().names()) {
        auto slot = _slotIdGenerator.generate();
        mapProjToSlot(slotMap, projectionName, slot);
        vals.push_back(slot);
    }

    const PlanNodeId planNodeId = getPlanNodeId(n);
    switch (n.getType()) {
        case SpoolConsumerType::Stack:
            return sbe::makeS<sbe::SpoolConsumerStage<true /*isStack*/>>(
                n.getSpoolId(), std::move(vals), _yieldPolicy, planNodeId);

        case SpoolConsumerType::Regular:
            return sbe::makeS<sbe::SpoolConsumerStage<false /*isStack*/>>(
                n.getSpoolId(), std::move(vals), _yieldPolicy, planNodeId);
    }

    MONGO_UNREACHABLE;
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const GroupByNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& aggBinds,
                                                      const ABT& aggRefs,
                                                      const ABT& gbBind,
                                                      const ABT& gbRefs) {

    auto input = generateInternal(child, slotMap, ridSlot);

    // Ideally, we should make a distinction between gbBind and gbRefs; i.e. internal references
    // used by the hash agg to determinte the group by values from its input and group by values as
    // outputted by the hash agg after the grouping. However, SBE hash agg uses the same slot it to
    // represent both so that distinction is kind of moot.
    sbe::value::SlotVector gbs;
    auto gbCols = gbRefs.cast<References>();
    tassert(6624223, "refs expected", gbCols);
    for (auto& o : gbCols->nodes()) {
        auto var = o.cast<Variable>();
        tassert(6624224, "var expected", var);
        auto it = slotMap.find(var->name());
        tassert(6624225, str::stream() << "undefined var: " << var->name(), it != slotMap.end());
        gbs.push_back(it->second);
    }

    // Similar considerations apply to the agg expressions as to the group by columns.
    auto& names = n.binderAgg().names();

    auto refsAgg = aggRefs.cast<References>();
    tassert(6624227, "refs expected", refsAgg);
    auto& exprs = refsAgg->nodes();

    sbe::AggExprVector aggs;
    aggs.reserve(exprs.size());

    const auto& groupProps = _nodeToGroupPropsMap.at(&n);
    for (size_t idx = 0; idx < exprs.size(); ++idx) {
        auto expr = lowerExpression(exprs[idx], slotMap, &groupProps);
        auto slot = _slotIdGenerator.generate();

        mapProjToSlot(slotMap, names[idx], slot);
        // TODO: We need to update the nullptr to pass in the initializer expression for certain
        // accumulators.
        aggs.push_back({slot, sbe::AggExprPair{nullptr, std::move(expr)}});
    }

    boost::optional<sbe::value::SlotId> collatorSlot =
        _providedSlots.getSlotIfExists("collator"_sd);
    // Unused
    sbe::value::SlotVector seekKeysSlots;

    return sbe::makeS<sbe::HashAggStage>(std::move(input),
                                         std::move(gbs),
                                         std::move(aggs),
                                         std::move(seekKeysSlots),
                                         true /*optimizedClose*/,
                                         collatorSlot,
                                         false /*allowDiskUse*/,
                                         // Since we are always disallowing disk use for this stage,
                                         // we need not provide merging expressions. Once spilling
                                         // is permitted here, we will need to generate merging
                                         // expressions during lowering.
                                         sbe::makeSlotExprPairVec() /*mergingExprs*/,
                                         _yieldPolicy,
                                         groupProps._planNodeId);
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const NestedLoopJoinNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& leftChild,
                                                      const ABT& rightChild,
                                                      const ABT& filter) {
    auto outerStage = generateInternal(leftChild, slotMap, ridSlot);
    auto innerStage = generateInternal(rightChild, slotMap, ridSlot);

    // List of correlated projections (bound in outer side and referred to in the inner side).
    sbe::value::SlotVector correlatedSlots;
    for (const ProjectionName& projectionName : n.getCorrelatedProjectionNames()) {
        correlatedSlots.push_back(slotMap.at(projectionName));
    }
    // Sorting is not essential. Here we sort only for SBE plan stability.
    std::sort(correlatedSlots.begin(), correlatedSlots.end());
    // However, we should deduplicate the slots, in case two projections mapped to the same slot.
    correlatedSlots.erase(std::unique(correlatedSlots.begin(), correlatedSlots.end()),
                          correlatedSlots.end());

    auto expr = lowerExpression(filter, slotMap, &_nodeToGroupPropsMap.at(&n));

    const auto& leftChildProps = _nodeToGroupPropsMap.at(n.getLeftChild().cast<Node>());
    auto outerProjects = convertRequiredProjectionsToSlots(slotMap, leftChildProps);

    const auto& rightChildProps = _nodeToGroupPropsMap.at(n.getRightChild().cast<Node>());
    auto innerProjects = convertRequiredProjectionsToSlots(slotMap, rightChildProps);

    sbe::JoinType joinType = [&]() {
        switch (n.getJoinType()) {
            case JoinType::Inner:
                return sbe::JoinType::Inner;
            case JoinType::Left:
                return sbe::JoinType::Left;
            default:
                MONGO_UNREACHABLE;
        }
    }();

    return sbe::makeS<sbe::LoopJoinStage>(std::move(outerStage),
                                          std::move(innerStage),
                                          std::move(outerProjects),
                                          std::move(correlatedSlots),
                                          std::move(innerProjects),
                                          std::move(expr),
                                          joinType,
                                          getPlanNodeId(n));
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const HashJoinNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& leftChild,
                                                      const ABT& rightChild,
                                                      const ABT& refs) {
    // Note the inner and outer sides here are reversed. The HashJoinNode assumes the build side is
    // the inner side while sbe hash join stage assumes the build side is the outer side.

    auto innerStage = generateInternal(leftChild, slotMap, ridSlot);
    auto outerStage = generateInternal(rightChild, slotMap, ridSlot);

    tassert(6624228, "Only inner joins supported for now", n.getJoinType() == JoinType::Inner);

    const auto& leftProps = _nodeToGroupPropsMap.at(n.getLeftChild().cast<Node>());
    const auto& rightProps = _nodeToGroupPropsMap.at(n.getRightChild().cast<Node>());

    // Add RID projection only from outer side.
    auto innerKeys = convertProjectionsToSlots(slotMap, n.getLeftKeys());
    auto innerProjects = convertRequiredProjectionsToSlots(slotMap, leftProps, innerKeys);
    auto outerKeys = convertProjectionsToSlots(slotMap, n.getRightKeys());
    auto outerProjects = convertRequiredProjectionsToSlots(slotMap, rightProps, outerKeys);

    boost::optional<sbe::value::SlotId> collatorSlot =
        _providedSlots.getSlotIfExists("collator"_sd);
    return sbe::makeS<sbe::HashJoinStage>(std::move(outerStage),
                                          std::move(innerStage),
                                          std::move(outerKeys),
                                          std::move(outerProjects),
                                          std::move(innerKeys),
                                          std::move(innerProjects),
                                          collatorSlot,
                                          _yieldPolicy,
                                          getPlanNodeId(n));
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const MergeJoinNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& leftChild,
                                                      const ABT& rightChild,
                                                      const ABT& refs) {
    auto outerStage = generateInternal(leftChild, slotMap, ridSlot);
    auto innerStage = generateInternal(rightChild, slotMap, ridSlot);

    const auto& leftProps = _nodeToGroupPropsMap.at(n.getLeftChild().cast<Node>());
    const auto& rightProps = _nodeToGroupPropsMap.at(n.getRightChild().cast<Node>());

    std::vector<sbe::value::SortDirection> sortDirs;
    for (const CollationOp op : n.getCollation()) {
        sortDirs.push_back(collationOpToSBESortDirection(op));
    }

    // Add RID projection only from outer side.
    auto outerKeys = convertProjectionsToSlots(slotMap, n.getLeftKeys());
    auto outerProjects = convertRequiredProjectionsToSlots(slotMap, leftProps, outerKeys);
    auto innerKeys = convertProjectionsToSlots(slotMap, n.getRightKeys());
    auto innerProjects = convertRequiredProjectionsToSlots(slotMap, rightProps, innerKeys);

    return sbe::makeS<sbe::MergeJoinStage>(std::move(outerStage),
                                           std::move(innerStage),
                                           std::move(outerKeys),
                                           std::move(outerProjects),
                                           std::move(innerKeys),
                                           std::move(innerProjects),
                                           std::move(sortDirs),
                                           getPlanNodeId(n));
}


std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const SortedMergeNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABTVector& children,
                                                      const ABT& binder,
                                                      const ABT& /*refs*/) {
    const auto& names = n.binder().names();

    const ProjectionCollationSpec& collSpec = n.getCollationReq().getCollationSpec();

    std::vector<sbe::value::SortDirection> keyDirs;
    for (const auto& collEntry : collSpec) {
        keyDirs.push_back(collationOpToSBESortDirection(collEntry.second));
    }

    sbe::PlanStage::Vector loweredChildren;
    std::vector<sbe::value::SlotVector> inputKeys;
    std::vector<sbe::value::SlotVector> inputVals;
    for (const ABT& child : children) {
        // Use a fresh map to prevent same projections for every child being overwritten. We
        // initialize with the current map in order to be able to use correlated slots.
        SlotVarMap localMap = slotMap;
        boost::optional<sbe::value::SlotId> localRIDSlot;
        auto loweredChild = optimize(child, localMap, localRIDSlot);
        tassert(7063700, "Unexpected rid slot", !localRIDSlot);

        loweredChildren.push_back(std::move(loweredChild));

        // Find the slots for the collation keys. Also find slots for other values passed.
        sbe::value::SlotVector childKeys(collSpec.size());
        sbe::value::SlotVector childVals;
        // Note that lowering for SortedMergeNode does not take into account required projections
        // from the Cascade props for this node. Like UnionNode, it's expected that all fields that
        // should be visible above a SortedMergeNode should be added to the exprBinder explicitly
        // before lowering.
        for (const auto& name : names) {
            const auto it = std::find_if(
                collSpec.begin(), collSpec.end(), [&](const auto& x) { return x.first == name; });
            if (it != collSpec.end()) {
                const size_t index = std::distance(collSpec.begin(), it);
                childKeys.at(index) = localMap.at(name);
            }
            childVals.push_back(localMap.at(name));
        }
        inputKeys.emplace_back(std::move(childKeys));
        inputVals.emplace_back(std::move(childVals));
    }

    sbe::value::SlotVector outputVals;
    for (const auto& name : names) {
        const auto outputSlot = _slotIdGenerator.generate();
        mapProjToSlot(slotMap, name, outputSlot);
        outputVals.push_back(outputSlot);
    }

    return sbe::makeS<sbe::SortedMergeStage>(std::move(loweredChildren),
                                             std::move(inputKeys),
                                             std::move(keyDirs),
                                             std::move(inputVals),
                                             std::move(outputVals),
                                             getPlanNodeId(n));
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const UnionNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABTVector& children,
                                                      const ABT& binder,
                                                      const ABT& /*refs*/) {
    const auto& names = n.binder().names();

    sbe::PlanStage::Vector loweredChildren;
    std::vector<sbe::value::SlotVector> inputVals;

    for (const ABT& child : children) {
        // Use a fresh map to prevent same projections for every child being overwritten. We
        // initialize with the current map in order to be able to use correlated slots.
        SlotVarMap localMap = slotMap;
        boost::optional<sbe::value::SlotId> localRIDSlot;
        auto loweredChild = optimize(child, localMap, localRIDSlot);
        tassert(6624258, "Unexpected rid slot", !localRIDSlot);

        if (children.size() == 1) {
            // Union with one child is used to restrict projections. Do not place a union stage.
            for (const auto& name : names) {
                mapProjToSlot(slotMap, name, localMap.at(name));
            }
            return loweredChild;
        }
        loweredChildren.push_back(std::move(loweredChild));

        sbe::value::SlotVector childSlots;
        for (const auto& name : names) {
            childSlots.push_back(localMap.at(name));
        }
        inputVals.emplace_back(std::move(childSlots));
    }

    sbe::value::SlotVector outputVals;
    for (const auto& name : names) {
        const auto outputSlot = _slotIdGenerator.generate();
        mapProjToSlot(slotMap, name, outputSlot);
        outputVals.push_back(outputSlot);
    }

    return sbe::makeS<sbe::UnionStage>(
        std::move(loweredChildren), std::move(inputVals), std::move(outputVals), getPlanNodeId(n));
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const UnwindNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& child,
                                                      const ABT& pidBind,
                                                      const ABT& refs) {
    auto input = generateInternal(child, slotMap, ridSlot);

    auto it = slotMap.find(n.getProjectionName());
    tassert(6624230,
            str::stream() << "undefined unwind variable: " << n.getProjectionName(),
            it != slotMap.end());

    auto inputSlot = it->second;
    auto outputSlot = _slotIdGenerator.generate();
    auto outputPidSlot = _slotIdGenerator.generate();

    // The unwind is overwriting the output projection.
    mapProjToSlot(slotMap, n.getProjectionName(), outputSlot, true /*canOverwrite*/);
    mapProjToSlot(slotMap, n.getPIDProjectionName(), outputPidSlot);

    return sbe::makeS<sbe::UnwindStage>(std::move(input),
                                        inputSlot,
                                        outputSlot,
                                        outputPidSlot,
                                        n.getRetainNonArrays(),
                                        getPlanNodeId(n));
}

void SBENodeLowering::generateSlots(SlotVarMap& slotMap,
                                    const FieldProjectionMap& fieldProjectionMap,
                                    boost::optional<sbe::value::SlotId>& ridSlot,
                                    boost::optional<sbe::value::SlotId>& rootSlot,
                                    std::vector<std::string>& fields,
                                    sbe::value::SlotVector& vars) {
    if (const auto& projName = fieldProjectionMap._ridProjection) {
        ridSlot = _slotIdGenerator.generate();
        // Allow overwriting slots for rid projections only. We have a single rid projection per
        // collection.
        mapProjToSlot(slotMap, *projName, ridSlot.value(), true /*canOverwrite*/);
    }
    if (const auto& projName = fieldProjectionMap._rootProjection) {
        rootSlot = _slotIdGenerator.generate();
        mapProjToSlot(slotMap, *projName, rootSlot.value());
    }

    for (const auto& [fieldName, projectionName] : fieldProjectionMap._fieldProjections) {
        vars.push_back(_slotIdGenerator.generate());
        mapProjToSlot(slotMap, projectionName, vars.back());
        fields.push_back(fieldName.value().toString());
    }
}


std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const PhysicalScanNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& /*binds*/) {
    const ScanDefinition& def = _metadata._scanDefs.at(n.getScanDefName());
    tassert(6624231, "Collection must exist to lower Scan", def.exists());
    auto& typeSpec = def.getOptionsMap().at("type");

    boost::optional<sbe::value::SlotId> scanRidSlot;
    boost::optional<sbe::value::SlotId> rootSlot;
    std::vector<std::string> fields;
    sbe::value::SlotVector vars;
    generateSlots(slotMap, n.getFieldProjectionMap(), scanRidSlot, rootSlot, fields, vars);

    const PlanNodeId planNodeId = getPlanNodeId(n);
    if (typeSpec == "mongod") {
        tassert(8423400, "ScanDefinition must have a UUID", def.getUUID().has_value());
        const UUID& uuid = def.getUUID().get();

        // Unused.
        boost::optional<sbe::value::SlotId> seekRecordIdSlot;

        sbe::ScanCallbacks callbacks({}, {}, {});
        if (n.useParallelScan()) {
            return sbe::makeS<sbe::ParallelScanStage>(uuid,
                                                      rootSlot,
                                                      scanRidSlot,
                                                      boost::none,
                                                      boost::none,
                                                      boost::none,
                                                      boost::none,
                                                      fields,
                                                      vars,
                                                      _yieldPolicy,
                                                      planNodeId,
                                                      callbacks);
        }

        ScanOrder scanOrder = n.getScanOrder();

        bool forwardScan = [&]() {
            switch (scanOrder) {
                case ScanOrder::Forward:
                case ScanOrder::Random:
                    return true;
                case ScanOrder::Reverse:
                    return false;
            }
            MONGO_UNREACHABLE;
        }();
        return sbe::makeS<sbe::ScanStage>(
            uuid,
            rootSlot,
            scanRidSlot,
            boost::none,
            boost::none,
            boost::none,
            boost::none,
            boost::none,
            fields,
            vars,
            seekRecordIdSlot,
            boost::none /* minRecordIdSlot */,
            boost::none /* maxRecordIdSlot */,
            forwardScan,
            _yieldPolicy,
            planNodeId,
            callbacks,
            gDeprioritizeUnboundedUserCollectionScans.load(), /* lowPriority */
            scanOrder == ScanOrder::Random);
    } else {
        tasserted(6624355, "Unknown scan type.");
    }
    return nullptr;
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(
    const ABT& abtn,
    const CoScanNode& n,
    SlotVarMap& slotMap,
    boost::optional<sbe::value::SlotId>& ridSlot) {
    return sbe::makeS<sbe::CoScanStage>(getPlanNodeId(n));
}

std::unique_ptr<sbe::EExpression> SBENodeLowering::convertBoundsToExpr(
    SlotVarMap& slotMap,
    const bool isLower,
    const bool reversed,
    const IndexDefinition& indexDef,
    const CompoundBoundRequirement& bound) {
    std::vector<std::unique_ptr<sbe::EExpression>> ksFnArgs;
    ksFnArgs.emplace_back(
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                   sbe::value::bitcastFrom<int64_t>(indexDef.getVersion())));

    // TODO: ordering is unsigned int32??
    ksFnArgs.emplace_back(
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                   sbe::value::bitcastFrom<uint32_t>(indexDef.getOrdering())));

    auto exprLower = getExpressionLowering(slotMap);
    for (const auto& expr : bound.getBound()) {
        ksFnArgs.emplace_back(exprLower.optimize(expr));
    }

    key_string::Discriminator discriminator;
    // For a reverse scan, we start from the high bound and iterate until the low bound.
    if (isLower != reversed) {
        // For the start point, we want to seek ExclusiveBefore iff the bound is inclusive,
        // so that values equal to the seek value are included.
        discriminator = bound.isInclusive() ? key_string::Discriminator::kExclusiveBefore
                                            : key_string::Discriminator::kExclusiveAfter;
    } else {
        // For the end point we want the opposite.
        discriminator = bound.isInclusive() ? key_string::Discriminator::kExclusiveAfter
                                            : key_string::Discriminator::kExclusiveBefore;
    }

    ksFnArgs.emplace_back(sbe::makeE<sbe::EConstant>(
        sbe::value::TypeTags::NumberInt64,
        sbe::value::bitcastFrom<int64_t>(static_cast<int64_t>(discriminator))));
    return sbe::makeE<sbe::EFunction>("ks", toInlinedVector(std::move(ksFnArgs)));
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const IndexScanNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT&) {
    const auto& fieldProjectionMap = n.getFieldProjectionMap();

    const std::string& indexDefName = n.getIndexDefName();
    const ScanDefinition& scanDef = _metadata._scanDefs.at(n.getScanDefName());
    tassert(6624232, "Collection must exist to lower IndexScan", scanDef.exists());
    const IndexDefinition& indexDef = scanDef.getIndexDefs().at(indexDefName);

    tassert(8423399, "ScanDefinition must have a UUID", scanDef.getUUID().has_value());
    const UUID& uuid = scanDef.getUUID().get();

    boost::optional<sbe::value::SlotId> scanRidSlot;
    boost::optional<sbe::value::SlotId> rootSlot;
    std::vector<std::string> fields;
    sbe::value::SlotVector vars;
    generateSlots(slotMap, fieldProjectionMap, scanRidSlot, rootSlot, fields, vars);
    tassert(6624233, "Cannot deliver root projection in this context", !rootSlot.has_value());

    std::vector<std::pair<size_t, sbe::value::SlotId>> indexVars;
    sbe::IndexKeysInclusionSet indexKeysToInclude;

    for (size_t index = 0; index < fields.size(); index++) {
        const size_t indexFieldPos = decodeIndexKeyName(fields.at(index));
        indexVars.emplace_back(indexFieldPos, vars.at(index));
        indexKeysToInclude.set(indexFieldPos, true);
    }

    // Make sure vars are in sorted order on index field position.
    std::sort(indexVars.begin(), indexVars.end());
    vars.clear();
    for (const auto& [indexFieldPos, slot] : indexVars) {
        vars.push_back(slot);
    }

    const auto& interval = n.getIndexInterval();
    const auto* lowBoundPtr = &interval.getLowBound();
    const auto* highBoundPtr = &interval.getHighBound();

    const bool reverse = n.isIndexReverseOrder();
    if (reverse) {
        std::swap(lowBoundPtr, highBoundPtr);
    }

    auto lowerBoundExpr =
        convertBoundsToExpr(slotMap, true /*isLower*/, reverse, indexDef, *lowBoundPtr);
    auto upperBoundExpr =
        convertBoundsToExpr(slotMap, false /*isLower*/, reverse, indexDef, *highBoundPtr);
    tassert(6624234,
            "Invalid bounds combination",
            lowerBoundExpr != nullptr || upperBoundExpr == nullptr);

    // Unused.
    boost::optional<sbe::value::SlotId> resultSlot;
    return sbe::makeS<sbe::SimpleIndexScanStage>(uuid,
                                                 indexDefName,
                                                 !reverse,
                                                 resultSlot,
                                                 scanRidSlot,
                                                 boost::none,
                                                 boost::none,
                                                 indexKeysToInclude,
                                                 vars,
                                                 std::move(lowerBoundExpr),
                                                 std::move(upperBoundExpr),
                                                 _yieldPolicy,
                                                 getPlanNodeId(n));
}

std::unique_ptr<sbe::PlanStage> SBENodeLowering::walk(const ABT& abtn,
                                                      const SeekNode& n,
                                                      SlotVarMap& slotMap,
                                                      boost::optional<sbe::value::SlotId>& ridSlot,
                                                      const ABT& /*binds*/,
                                                      const ABT& /*refs*/) {
    const ScanDefinition& def = _metadata._scanDefs.at(n.getScanDefName());
    tassert(6624235, "Collection must exist to lower Seek", def.exists());

    auto& typeSpec = def.getOptionsMap().at("type");
    tassert(6624236, "SeekNode only supports mongod collections", typeSpec == "mongod");

    tassert(8423398, "ScanDefinition must have a UUID", def.getUUID().has_value());
    const UUID& uuid = def.getUUID().get();

    boost::optional<sbe::value::SlotId> seekRidSlot;
    boost::optional<sbe::value::SlotId> rootSlot;
    std::vector<std::string> fields;
    sbe::value::SlotVector vars;
    generateSlots(slotMap, n.getFieldProjectionMap(), seekRidSlot, rootSlot, fields, vars);

    boost::optional<sbe::value::SlotId> seekRecordIdSlot = slotMap.at(n.getRIDProjectionName());

    sbe::ScanCallbacks callbacks({}, {}, {});
    return sbe::makeS<sbe::ScanStage>(uuid,
                                      rootSlot,
                                      seekRidSlot,
                                      boost::none,
                                      boost::none,
                                      boost::none,
                                      boost::none,
                                      boost::none,
                                      fields,
                                      vars,
                                      seekRecordIdSlot,
                                      boost::none /* minRecordIdSlot */,
                                      boost::none /* maxRecordIdSlot */,
                                      true /*forward*/,
                                      _yieldPolicy,
                                      getPlanNodeId(n),
                                      callbacks);
}
}  // namespace mongo::optimizer
