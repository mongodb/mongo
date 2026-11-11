// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint_makeobj.h"

#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"


namespace mongo::stage_builder {
namespace {
using sbe::MakeObjSpec;

bool isSlotVariable(const abt::ABT& expr) {
    const auto* var = expr.cast<abt::Variable>();
    return var && SbSlot::fromProjectionName(var->name());
}

bool isSlotOrConstant(const abt::ABT& expr) {
    return isSlotVariable(expr) || expr.is<abt::Constant>();
}
}  // namespace

boost::optional<abt::ABT> readFieldThroughMakeObj(const abt::ABT& makeObj,
                                                  std::string_view field,
                                                  TypeSignature inputType) {
    const auto* call = makeObj.cast<abt::FunctionCall>();
    if (!call || call->fn() != sbe::EFn::kMakeObj ||
        call->nodes().size() < MakeObjSpec::kFirstActionArgPos) {
        return boost::none;
    }
    const auto* specConstant = call->nodes()[MakeObjSpec::kSpecArgPos].cast<abt::Constant>();
    if (!specConstant) {
        return boost::none;
    }
    auto [specTag, specVal] = specConstant->get();
    if (specTag != sbe::value::TypeTags::makeObjSpec) {
        return boost::none;
    }
    const MakeObjSpec& spec = *sbe::value::getMakeObjSpecView(specVal);
    tassert(13888800,
            "Expected a makeObj() call to pass every argument its spec refers to",
            call->nodes().size() == MakeObjSpec::actionArgPos(spec.totalNumArgs));
    const auto& callInput = call->nodes()[MakeObjSpec::kInputArgPos];

    const auto& action = spec.getAction(spec.fields.findPos(field));
    switch (action.type()) {
        case MakeObjSpec::ActionType::kKeep:
            // The field, if present, is copied through unchanged. If the input is not an object,
            // or is an array that the spec traverses, getField() of the result is Nothing, and so
            // is getField() of the input.
            if (!isSlotVariable(callInput)) {
                return boost::none;
            }
            return abt::make<abt::FunctionCall>(sbe::EFn::kGetField,
                                                abt::makeSeq(callInput, abt::Constant::str(field)));
        case MakeObjSpec::ActionType::kDrop:
            return abt::Constant::nothing();
        case MakeObjSpec::ActionType::kSetArg:
        case MakeObjSpec::ActionType::kAddArg: {
            // The result holds the argument under 'field' when the input is an object. For other
            // inputs the result depends on the spec's NonObjInputBehavior and traversal depth, so
            // the read is declined.
            const bool inputIsObject = inputType.isSubset(TypeSignature::kObjectType);
            const size_t argPos = MakeObjSpec::actionArgPos(action.getArgIdx());
            const auto& makeObjArg = call->nodes()[argPos];
            if (!inputIsObject || !isSlotOrConstant(makeObjArg)) {
                return boost::none;
            }
            return makeObjArg;
        }
        default:
            return boost::none;
    }
}
}  // namespace mongo::stage_builder
