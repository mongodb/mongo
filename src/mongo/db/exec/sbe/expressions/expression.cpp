/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expressions/expression.h"

#include "mongo/bson/ordering.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/print_options.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/exec/sbe/vm/vm_datetime.h"
#include "mongo/db/exec/sbe/vm/vm_instruction.h"
#include "mongo/db/exec/sbe/vm/vm_types.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <functional>
#include <sstream>
#include <vector>


namespace mongo {
namespace sbe {


/**
 * Try to convert to a variable if possible.
 */
EVariable* getFrameVariable(EExpression* e) {
    auto var = e->as<EVariable>();
    if (var && var->getFrameId()) {
        return var;
    }
    return nullptr;
}

/**
 * Construct a parameter descriptor from a variable.
 */
vm::Instruction::Parameter getParam(EVariable* var) {
    if (var) {
        return {(int)var->getSlotId(), var->isMoveFrom(), var->getFrameId()};
    } else {
        return {};
    }
}

vm::Instruction::Parameter appendParameter(vm::CodeFragment& code,
                                           CompileCtx& ctx,
                                           EExpression* e) {
    auto var = getFrameVariable(e);

    // If an expression is not a simple variable then we must generate code for it.
    if (!var) {
        code.append(e->compileDirect(ctx));
    }

    return getParam(var);
}

/**
 * Set of functions that allocate one or two labels, constructs code using 'f', and cleans up the
 * labels before returning the constructed code. These functions should be used when working with
 * labels in order to guarantee that the generated labels are destroyed _before_ returning the
 * generated code.
 */
vm::CodeFragment withNewLabel(CompileCtx& ctx, std::function<vm::CodeFragment(vm::LabelId)> f) {
    auto label = ctx.newLabelId();
    auto code = f(label);
    code.removeLabel(label);
    return code;
}

vm::CodeFragment withNewLabels(CompileCtx& ctx,
                               std::function<vm::CodeFragment(vm::LabelId, vm::LabelId)> f) {
    auto label1 = ctx.newLabelId();
    auto label2 = ctx.newLabelId();
    auto code = f(label1, label2);
    code.removeLabel(label1);
    code.removeLabel(label2);
    return code;
}

std::unique_ptr<vm::CodeFragment> EExpression::compile(CompileCtx& ctx) const {
    ctx.lastLabelId = 0;
    auto result = std::make_unique<vm::CodeFragment>(compileDirect(ctx));
    result->validate();
    return result;
}

std::string EExpression::toString() const {
    return DebugPrinter{}.print(debugPrint());
}

std::unique_ptr<EExpression> EConstant::clone() const {
    auto [tag, val] = value::copyValue(_tag, _val);
    return std::make_unique<EConstant>(tag, val);
}

vm::CodeFragment EConstant::compileDirect(CompileCtx& ctx) const {
    vm::CodeFragment code;

    code.appendConstVal(_tag, _val);

    return code;
}

std::vector<DebugPrinter::Block> EConstant::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    std::stringstream ss;
    value::ValuePrinters::make(ss,
                               PrintOptions().useTagForAmbiguousValues(true).normalizeOutput(true))
        .writeValueToStream(_tag, _val);

    ret.emplace_back(ss.str());

    return ret;
}

size_t EConstant::estimateSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_tag, _val);
    size += size_estimator::estimate(_nodes);
    return size;
}

std::unique_ptr<EExpression> EVariable::clone() const {
    return _frameId ? std::make_unique<EVariable>(*_frameId, _var, _moveFrom)
                    : std::make_unique<EVariable>(_var);
}

vm::CodeFragment EVariable::compileDirect(CompileCtx& ctx) const {
    vm::CodeFragment code;

    if (_frameId) {
        code.appendLocalVal(*_frameId, _var, _moveFrom);
    } else {
        // ctx.root is optional. If root stage is not specified, then resolve the variable using
        // default context rules.
        auto accessor = ctx.root ? ctx.root->getAccessor(ctx, _var) : ctx.getAccessor(_var);
        if (_moveFrom) {
            code.appendMoveVal(accessor);
        } else {
            code.appendAccessVal(accessor);
        }
    }

    return code;
}

std::vector<DebugPrinter::Block> EVariable::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    if (_moveFrom) {
        ret.emplace_back("move(`"_sd);
    }
    if (_frameId) {
        DebugPrinter::addIdentifier(ret, *_frameId, _var);
    } else {
        DebugPrinter::addIdentifier(ret, _var);
    }
    if (_moveFrom) {
        ret.emplace_back("`)"_sd);
    }

    return ret;
}

std::unique_ptr<EExpression> EPrimNary::clone() const {
    std::vector<std::unique_ptr<EExpression>> args;
    args.reserve(_nodes.size());
    for (auto& arg : _nodes) {
        args.emplace_back(arg->clone());
    }
    return std::make_unique<EPrimNary>(_op, std::move(args));
}

/*
 * Given a vector of clauses named [lhs1,...,lhsN-1, rhs], and a boolean isDisjunctive to indicate
 * whether we are ANDing or ORing the clauses, we output the appropriate short circuiting
 * CodeFragment. For AND (conjunctive) we compile them as following byte code:
 * @true1:    lhs1
 *            jumpNothing @end
 *            jumpFalse @false
 * ...
 * @trueN-1:  lhsN-1
 *            jumpNothing @end
 *            jumpFalse @false
 * @trueN:    rhs
 *            jmp @end
 * @false:    push false
 * @end:
 *
 * For OR (disjunctive) we compile them as:
 * @false1:   lhs1
 *            jumpNothing @end
 *            jumpTrue @true
 * ...
 * @falseN-1: lhsN-1
 *            jumpNothing @end
 *            jumpTrue @true
 * @tfalseN:  rhs
 *            jmp @end
 * @true:     push true
 * @end:
 */
template <typename Vector>
vm::CodeFragment buildShortCircuitCode(CompileCtx& ctx, const Vector& clauses, bool isDisjunction) {
    return withNewLabels(ctx, [&](vm::LabelId endLabel, vm::LabelId resultLabel) {
        // Build code fragment for all but the last clause, which is used for the final result
        // branch.
        tassert(7858700,
                "There should be two or more clauses when compiling a logicAnd/logicOr.",
                clauses.size() >= 2);
        vm::CodeFragment code;
        for (size_t i = 0; i < clauses.size() - 1; i++) {
            auto clauseCode = clauses.at(i)->compileDirect(ctx);
            clauseCode.appendLabelJumpNothing(endLabel);

            if (isDisjunction) {
                clauseCode.appendLabelJumpTrue(resultLabel);
            } else {
                clauseCode.appendLabelJumpFalse(resultLabel);
            }

            code.append(std::move(clauseCode));
        }

        // Build code fragment for final clause.
        auto finalClause = clauses.back()->compileDirect(ctx);
        finalClause.appendLabelJump(endLabel);

        // Build code fragment for the short-circuited result.
        vm::CodeFragment resultBranch;
        resultBranch.appendLabel(resultLabel);
        resultBranch.appendConstVal(value::TypeTags::Boolean,
                                    value::bitcastFrom<bool>(isDisjunction));

        // Only one of `finalClause` or `resultBranch` will execute, so the stack size adjustment
        // should only be made one time here, rather than one adjustment for each CodeFragment.
        code.append({std::move(finalClause), std::move(resultBranch)});
        code.appendLabel(endLabel);
        return code;
    });
}

vm::CodeFragment EPrimNary::compileDirect(CompileCtx& ctx) const {
    if (_op == EPrimNary::logicAnd || _op == EPrimNary::logicOr) {
        return buildShortCircuitCode(ctx, _nodes, _op == EPrimNary::logicOr /*isDisjunction*/);
    }

    vm::CodeFragment code;
    vm::Instruction::Parameter lhsParam, rhsParam;

    lhsParam = appendParameter(code, ctx, _nodes[0].get());

    auto appendInstr = [&](vm::Instruction::Parameter lhsParam,
                           vm::Instruction::Parameter rhsParam) {
        switch (_op) {
            case EPrimNary::add:
                code.appendAdd(lhsParam, rhsParam);
                break;
            case EPrimNary::mul:
                code.appendMul(lhsParam, rhsParam);
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(11122900);
        }
    };

    for (size_t idx = 1; idx < _nodes.size(); ++idx) {
        rhsParam = appendParameter(code, ctx, _nodes[idx].get());
        appendInstr(lhsParam, rhsParam);
        lhsParam = {};
    }

    return code;
}

std::vector<DebugPrinter::Block> EPrimNary::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    ret.emplace_back("(`");
    for (size_t i = 0; i < _nodes.size(); i++) {
        DebugPrinter::addBlocks(ret, _nodes[i]->debugPrint());
        if (i != _nodes.size() - 1) {
            switch (_op) {
                case EPrimNary::logicAnd:
                    ret.emplace_back("&&");
                    break;
                case EPrimNary::logicOr:
                    ret.emplace_back("||");
                    break;
                case EPrimNary::add:
                    ret.emplace_back("+");
                    break;
                case EPrimNary::mul:
                    ret.emplace_back("*");
                    break;
                default:
                    MONGO_UNREACHABLE_TASSERT(11122901);
            }
        }
    }
    ret.emplace_back("`)");

    return ret;
}

size_t EPrimNary::estimateSize() const {
    return sizeof(*this) + size_estimator::estimate(_nodes);
}

std::unique_ptr<EExpression> EPrimBinary::clone() const {
    if (_nodes.size() == 2) {
        return std::make_unique<EPrimBinary>(_op, _nodes[0]->clone(), _nodes[1]->clone());
    } else {
        tassert(11093400,
                "Unexpected number of nodes in binary primitive operation",
                _nodes.size() == 3);
        return std::make_unique<EPrimBinary>(
            _op, _nodes[0]->clone(), _nodes[1]->clone(), _nodes[2]->clone());
    }
}

vm::CodeFragment EPrimBinary::compileDirect(CompileCtx& ctx) const {
    const bool hasCollatorArg = (_nodes.size() == 3);

    tassert(11093401, "Operation is not a comparison", !hasCollatorArg || isComparisonOp(_op));

    if (_op == EPrimBinary::logicAnd) {
        auto clauses = collectAndClauses();
        return buildShortCircuitCode(ctx, clauses, false /*isDisjunction*/);
    } else if (_op == EPrimBinary::logicOr) {
        auto clauses = collectOrClauses();
        return buildShortCircuitCode(ctx, clauses, true /*isDisjunction*/);
    } else if (_op == EPrimBinary::fillEmpty) {
        // Special cases: rhs is trivial to evaluate -> avoid a jump
        if (EConstant* rhsConst = _nodes[1]->as<EConstant>()) {
            vm::CodeFragment code;
            auto [tag, val] = rhsConst->getConstant();
            if (tag == value::TypeTags::Null) {
                code.append(_nodes[0]->compileDirect(ctx));
                code.appendFillEmpty(vm::Instruction::Null);
                return code;
            }
            if (tag == value::TypeTags::Boolean) {
                code.append(_nodes[0]->compileDirect(ctx));
                code.appendFillEmpty(value::bitcastTo<bool>(val) ? vm::Instruction::True
                                                                 : vm::Instruction::False);
                return code;
            }
        }

        /*
         *            lhs
         *            jumpNotNothing end
         * @nothing:  pop
         *            rhs
         * @end:
         */
        return withNewLabel(ctx, [&](vm::LabelId endLabel) {
            vm::CodeFragment code;
            code.append(_nodes[0]->compileDirect(ctx));
            code.appendLabelJumpNotNothing(endLabel);

            code.appendPop();
            code.append(_nodes[1]->compileDirect(ctx));

            code.appendLabel(endLabel);
            return code;
        });
    }

    vm::CodeFragment code;
    vm::Instruction::Parameter collatorParam;

    if (hasCollatorArg) {
        collatorParam = appendParameter(code, ctx, _nodes[2].get());
    }
    vm::Instruction::Parameter lhsParam = appendParameter(code, ctx, _nodes[0].get());
    vm::Instruction::Parameter rhsParam = appendParameter(code, ctx, _nodes[1].get());

    switch (_op) {
        case EPrimBinary::add:
            code.appendAdd(lhsParam, rhsParam);
            break;
        case EPrimBinary::sub:
            code.appendSub(lhsParam, rhsParam);
            break;
        case EPrimBinary::mul:
            code.appendMul(lhsParam, rhsParam);
            break;
        case EPrimBinary::div:
            code.appendDiv(lhsParam, rhsParam);
            break;
        case EPrimBinary::less:
            hasCollatorArg ? code.appendCollLess(lhsParam, rhsParam, collatorParam)
                           : code.appendLess(lhsParam, rhsParam);
            break;
        case EPrimBinary::lessEq:
            hasCollatorArg ? code.appendCollLessEq(lhsParam, rhsParam, collatorParam)
                           : code.appendLessEq(lhsParam, rhsParam);
            break;
        case EPrimBinary::greater:
            hasCollatorArg ? code.appendCollGreater(lhsParam, rhsParam, collatorParam)
                           : code.appendGreater(lhsParam, rhsParam);
            break;
        case EPrimBinary::greaterEq:
            hasCollatorArg ? code.appendCollGreaterEq(lhsParam, rhsParam, collatorParam)
                           : code.appendGreaterEq(lhsParam, rhsParam);
            break;
        case EPrimBinary::eq:
            hasCollatorArg ? code.appendCollEq(lhsParam, rhsParam, collatorParam)
                           : code.appendEq(lhsParam, rhsParam);
            break;
        case EPrimBinary::neq:
            hasCollatorArg ? code.appendCollNeq(lhsParam, rhsParam, collatorParam)
                           : code.appendNeq(lhsParam, rhsParam);
            break;
        case EPrimBinary::cmp3w:
            hasCollatorArg ? code.appendCollCmp3w(lhsParam, rhsParam, collatorParam)
                           : code.appendCmp3w(lhsParam, rhsParam);
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(11122902);
    }
    return code;
}

std::vector<const EExpression*> EPrimBinary::collectOrClauses() const {
    tassert(11093402, "Unexpected operation type", _op == EPrimBinary::Op::logicOr);

    auto expandPredicate = [](const EExpression* expr) {
        const EPrimBinary* binaryExpr = expr->as<EPrimBinary>();
        return binaryExpr != nullptr && binaryExpr->_op == EPrimBinary::Op::logicOr;
    };

    std::vector<const EExpression*> acc;
    collectDescendants(expandPredicate, &acc);
    return acc;
}
std::vector<const EExpression*> EPrimBinary::collectAndClauses() const {
    auto expandPredicate = [](const EExpression* expr) {
        const EPrimBinary* binaryExpr = expr->as<EPrimBinary>();
        return binaryExpr != nullptr && binaryExpr->_op == EPrimBinary::Op::logicAnd;
    };

    std::vector<const EExpression*> acc;
    collectDescendants(expandPredicate, &acc);
    return acc;
}

std::vector<DebugPrinter::Block> EPrimBinary::debugPrint() const {
    bool hasCollatorArg = (_nodes.size() == 3);
    std::vector<DebugPrinter::Block> ret;

    tassert(11093403, "Operation is not a comparison", !hasCollatorArg || isComparisonOp(_op));

    ret.emplace_back("(`");
    DebugPrinter::addBlocks(ret, _nodes[0]->debugPrint());

    switch (_op) {
        case EPrimBinary::logicAnd:
            ret.emplace_back("&&");
            break;
        case EPrimBinary::logicOr:
            ret.emplace_back("||");
            break;
        case EPrimBinary::fillEmpty:
            // Sometimes called the "Elvis operator"...
            ret.emplace_back("?:");
            break;
        case EPrimBinary::add:
            ret.emplace_back("+");
            break;
        case EPrimBinary::sub:
            ret.emplace_back("-");
            break;
        case EPrimBinary::mul:
            ret.emplace_back("*");
            break;
        case EPrimBinary::div:
            ret.emplace_back("/");
            break;
        case EPrimBinary::less:
            ret.emplace_back("<");
            break;
        case EPrimBinary::lessEq:
            ret.emplace_back("<=");
            break;
        case EPrimBinary::greater:
            ret.emplace_back(">");
            break;
        case EPrimBinary::greaterEq:
            ret.emplace_back(">=");
            break;
        case EPrimBinary::eq:
            ret.emplace_back("==");
            break;
        case EPrimBinary::neq:
            ret.emplace_back("!=");
            break;
        case EPrimBinary::cmp3w:
            ret.emplace_back("<=>");
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(11122903);
    }

    if (hasCollatorArg) {
        ret.emplace_back("`[`");
        DebugPrinter::addBlocks(ret, _nodes[2]->debugPrint());
        ret.emplace_back("`]");
    }

    DebugPrinter::addBlocks(ret, _nodes[1]->debugPrint());
    ret.emplace_back("`)");

    return ret;
}

size_t EPrimBinary::estimateSize() const {
    return sizeof(*this) + size_estimator::estimate(_nodes);
}


std::unique_ptr<EExpression> EPrimUnary::clone() const {
    return std::make_unique<EPrimUnary>(_op, _nodes[0]->clone());
}

vm::CodeFragment EPrimUnary::compileDirect(CompileCtx& ctx) const {
    vm::CodeFragment code;

    auto param = appendParameter(code, ctx, _nodes[0].get());

    switch (_op) {
        case EPrimUnary::negate:
            code.appendNegate(param);
            break;
        case EPrimUnary::logicNot:
            code.appendNot(param);
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(11122904);
    }
    return code;
}

std::vector<DebugPrinter::Block> EPrimUnary::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    switch (_op) {
        case EPrimUnary::negate:
            ret.emplace_back("-");
            break;
        case EPrimUnary::logicNot:
            ret.emplace_back("!");
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(11122905);
    }

    ret.emplace_back("`(`");
    DebugPrinter::addBlocks(ret, _nodes[0]->debugPrint());
    ret.emplace_back("`)");

    return ret;
}

size_t EPrimUnary::estimateSize() const {
    return sizeof(*this) + size_estimator::estimate(_nodes);
}

std::unique_ptr<EExpression> EFunction::clone() const {
    Vector args;
    args.reserve(_nodes.size());
    for (auto& a : _nodes) {
        args.emplace_back(a->clone());
    }
    return std::make_unique<EFunction>(_fn, std::move(args));
}


namespace {
/**
 * The arity test function. It returns true if the number of arguments is correct.
 */
using ArityFn = bool (*)(size_t);

/**
 * The arity test function that trivially accepts any number of arguments.
 */
static constexpr ArityFn kAnyNumberOfArgs = [](size_t) {
    return true;
};

/**
 * The builtin function description.
 */
struct BuiltinFn {
    ArityFn arityTest;
    vm::Builtin builtin;
    bool aggregate;
};

/**
 * The map of recognized builtin functions.
 *
 * *************************************************************************************
 * IMPORTANT:
 *   Iff the third argument ('aggregate') to BuiltinFn{} is true, the actual arity
 *   WILL BE INCREMENTED BY THE COMPILER (EFunction::compileDirect()) and it will push
 *   an extra arg onto the stack (the accumulator value) for such fns. So an arityTest
 *   fn with body {return n == 1} for the arity test really means an arity-2 function.
 * *************************************************************************************
 */
// Note: EFn::kFail, EFn::kConvert, and EFn::kAggState are intentionally absent from both
// kBuiltinFunctions and kInstrFunctions. EFn::kFail is used as a sentinel "always-fail"
// expression in tests; EFn::kConvert is lowered to ENumericConvert by abt_lower.cpp before
// compilation and therefore never appears as an EFunction node; EFn::kAggState is handled by
// a dedicated special case in EFunction::compileDirect() rather than via the generic dispatch
// tables. Any EFn value not present in either table (and not handled by a special case) will
// trigger an "unknown function call" uassert in compileDirect() at runtime.
static stdx::unordered_map<EFn, BuiltinFn> kBuiltinFunctions = {
    {EFn::kDateDiff,
     BuiltinFn{[](size_t n) { return n == 5 || n == 6; }, vm::Builtin::dateDiff, false}},
    {EFn::kDateParts, BuiltinFn{[](size_t n) { return n == 9; }, vm::Builtin::dateParts, false}},
    {EFn::kDateToParts,
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::dateToParts, false}},
    {EFn::kIsoDateToParts,
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::isoDateToParts, false}},
    {EFn::kDayOfYear,
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::dayOfYear, false}},
    {EFn::kDayOfMonth,
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::dayOfMonth, false}},
    {EFn::kDayOfWeek,
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::dayOfWeek, false}},
    {EFn::kYear, BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::year, false}},
    {EFn::kMonth, BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::month, false}},
    {EFn::kHour, BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::hour, false}},
    {EFn::kMinute,
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::minute, false}},
    {EFn::kSecond,
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::second, false}},
    {EFn::kMillisecond,
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::millisecond, false}},
    {EFn::kWeek, BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::week, false}},
    {EFn::kIsoWeekYear,
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::isoWeekYear, false}},
    {EFn::kIsoDayOfWeek,
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::isoDayOfWeek, false}},
    {EFn::kIsoWeek,
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::isoWeek, false}},
    {EFn::kDatePartsWeekYear,
     BuiltinFn{[](size_t n) { return n == 9; }, vm::Builtin::datePartsWeekYear, false}},
    {EFn::kDateToString,
     BuiltinFn{[](size_t n) { return n == 4; }, vm::Builtin::dateToString, false}},
    {EFn::kDateFromString,
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::dateFromString, false}},
    {EFn::kDateFromStringNoThrow,
     BuiltinFn{
         [](size_t n) { return n == 3 || n == 4; }, vm::Builtin::dateFromStringNoThrow, false}},
    {EFn::kSplit, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::split, false}},
    {EFn::kRegexMatch, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::regexMatch, false}},
    {EFn::kReplaceOne, BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::replaceOne, false}},
    {EFn::kDropFields, BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::dropFields, false}},
    {EFn::kNewArray, BuiltinFn{kAnyNumberOfArgs, vm::Builtin::newArray, false}},
    {EFn::kKeepFields, BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::keepFields, false}},
    {EFn::kNewArrayFromRange,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::newArrayFromRange, false}},
    {EFn::kNewObj, BuiltinFn{[](size_t n) { return n % 2 == 0; }, vm::Builtin::newObj, false}},
    {EFn::kNewBsonObj,
     BuiltinFn{[](size_t n) { return n % 2 == 0; }, vm::Builtin::newBsonObj, false}},
    {EFn::kMakeObj, BuiltinFn{[](size_t n) { return n >= 2; }, vm::Builtin::makeObj, false}},
    {EFn::kMakeBsonObj,
     BuiltinFn{[](size_t n) { return n >= 2; }, vm::Builtin::makeBsonObj, false}},
    {EFn::kKs,
     BuiltinFn{[](size_t n) { return n >= 3 && n <= Ordering::kMaxCompoundIndexKeys + 3; },
               vm::Builtin::newKs,
               false}},
    {EFn::kCollKs,
     BuiltinFn{[](size_t n) { return n >= 4 && n < Ordering::kMaxCompoundIndexKeys + 4; },
               vm::Builtin::collNewKs,
               false}},
    {EFn::kAbs, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::abs, false}},
    {EFn::kCeil, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::ceil, false}},
    {EFn::kFloor, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::floor, false}},
    {EFn::kTrunc, BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::trunc, false}},
    {EFn::kExp, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::exp, false}},
    {EFn::kLn, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::ln, false}},
    {EFn::kLog10, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::log10, false}},
    {EFn::kSqrt, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::sqrt, false}},
    {EFn::kPow, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::pow, false}},
    {EFn::kAddToArray, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::addToArray, true}},
    {EFn::kAddToArrayCapped,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::addToArrayCapped, true}},
    {EFn::kMergeObjects,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::mergeObjects, true}},
    {EFn::kAddToSet, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::addToSet, true}},
    {EFn::kAddToSetCapped,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::addToSetCapped, true}},
    {EFn::kCollAddToSet,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::collAddToSet, true}},
    {EFn::kCollAddToSetCapped,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::collAddToSetCapped, true}},
    {EFn::kSetToArray, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::setToArray, false}},
    {EFn::kFillType, BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::fillType, false}},
    {EFn::kDoubleDoubleSum,
     BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::doubleDoubleSum, false}},
    {EFn::kConvertSimpleSumToDoubleDoubleSum,
     BuiltinFn{
         [](size_t n) { return n == 1; }, vm::Builtin::convertSimpleSumToDoubleDoubleSum, false}},
    {EFn::kAggDoubleDoubleSum,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggDoubleDoubleSum, true}},
    {EFn::kAggMergeDoubleDoubleSums,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggMergeDoubleDoubleSums, true}},
    {EFn::kDoubleDoubleSumFinalize,
     BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::doubleDoubleSumFinalize, false}},
    {EFn::kDoubleDoublePartialSumFinalize,
     BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::doubleDoublePartialSumFinalize, false}},
    {EFn::kAggStdDev, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggStdDev, true}},
    {EFn::kAggMergeStdDevs,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggMergeStdDevs, true}},
    {EFn::kStdDevPopFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::stdDevPopFinalize, false}},
    {EFn::kStdDevSampFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::stdDevSampFinalize, false}},
    {EFn::kBitTestZero,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::bitTestZero, false}},
    {EFn::kBitTestMask,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::bitTestMask, false}},
    {EFn::kBitTestPosition,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::bitTestPosition, false}},
    {EFn::kBsonSize, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::bsonSize, false}},
    {EFn::kStrLenBytes,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::strLenBytes, false}},
    {EFn::kStrLenCP, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::strLenCP, false}},
    {EFn::kSubstrBytes,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::substrBytes, false}},
    {EFn::kSubstrCP, BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::substrCP, false}},
    {EFn::kToLower, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::toLower, false}},
    {EFn::kToUpper, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::toUpper, false}},
    {EFn::kTrim, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::trim, false}},
    {EFn::kLtrim, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::ltrim, false}},
    {EFn::kRtrim, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::rtrim, false}},
    {EFn::kCoerceToBool,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::coerceToBool, false}},
    {EFn::kCoerceToString,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::coerceToString, false}},
    {EFn::kAcos, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::acos, false}},
    {EFn::kAcosh, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::acosh, false}},
    {EFn::kAsin, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::asin, false}},
    {EFn::kAsinh, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::asinh, false}},
    {EFn::kAtan, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::atan, false}},
    {EFn::kAtanh, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::atanh, false}},
    {EFn::kAtan2, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::atan2, false}},
    {EFn::kCos, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::cos, false}},
    {EFn::kCosh, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::cosh, false}},
    {EFn::kDegreesToRadians,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::degreesToRadians, false}},
    {EFn::kRadiansToDegrees,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::radiansToDegrees, false}},
    {EFn::kSin, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::sin, false}},
    {EFn::kSinh, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::sinh, false}},
    {EFn::kTan, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::tan, false}},
    {EFn::kTanh, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::tanh, false}},
    {EFn::kRand, BuiltinFn{[](size_t n) { return n == 0; }, vm::Builtin::rand, false}},
    {EFn::kRound, BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::round, false}},
    {EFn::kConcat, BuiltinFn{kAnyNumberOfArgs, vm::Builtin::concat, false}},
    {EFn::kConcatArrays, BuiltinFn{kAnyNumberOfArgs, vm::Builtin::concatArrays, false}},
    {EFn::kZipArrays, BuiltinFn{[](size_t n) { return n >= 2; }, vm::Builtin::zipArrays, false}},
    {EFn::kAggConcatArraysCapped,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggConcatArraysCapped, true}},
    {EFn::kIsMember, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::isMember, false}},
    {EFn::kCollIsMember,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::collIsMember, false}},
    {EFn::kIndexOfBytes,
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::indexOfBytes, false}},
    {EFn::kIndexOfCP,
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::indexOfCP, false}},
    {EFn::kIsDayOfWeek,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::isDayOfWeek, false}},
    {EFn::kIsTimeUnit, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::isTimeUnit, false}},
    {EFn::kIsTimezone, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::isTimezone, false}},
    {EFn::kIsValidToStringFormat,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::isValidToStringFormat, false}},
    {EFn::kValidateFromStringFormat,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::validateFromStringFormat, false}},
    {EFn::kSetUnion, BuiltinFn{kAnyNumberOfArgs, vm::Builtin::setUnion, false}},
    {EFn::kSetIntersection, BuiltinFn{kAnyNumberOfArgs, vm::Builtin::setIntersection, false}},
    {EFn::kSetDifference,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::setDifference, false}},
    {EFn::kSetEquals, BuiltinFn{[](size_t n) { return n >= 2; }, vm::Builtin::setEquals, false}},
    {EFn::kSetIsSubset,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::setIsSubset, false}},
    {EFn::kCollSetUnion,
     BuiltinFn{[](size_t n) { return n >= 1; }, vm::Builtin::collSetUnion, false}},
    {EFn::kCollSetIntersection,
     BuiltinFn{[](size_t n) { return n >= 1; }, vm::Builtin::collSetIntersection, false}},
    {EFn::kCollSetDifference,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::collSetDifference, false}},
    {EFn::kCollSetEquals,
     BuiltinFn{[](size_t n) { return n >= 3; }, vm::Builtin::collSetEquals, false}},
    {EFn::kCollSetIsSubset,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::collSetIsSubset, false}},
    {EFn::kAggSetUnion, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggSetUnion, true}},
    {EFn::kAggSetUnionCapped,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggSetUnionCapped, true}},
    {EFn::kAggCollSetUnion,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggCollSetUnion, true}},
    {EFn::kAggCollSetUnionCapped,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::aggCollSetUnionCapped, true}},
    {EFn::kSetUnionCapped,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::setUnionCapped, true}},
    {EFn::kCollSetUnionCapped,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::collSetUnionCapped, true}},
    {EFn::kConcatArraysCapped,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::concatArraysCapped, true}},
    {EFn::kRunJsPredicate,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::runJsPredicate, false}},
    {EFn::kRegexCompile,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::regexCompile, false}},
    {EFn::kRegexFind, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::regexFind, false}},
    {EFn::kRegexFindAll,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::regexFindAll, false}},
    {EFn::kGetRegexPattern,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::getRegexPattern, false}},
    {EFn::kGetRegexFlags,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::getRegexFlags, false}},
    {EFn::kShardFilter,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::shardFilter, false}},
    {EFn::kShardHash, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::shardHash, false}},
    {EFn::kExtractSubArray,
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::extractSubArray, false}},
    {EFn::kIsArrayEmpty,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::isArrayEmpty, false}},
    {EFn::kReverseArray,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::reverseArray, false}},
    {EFn::kSortArray,
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::sortArray, false}},
    {EFn::kTopN, BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::topN, false}},
    {EFn::kTop, BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::top, false}},
    {EFn::kBottomN,
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::bottomN, false}},
    {EFn::kBottom,
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::bottom, false}},
    {EFn::kDateAdd, BuiltinFn{[](size_t n) { return n == 5; }, vm::Builtin::dateAdd, false}},
    {EFn::kHasNullBytes,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::hasNullBytes, false}},
    {EFn::kHash, BuiltinFn{kAnyNumberOfArgs, vm::Builtin::hash, false}},
    {EFn::kFtsMatch, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::ftsMatch, false}},
    {EFn::kGenerateSortKey,
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::generateSortKey, false}},
    {EFn::kGenerateCheapSortKey,
     BuiltinFn{
         [](size_t n) { return n == 2 || n == 3; }, vm::Builtin::generateCheapSortKey, false}},
    {EFn::kSortKeyComponentVectorGetElement,
     BuiltinFn{
         [](size_t n) { return n == 2; }, vm::Builtin::sortKeyComponentVectorGetElement, false}},
    {EFn::kSortKeyComponentVectorToArray,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::sortKeyComponentVectorToArray, false}},
    {EFn::kTsSecond, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::tsSecond, false}},
    {EFn::kTsIncrement,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::tsIncrement, false}},
    {EFn::kTypeMatch, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::typeMatch, false}},
    {EFn::kDateTrunc, BuiltinFn{[](size_t n) { return n == 6; }, vm::Builtin::dateTrunc, false}},
    {EFn::kGetSortKeyAsc,
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::getSortKeyAsc, false}},
    {EFn::kGetSortKeyDesc,
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::getSortKeyDesc, false}},
    {EFn::kGetNonLeafSortKeyAsc,
     BuiltinFn{
         [](size_t n) { return n == 1 || n == 2; }, vm::Builtin::getNonLeafSortKeyAsc, false}},
    {EFn::kGetNonLeafSortKeyDesc,
     BuiltinFn{
         [](size_t n) { return n == 1 || n == 2; }, vm::Builtin::getNonLeafSortKeyDesc, false}},
    {EFn::kObjectToArray,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::objectToArray, false}},
    {EFn::kArrayToObject,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::arrayToObject, false}},
    {EFn::kUnwindArray,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::unwindArray, false}},
    {EFn::kArrayToSet, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::arrayToSet, false}},
    {EFn::kCollArrayToSet,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::collArrayToSet, false}},
    {EFn::kArray, BuiltinFn{kAnyNumberOfArgs, vm::Builtin::newArray, false}},
    {EFn::kAvgOfArray, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::avgOfArray, false}},
    {EFn::kMaxOfArray, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::maxOfArray, false}},
    {EFn::kMinOfArray, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::minOfArray, false}},
    {EFn::kStdDevPop, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::stdDevPop, false}},
    {EFn::kStdDevSamp, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::stdDevSamp, false}},
    {EFn::kSumOfArray, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::sumOfArray, false}},
    {EFn::kAggFirstNNeedsMoreInput,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggFirstNNeedsMoreInput, false}},
    {EFn::kAggFirstN, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggFirstN, false}},
    {EFn::kAggFirstNMerge,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggFirstNMerge, true}},
    {EFn::kAggFirstNFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggFirstNFinalize, false}},
    {EFn::kAggLastN, BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggLastN, true}},
    {EFn::kAggLastNMerge,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggLastNMerge, true}},
    {EFn::kAggLastNFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggLastNFinalize, false}},
    {EFn::kAggTopN, BuiltinFn{[](size_t n) { return n >= 3; }, vm::Builtin::aggTopN, true}},
    {EFn::kAggTopNArray,
     BuiltinFn{[](size_t n) { return n >= 2; }, vm::Builtin::aggTopNArray, true}},
    {EFn::kAggTopNMerge,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggTopNMerge, true}},
    {EFn::kAggTopNFinalize,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggTopNFinalize, false}},
    {EFn::kAggBottomN, BuiltinFn{[](size_t n) { return n >= 3; }, vm::Builtin::aggBottomN, true}},
    {EFn::kAggBottomNArray,
     BuiltinFn{[](size_t n) { return n >= 2; }, vm::Builtin::aggBottomNArray, true}},
    {EFn::kAggBottomNMerge,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggBottomNMerge, true}},
    {EFn::kAggBottomNFinalize,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggBottomNFinalize, false}},
    {EFn::kAggMaxN,
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::aggMaxN, true}},
    {EFn::kAggMaxNMerge,
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::aggMaxNMerge, true}},
    {EFn::kAggMaxNFinalize,
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::aggMaxNFinalize, false}},
    {EFn::kAggMinN,
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::aggMinN, true}},
    {EFn::kAggMinNMerge,
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::aggMinNMerge, true}},
    {EFn::kAggMinNFinalize,
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::aggMinNFinalize, false}},
    {EFn::kAggRank, BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRank, true}},
    {EFn::kAggRankColl, BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::aggRankColl, true}},
    {EFn::kAggDenseRank,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggDenseRank, true}},
    {EFn::kAggDenseRankColl,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::aggDenseRankColl, true}},
    {EFn::kAggRankFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRankFinalize, false}},
    {EFn::kAggExpMovingAvg,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggExpMovingAvg, true}},
    {EFn::kAggExpMovingAvgFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggExpMovingAvgFinalize, false}},
    {EFn::kAggRemovableSumAdd,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableSumAdd, true}},
    {EFn::kAggRemovableSumRemove,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableSumRemove, true}},
    {EFn::kAggRemovableSumFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableSumFinalize, false}},
    {EFn::kAggIntegralInit,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggIntegralInit, false}},
    {EFn::kAggIntegralAdd,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggIntegralAdd, true}},
    {EFn::kAggIntegralRemove,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggIntegralRemove, true}},
    {EFn::kAggIntegralFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggIntegralFinalize, false}},
    {EFn::kAggDerivativeFinalize,
     BuiltinFn{[](size_t n) { return n == 5; }, vm::Builtin::aggDerivativeFinalize, false}},
    {EFn::kAggCovarianceAdd,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggCovarianceAdd, true}},
    {EFn::kAggCovarianceRemove,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggCovarianceRemove, true}},
    {EFn::kAggCovarianceSampFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggCovarianceSampFinalize, false}},
    {EFn::kAggCovariancePopFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggCovariancePopFinalize, false}},
    {EFn::kAggRemovablePushAdd,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovablePushAdd, true}},
    {EFn::kAggRemovablePushRemove,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovablePushRemove, true}},
    {EFn::kAggRemovablePushFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovablePushFinalize, false}},
    {EFn::kAggRemovableConcatArraysInit,
     BuiltinFn{[](size_t n) { return n == 0; }, vm::Builtin::aggRemovableConcatArraysInit, false}},
    {EFn::kAggRemovableConcatArraysAdd,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableConcatArraysAdd, true}},
    {EFn::kAggRemovableConcatArraysRemove,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableConcatArraysRemove, true}},
    {EFn::kAggRemovableConcatArraysFinalize,
     BuiltinFn{
         [](size_t n) { return n == 1; }, vm::Builtin::aggRemovableConcatArraysFinalize, false}},
    {EFn::kAggRemovableStdDevAdd,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableStdDevAdd, true}},
    {EFn::kAggRemovableStdDevRemove,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableStdDevRemove, true}},
    {EFn::kAggRemovableStdDevSampFinalize,
     BuiltinFn{
         [](size_t n) { return n == 1; }, vm::Builtin::aggRemovableStdDevSampFinalize, false}},
    {EFn::kAggRemovableStdDevPopFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableStdDevPopFinalize, false}},
    {EFn::kAggRemovableAvgFinalize,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableAvgFinalize, false}},
    {EFn::kAggLinearFillCanAdd,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggLinearFillCanAdd, false}},
    {EFn::kAggLinearFillAdd,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggLinearFillAdd, true}},
    {EFn::kAggLinearFillFinalize,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggLinearFillFinalize, false}},
    {EFn::kAggRemovableFirstNInit,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableFirstNInit, false}},
    {EFn::kAggRemovableFirstNAdd,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableFirstNAdd, true}},
    {EFn::kAggRemovableFirstNRemove,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableFirstNRemove, true}},
    {EFn::kAggRemovableFirstNFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableFirstNFinalize, false}},
    {EFn::kAggRemovableLastNInit,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableLastNInit, false}},
    {EFn::kAggRemovableLastNAdd,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableLastNAdd, true}},
    {EFn::kAggRemovableLastNRemove,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableLastNRemove, true}},
    {EFn::kAggRemovableLastNFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableLastNFinalize, false}},
    {EFn::kAggRemovableSetCommonInit,
     BuiltinFn{[](size_t n) { return n == 0; }, vm::Builtin::aggRemovableSetCommonInit, false}},
    {EFn::kAggRemovableSetCommonCollInit,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableSetCommonCollInit, false}},
    {EFn::kAggRemovableAddToSetAdd,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableAddToSetAdd, true}},
    {EFn::kAggRemovableAddToSetRemove,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableAddToSetRemove, true}},
    {EFn::kAggRemovableSetUnionAdd,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableSetUnionAdd, true}},
    {EFn::kAggRemovableSetUnionRemove,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableSetUnionRemove, true}},
    {EFn::kAggRemovableSetCommonFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableSetCommonFinalize, false}},
    {EFn::kAggRemovableMinMaxNCollInit,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::aggRemovableMinMaxNCollInit, false}},
    {EFn::kAggRemovableMinMaxNInit,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableMinMaxNInit, false}},
    {EFn::kAggRemovableMinMaxNAdd,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableMinMaxNAdd, true}},
    {EFn::kAggRemovableMinMaxNRemove,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableMinMaxNRemove, true}},
    {EFn::kAggRemovableMinNFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableMinNFinalize, false}},
    {EFn::kAggRemovableMaxNFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableMaxNFinalize, false}},
    {EFn::kAggRemovableTopNInit,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableTopNInit, false}},
    {EFn::kAggRemovableTopNAdd,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableTopNAdd, true}},
    {EFn::kAggRemovableTopNRemove,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableTopNRemove, true}},
    {EFn::kAggRemovableTopNFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableTopNFinalize, false}},
    {EFn::kAggRemovableBottomNInit,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableBottomNInit, false}},
    {EFn::kAggRemovableBottomNAdd,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableBottomNAdd, true}},
    {EFn::kAggRemovableBottomNRemove,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableBottomNRemove, true}},
    {EFn::kAggRemovableBottomNFinalize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableBottomNFinalize, false}},
    {EFn::kValueBlockExists,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::valueBlockExists, false}},
    {EFn::kValueBlockTypeMatch,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockTypeMatch, false}},
    {EFn::kValueBlockIsTimezone,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockIsTimezone, false}},
    {EFn::kValueBlockFillEmpty,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockFillEmpty, false}},
    {EFn::kValueBlockFillEmptyBlock,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockFillEmptyBlock, false}},
    {EFn::kValueBlockFillType,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::valueBlockFillType, false}},
    {EFn::kValueBlockAggMin,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockAggMin, true}},
    {EFn::kValueBlockAggMax,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockAggMax, true}},
    {EFn::kValueBlockAggCount,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::valueBlockAggCount, true}},
    {EFn::kValueBlockAggSum,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockAggSum, true}},
    {EFn::kValueBlockAggDoubleDoubleSum,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockAggDoubleDoubleSum, true}},
    {EFn::kValueBlockAggTopN,
     BuiltinFn{[](size_t n) { return n >= 4; }, vm::Builtin::valueBlockAggTopN, true}},
    {EFn::kValueBlockAggTopNArray,
     BuiltinFn{[](size_t n) { return n >= 3; }, vm::Builtin::valueBlockAggTopNArray, true}},
    {EFn::kValueBlockAggBottomN,
     BuiltinFn{[](size_t n) { return n >= 4; }, vm::Builtin::valueBlockAggBottomN, true}},
    {EFn::kValueBlockAggBottomNArray,
     BuiltinFn{[](size_t n) { return n >= 3; }, vm::Builtin::valueBlockAggBottomNArray, true}},
    {EFn::kValueBlockDateDiff,
     BuiltinFn{[](size_t n) { return n == 6 || n == 7; }, vm::Builtin::valueBlockDateDiff, false}},
    {EFn::kValueBlockDateTrunc,
     BuiltinFn{[](size_t n) { return n == 7; }, vm::Builtin::valueBlockDateTrunc, false}},
    {EFn::kValueBlockDateAdd,
     BuiltinFn{[](size_t n) { return n == 6; }, vm::Builtin::valueBlockDateAdd, false}},
    {EFn::kValueBlockAdd,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::valueBlockAdd, false}},
    {EFn::kValueBlockSub,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::valueBlockSub, false}},
    {EFn::kValueBlockMult,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::valueBlockMult, false}},
    {EFn::kValueBlockDiv,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::valueBlockDiv, false}},
    {EFn::kValueBlockGtScalar,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockGtScalar, false}},
    {EFn::kValueBlockGteScalar,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockGteScalar, false}},
    {EFn::kValueBlockEqScalar,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockEqScalar, false}},
    {EFn::kValueBlockNeqScalar,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockNeqScalar, false}},
    {EFn::kValueBlockLtScalar,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockLtScalar, false}},
    {EFn::kValueBlockLteScalar,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockLteScalar, false}},
    {EFn::kValueBlockCmp3wScalar,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockCmp3wScalar, false}},
    {EFn::kValueBlockCombine,
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::valueBlockCombine, false}},
    {EFn::kValueBlockLogicalAnd,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockLogicalAnd, false}},
    {EFn::kValueBlockLogicalOr,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockLogicalOr, false}},
    {EFn::kValueBlockLogicalNot,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::valueBlockLogicalNot, false}},
    {EFn::kValueBlockNewFill,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockNewFill, false}},
    {EFn::kValueBlockSize,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::valueBlockSize, false}},
    {EFn::kValueBlockNone,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockNone, false}},
    {EFn::kValueBlockIsMember,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockIsMember, false}},
    {EFn::kValueBlockCoerceToBool,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::valueBlockCoerceToBool, false}},
    {EFn::kValueBlockTrunc,
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::valueBlockTrunc, false}},
    {EFn::kValueBlockRound,
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::valueBlockRound, false}},
    {EFn::kValueBlockMod,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockMod, false}},
    {EFn::kValueBlockConvert,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockConvert, false}},
    {EFn::kValueBlockGetSortKeyAsc,
     BuiltinFn{
         [](size_t n) { return n == 1 || n == 2; }, vm::Builtin::valueBlockGetSortKeyAsc, false}},
    {EFn::kValueBlockGetSortKeyDesc,
     BuiltinFn{
         [](size_t n) { return n == 1 || n == 2; }, vm::Builtin::valueBlockGetSortKeyDesc, false}},
    {EFn::kCellFoldValues_F,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::cellFoldValues_F, false}},
    {EFn::kCellFoldValues_P,
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::cellFoldValues_P, false}},
    {EFn::kCellBlockGetFlatValuesBlock,
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::cellBlockGetFlatValuesBlock, false}},
    {EFn::kCurrentDate,
     BuiltinFn{[](size_t n) { return n == 0; }, vm::Builtin::currentDate, false}},
};

/**
 * The code generation function.
 */
using CodeFnLegacy = void (vm::CodeFragment::*)();
using CodeFn = vm::CodeFragment (*)(CompileCtx&, const EExpression::Vector&, bool);

/**
 * The function description.
 */
struct InstrFn {
    size_t arity;
    CodeFn generate;
    bool aggregate;
};

template <CodeFnLegacy Appender>
vm::CodeFragment generatorLegacy(CompileCtx& ctx,
                                 const EExpression::Vector& nodes,
                                 bool aggregate) {
    vm::CodeFragment code;

    if (aggregate) {
        code.appendAccessVal(ctx.accumulator);
    }

    for (size_t idx = 0; idx < nodes.size(); ++idx) {
        code.append(nodes[idx]->compileDirect(ctx));
    }

    (code.*Appender)();

    return code;
}

void generatorCommon(vm::CodeFragment& code,
                     vm::Instruction::Parameter* params,
                     size_t arity,
                     CompileCtx& ctx,
                     const EExpression::Vector& nodes,
                     bool aggregate) {

    tassert(11096701,
            str::stream()
                << "Expect arity to match the number of nodes in expression but we have arity "
                << arity << " and nodes " << nodes.size(),
            nodes.size() == arity);

    if (aggregate) {
        code.appendAccessVal(ctx.accumulator);
    }
    for (size_t idx = 0; idx < arity; ++idx) {
        params[idx] = appendParameter(code, ctx, nodes[idx].get());
    }
}

template <size_t Arity, auto Appender>
vm::CodeFragment generator(CompileCtx& ctx, const EExpression::Vector& nodes, bool aggregate) {
    vm::CodeFragment code;
    vm::Instruction::Parameter params[Arity];

    generatorCommon(code, params, Arity, ctx, nodes, aggregate);

    if constexpr (Arity == 0) {
        (code.*Appender)();
    } else if constexpr (Arity == 1) {
        (code.*Appender)(params[0]);
    } else if constexpr (Arity == 2) {
        (code.*Appender)(params[0], params[1]);
    } else {
        static_assert(!Arity, "Missing specialization for Arity");
    }

    return code;
}

vm::CodeFragment generateGetField(CompileCtx& ctx, const EExpression::Vector& nodes, bool) {
    vm::CodeFragment code;

    if (nodes[1]->as<EConstant>()) {
        auto [tag, val] = nodes[1]->as<EConstant>()->getConstant();

        if (value::isString(tag)) {
            auto fieldName = value::getStringView(tag, val);
            if (fieldName.size() < vm::Instruction::kMaxInlineStringSize) {
                auto param = appendParameter(code, ctx, nodes[0].get());
                code.appendGetField(param, fieldName);

                return code;
            }
        }
    }

    vm::Instruction::Parameter params[2];
    generatorCommon(code, params, 2, ctx, nodes, false);
    code.appendGetField(params[0], params[1]);

    return code;
}

vm::CodeFragment generateTraverseP(CompileCtx& ctx, const EExpression::Vector& nodes, bool) {
    if (nodes[1]->as<ELocalLambda>() && nodes[2]->as<EConstant>()) {
        auto [tag, val] = nodes[2]->as<EConstant>()->getConstant();
        if ((tag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(val) == 1) ||
            tag == value::TypeTags::Nothing) {
            return withNewLabel(ctx, [&, tag = tag](vm::LabelId afterBodyLabel) {
                vm::CodeFragment code;
                auto lambda = nodes[1]->as<ELocalLambda>();

                // Jump around the body.
                code.appendLabelJump(afterBodyLabel);

                // Remember the position and append the body.
                auto bodyPosition = code.instrs().size();
                code.appendNoStack(lambda->compileBodyDirect(ctx));

                // Append the traverseP call.
                code.appendLabel(afterBodyLabel);
                code.append(nodes[0]->compileDirect(ctx));
                code.appendTraverseP(bodyPosition,
                                     lambda->numArguments(),
                                     tag == value::TypeTags::Nothing ? vm::Instruction::Nothing
                                                                     : vm::Instruction::Int32One);
                return code;
            });
        }
    }

    return generatorLegacy<&vm::CodeFragment::appendTraverseP>(ctx, nodes, false);
}

vm::CodeFragment generateTraverseF(CompileCtx& ctx, const EExpression::Vector& nodes, bool) {
    if (nodes[1]->as<ELocalLambda>() && nodes[2]->as<EConstant>()) {
        return withNewLabel(ctx, [&](vm::LabelId afterBodyLabel) {
            vm::CodeFragment code;
            auto lambda = nodes[1]->as<ELocalLambda>();
            auto [tag, val] = nodes[2]->as<EConstant>()->getConstant();

            // Jump around the body.
            code.appendLabelJump(afterBodyLabel);

            // Remember the position and append the body.
            auto bodyPosition = code.instrs().size();
            code.appendNoStack(lambda->compileBodyDirect(ctx));

            // Append the traverseF call.
            code.appendLabel(afterBodyLabel);
            code.append(nodes[0]->compileDirect(ctx));
            code.appendTraverseF(bodyPosition,
                                 lambda->numArguments(),
                                 value::bitcastTo<bool>(val) ? vm::Instruction::True
                                                             : vm::Instruction::False);
            return code;
        });
    }

    return generatorLegacy<&vm::CodeFragment::appendTraverseF>(ctx, nodes, false);
}

/**
 * The map of functions that resolve directly to instructions.
 */
static stdx::unordered_map<EFn, InstrFn> kInstrFunctions = {
    {EFn::kMakeOwn, InstrFn{1, generator<1, &vm::CodeFragment::appendMakeOwn>, false}},
    {EFn::kGetElement, InstrFn{2, generator<2, &vm::CodeFragment::appendGetElement>, false}},
    {EFn::kGetField, InstrFn{2, generateGetField, false}},
    {EFn::kGetArraySize, InstrFn{1, generator<1, &vm::CodeFragment::appendGetArraySize>, false}},
    {EFn::kCollComparisonKey,
     InstrFn{2, generator<2, &vm::CodeFragment::appendCollComparisonKey>, false}},
    {EFn::kGetFieldOrElement,
     InstrFn{2, generator<2, &vm::CodeFragment::appendGetFieldOrElement>, false}},
    {EFn::kTraverseP, InstrFn{3, generateTraverseP, false}},
    {EFn::kTraverseF, InstrFn{3, generateTraverseF, false}},
    {EFn::kMagicTraverseF,
     InstrFn{5, generatorLegacy<&vm::CodeFragment::appendMagicTraverseF>, false}},
    {EFn::kSetField, InstrFn{3, generatorLegacy<&vm::CodeFragment::appendSetField>, false}},
    {EFn::kSum, InstrFn{1, generatorLegacy<&vm::CodeFragment::appendSum>, true}},
    {EFn::kCount, InstrFn{0, generatorLegacy<&vm::CodeFragment::appendCount>, true}},
    {EFn::kMin, InstrFn{1, generatorLegacy<&vm::CodeFragment::appendMin>, true}},
    {EFn::kMax, InstrFn{1, generatorLegacy<&vm::CodeFragment::appendMax>, true}},
    {EFn::kFirst, InstrFn{1, generatorLegacy<&vm::CodeFragment::appendFirst>, true}},
    {EFn::kLast, InstrFn{1, generatorLegacy<&vm::CodeFragment::appendLast>, true}},
    {EFn::kCollMin, InstrFn{2, generatorLegacy<&vm::CodeFragment::appendCollMin>, true}},
    {EFn::kCollMax, InstrFn{2, generatorLegacy<&vm::CodeFragment::appendCollMax>, true}},
    {EFn::kExists, InstrFn{1, generator<1, &vm::CodeFragment::appendExists>, false}},
    {EFn::kMod, InstrFn{2, generator<2, &vm::CodeFragment::appendMod>, false}},
    {EFn::kIsDate, InstrFn{1, generator<1, &vm::CodeFragment::appendIsDate>, false}},
    {EFn::kIsNumber, InstrFn{1, generator<1, &vm::CodeFragment::appendIsNumber>, false}},
    {EFn::kIsNull, InstrFn{1, generator<1, &vm::CodeFragment::appendIsNull>, false}},
    {EFn::kIsObject, InstrFn{1, generator<1, &vm::CodeFragment::appendIsObject>, false}},
    {EFn::kIsArray, InstrFn{1, generator<1, &vm::CodeFragment::appendIsArray>, false}},
    {EFn::kIsInList, InstrFn{1, generator<1, &vm::CodeFragment::appendIsInList>, false}},
    {EFn::kIsString, InstrFn{1, generator<1, &vm::CodeFragment::appendIsString>, false}},
    {EFn::kIsBinData, InstrFn{1, generator<1, &vm::CodeFragment::appendIsBinData>, false}},
    {EFn::kIsNaN, InstrFn{1, generator<1, &vm::CodeFragment::appendIsNaN>, false}},
    {EFn::kIsInfinity, InstrFn{1, generator<1, &vm::CodeFragment::appendIsInfinity>, false}},
    {EFn::kIsRecordId, InstrFn{1, generator<1, &vm::CodeFragment::appendIsRecordId>, false}},
    {EFn::kIsMinKey, InstrFn{1, generator<1, &vm::CodeFragment::appendIsMinKey>, false}},
    {EFn::kIsMaxKey, InstrFn{1, generator<1, &vm::CodeFragment::appendIsMaxKey>, false}},
    {EFn::kIsTimestamp, InstrFn{1, generator<1, &vm::CodeFragment::appendIsTimestamp>, false}},
    {EFn::kIsKeyString, InstrFn{1, generator<1, &vm::CodeFragment::appendIsKeyString>, false}},
    {EFn::kValueBlockApplyLambda,
     InstrFn{3, generatorLegacy<&vm::CodeFragment::appendValueBlockApplyLambda>, false}},
};
}  // namespace

vm::CodeFragment EFunction::compileDirect(CompileCtx& ctx) const {
    // Built-in function compilations.
    if (auto it = kBuiltinFunctions.find(_fn); it != kBuiltinFunctions.end()) {
        auto arity = _nodes.size();
        if (!it->second.arityTest(arity)) {
            uasserted(4822843,
                      str::stream() << "function call: " << sbe::toString(_fn)
                                    << " has wrong arity: " << arity);
        }
        vm::CodeFragment code;

        // Optimize well known set of functions with constant arguments and generate their
        // specialized variants.
        if (_fn == EFn::kTypeMatch && _nodes[1]->as<EConstant>()) {
            auto [tag, val] = _nodes[1]->as<EConstant>()->getConstant();
            if (tag == value::TypeTags::NumberInt32) {
                auto mask = static_cast<uint32_t>(value::bitcastTo<int32_t>(val));
                auto param = appendParameter(code, ctx, _nodes[0].get());
                code.appendTypeMatch(param, mask);

                return code;
            }
        } else if (_fn == EFn::kDateTrunc && _nodes[2]->as<EConstant>() &&
                   _nodes[3]->as<EConstant>() && _nodes[4]->as<EConstant>() &&
                   _nodes[5]->as<EConstant>()) {
            // The validation for the arguments has been omitted here because the constants
            // have already been validated in the stage builder.
            auto [timezoneDBTag, timezoneDBVal] =
                ctx.getRuntimeEnvAccessor(_nodes[0]->as<EVariable>()->getSlotId())
                    ->getViewOfValue();
            auto timezoneDB = value::getTimeZoneDBView(timezoneDBVal);

            auto [unitTag, unitVal] = _nodes[2]->as<EConstant>()->getConstant();
            auto unitString = value::getStringView(unitTag, unitVal);
            auto unit = parseTimeUnit(unitString);

            auto [binSizeTag, binSizeValue] = _nodes[3]->as<EConstant>()->getConstant();
            auto [binSizeLongOwn, binSizeLongTag, binSizeLongValue] =
                genericNumConvert(binSizeTag, binSizeValue, value::TypeTags::NumberInt64);
            auto binSize = value::bitcastTo<int64_t>(binSizeLongValue);

            auto [timezoneTag, timezoneVal] = _nodes[4]->as<EConstant>()->getConstant();
            auto timezone = vm::getTimezone(timezoneTag, timezoneVal, timezoneDB);

            DayOfWeek startOfWeek{kStartOfWeekDefault};
            if (unit == TimeUnit::week) {
                auto [startOfWeekTag, startOfWeekVal] = _nodes[5]->as<EConstant>()->getConstant();
                auto startOfWeekString = value::getStringView(startOfWeekTag, startOfWeekVal);
                startOfWeek = parseDayOfWeek(startOfWeekString);
            }

            code.append(_nodes[1]->compileDirect(ctx));
            code.appendDateTrunc(unit, binSize, timezone, startOfWeek);
            return code;
        }

        for (size_t idx = arity; idx-- > 0;) {
            code.append(_nodes[idx]->compileDirect(ctx));
        }

        if (it->second.aggregate) {
            uassert(4822844,
                    str::stream() << "aggregate function call: " << sbe::toString(_fn)
                                  << " occurs in the non-aggregate context.",
                    ctx.aggExpression && ctx.accumulator);

            code.appendMoveVal(ctx.accumulator);
            ++arity;
        }

        code.appendFunction(it->second.builtin, arity);

        return code;
    }  // if built-in function

    // Instruction compilations from here down.
    if (auto it = kInstrFunctions.find(_fn); it != kInstrFunctions.end()) {
        if (it->second.arity != _nodes.size()) {
            uasserted(4822845,
                      str::stream() << "function call: " << sbe::toString(_fn)
                                    << " has wrong arity: " << _nodes.size());
        }
        if (it->second.aggregate) {
            uassert(4822846,
                    str::stream() << "aggregate function call: " << sbe::toString(_fn)
                                  << " occurs in the non-aggregate context.",
                    ctx.aggExpression && ctx.accumulator);
        }

        return (it->second.generate)(ctx, _nodes, it->second.aggregate);
    }

    if (_fn == EFn::kAggState) {
        uassert(7695204,
                "aggregate function call: aggState occurs in the non-aggregate context.",
                ctx.aggExpression && ctx.accumulator);
        uassert(7695205,
                str::stream() << "function call: aggState has wrong arity: " << _nodes.size(),
                _nodes.size() == 0);
        vm::CodeFragment code;
        code.appendMoveVal(ctx.accumulator);
        return code;
    }

    uasserted(4822847, str::stream() << "unknown function call: " << sbe::toString(_fn));
}  // compileDirect

std::vector<DebugPrinter::Block> EFunction::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    DebugPrinter::addKeyword(ret, sbe::toString(_fn));

    ret.emplace_back("`(`");
    for (size_t idx = 0; idx < _nodes.size(); ++idx) {
        if (idx) {
            ret.emplace_back("`,");
        }

        DebugPrinter::addBlocks(ret, _nodes[idx]->debugPrint());
    }
    ret.emplace_back("`)");

    return ret;
}

size_t EFunction::estimateSize() const {
    return sizeof(*this) + size_estimator::estimate(_nodes);
}

std::unique_ptr<EExpression> EIf::clone() const {
    return std::make_unique<EIf>(_nodes[0]->clone(), _nodes[1]->clone(), _nodes[2]->clone());
}

vm::CodeFragment EIf::compileDirect(CompileCtx& ctx) const {
    /*
     * Compile if-then-else into following bytecode:
     *            cond
     *            jumpNothing @end
     *            jumpTrue @then
     *            elseBranch
     *            jump @end
     * @then:     thenBranch
     * @end:
     */
    return withNewLabels(ctx, [&](vm::LabelId endLabel, vm::LabelId thenLabel) {
        // Compile the condition
        auto code = _nodes[0]->compileDirect(ctx);

        // Compile the jumps
        code.appendLabelJumpNothing(endLabel);
        code.appendLabelJumpTrue(thenLabel);

        // Compile else-branch
        auto elseCodeBranch = _nodes[2]->compileDirect(ctx);
        elseCodeBranch.appendLabelJump(endLabel);

        // Compile then-branch
        vm::CodeFragment thenCodeBranch;
        thenCodeBranch.appendLabel(thenLabel);
        thenCodeBranch.append(_nodes[1]->compileDirect(ctx));

        // Combine the branches
        code.append({std::move(elseCodeBranch), std::move(thenCodeBranch)});
        code.appendLabel(endLabel);
        return code;
    });
}

std::vector<DebugPrinter::Block> EIf::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);

    // Print the condition.
    DebugPrinter::addKeyword(ret, "if");
    DebugPrinter::addBlocks(ret, _nodes[0]->debugPrint());
    DebugPrinter::addNewLine(ret);

    // Print thenBranch.
    DebugPrinter::addKeyword(ret, "then");
    DebugPrinter::addBlocks(ret, _nodes[1]->debugPrint());
    DebugPrinter::addNewLine(ret);

    // Print elseBranch.
    DebugPrinter::addKeyword(ret, "else");
    DebugPrinter::addBlocks(ret, _nodes[2]->debugPrint());

    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    return ret;
}

size_t EIf::estimateSize() const {
    return sizeof(*this) + size_estimator::estimate(_nodes);
}

std::unique_ptr<EExpression> ESwitch::clone() const {
    std::vector<std::unique_ptr<EExpression>> nodes;
    nodes.reserve(_nodes.size());
    for (auto&& n : _nodes) {
        nodes.push_back(n->clone());
    }
    return std::make_unique<ESwitch>(std::move(nodes));
}

vm::CodeFragment ESwitch::compileDirect(CompileCtx& ctx) const {
    /*
     * Compile if-then-elif-...-else into following bytecode:
     *            cond1
     *            jumpNothing @end
     *            jumpTrue @then1
     *            cond2
     *            jumpNothing @end
     *            jumpTrue @then2
     *            elseBranch
     *            jump @end
     * @then1:    thenBranch1
     *            jump @end
     * @then2:    thenBranch2
     * @end:
     */

    auto endLabel = ctx.newLabelId();
    size_t numBranches = getNumBranches();
    std::vector<vm::LabelId> labels;
    labels.reserve(numBranches);
    for (size_t i = 0; i < numBranches; i++) {
        labels.push_back(ctx.newLabelId());
    }
    std::vector<vm::CodeFragment> fragments;
    fragments.reserve(numBranches + 1);

    vm::CodeFragment mainCode;
    for (size_t i = 0; i < numBranches; i++) {
        // Compile the condition
        auto code = getCondition(i)->compileDirect(ctx);
        // Compile the jumps
        code.appendLabelJumpNothing(endLabel);
        code.appendLabelJumpTrue(labels[i]);
        mainCode.append(std::move(code));
    }
    // Compile else-branch
    fragments.emplace_back(getDefault()->compileDirect(ctx));
    fragments.back().appendLabelJump(endLabel);
    // Compile then-branch
    for (size_t i = 0; i < numBranches; i++) {
        vm::CodeFragment thenCodeBranch;
        thenCodeBranch.appendLabel(labels[i]);
        thenCodeBranch.append(getThenBranch(i)->compileDirect(ctx));
        if (i < numBranches - 1) {
            thenCodeBranch.appendLabelJump(endLabel);
        }
        fragments.emplace_back(std::move(thenCodeBranch));
    }

    mainCode.append(std::move(fragments));
    mainCode.appendLabel(endLabel);

    for (size_t i = 0; i < numBranches; i++) {
        mainCode.removeLabel(labels[i]);
    }
    mainCode.removeLabel(endLabel);
    return mainCode;
}

std::vector<DebugPrinter::Block> ESwitch::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);

    for (size_t i = 0; i < getNumBranches(); i++) {
        // Print the condition.
        DebugPrinter::addKeyword(ret, i == 0 ? "if" : "elif");
        DebugPrinter::addBlocks(ret, getCondition(i)->debugPrint());
        DebugPrinter::addNewLine(ret);

        // Print thenBranch.
        DebugPrinter::addKeyword(ret, "then");
        DebugPrinter::addBlocks(ret, getThenBranch(i)->debugPrint());
        DebugPrinter::addNewLine(ret);
    }
    // Print elseBranch.
    DebugPrinter::addKeyword(ret, "else");
    DebugPrinter::addBlocks(ret, getDefault()->debugPrint());

    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    return ret;
}

size_t ESwitch::estimateSize() const {
    return sizeof(*this) + size_estimator::estimate(_nodes);
}

std::unique_ptr<EExpression> ELocalBind::clone() const {
    Vector binds;
    binds.reserve(_nodes.size() - 1);
    for (size_t idx = 0; idx < _nodes.size() - 1; ++idx) {
        binds.emplace_back(_nodes[idx]->clone());
    }
    return std::make_unique<ELocalBind>(_frameId, std::move(binds), _nodes.back()->clone());
}

vm::CodeFragment ELocalBind::compileDirect(CompileCtx& ctx) const {
    vm::CodeFragment code;

    // Declare the frame at the top of the stack, where the local variable values will reside.
    code.declareFrame(_frameId);

    // Generate bytecode for local variables and the 'in' expression. The 'in' expression is in the
    // last position of _nodes.
    for (size_t idx = 0; idx < _nodes.size(); ++idx) {
        code.append(_nodes[idx]->compileDirect(ctx));
    }

    // After the execution we have to cleanup the stack; i.e. local variables go out of scope.
    // However, note that the top of the stack holds the overall result (i.e. the 'in' expression)
    // and it cannot be destroyed. So we 'bubble' it down with a series of swap/pop instructions.
    for (size_t idx = 0; idx < _nodes.size() - 1; ++idx) {
        code.appendSwap();
        code.appendPop();
    }

    // Local variables are no longer accessible after this point so remove the frame.
    code.removeFrame(_frameId);
    return code;
}

std::vector<DebugPrinter::Block> ELocalBind::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);

    DebugPrinter::addKeyword(ret, "let");
    ret.emplace_back("[`");
    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);

    for (size_t idx = 0; idx < _nodes.size() - 1; ++idx) {
        if (idx != 0) {
            DebugPrinter::addNewLine(ret);
        }

        DebugPrinter::addIdentifier(ret, _frameId, idx);
        ret.emplace_back("=");
        DebugPrinter::addBlocks(ret, _nodes[idx]->debugPrint());
    }
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);
    ret.emplace_back("]");
    DebugPrinter::addNewLine(ret);

    DebugPrinter::addKeyword(ret, "in");
    DebugPrinter::addBlocks(ret, _nodes.back()->debugPrint());

    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    return ret;
}

size_t ELocalBind::estimateSize() const {
    return sizeof(*this) + size_estimator::estimate(_nodes);
}

std::unique_ptr<EExpression> ELocalLambda::clone() const {
    return std::make_unique<ELocalLambda>(_frameId, _nodes.back()->clone(), _numArguments);
}

vm::CodeFragment ELocalLambda::compileBodyDirect(CompileCtx& ctx) const {
    // Compile the body first so we know its size.
    auto inner = _nodes.back()->compileDirect(ctx);
    vm::CodeFragment body;
    // Declare the frame containing lambda variable.
    // The variables are expected to be already on the stack so declare the frame just below the
    // current top of the stack.
    body.declareFrame(_frameId, -(int)_numArguments);

    // Make sure the stack is sufficiently large.
    body.appendAllocStack(inner.maxStackSize());
    body.append(std::move(inner));
    body.appendRet();
    invariant(body.stackSize() == 1);

    // Lambda parameter is no longer accessible after this point so remove the frame.
    body.removeFrame(_frameId);

    // Verify that 'body' does not refer to local variables defined outside of 'body'.
    tassert(7284301,
            "Lambda referring to local variable defined outside of the lambda is not supported.",
            !body.hasFrames());

    return body;
}

vm::CodeFragment ELocalLambda::compileDirect(CompileCtx& ctx) const {
    return withNewLabel(ctx, [&](vm::LabelId afterBodyLabel) {
        vm::CodeFragment code;

        // Jump around the body.
        code.appendLabelJump(afterBodyLabel);

        // Remember the position and append the body.
        auto bodyPosition = code.instrs().size();
        code.appendNoStack(compileBodyDirect(ctx));

        // Push the lambda value on the stack
        code.appendLabel(afterBodyLabel);
        code.appendLocalLambda(bodyPosition, _numArguments);

        return code;
    });
}

std::vector<DebugPrinter::Block> ELocalLambda::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    DebugPrinter::addKeyword(ret, "lambda");
    ret.emplace_back("`(`");
    for (size_t i = 0; i < _numArguments; i++) {
        DebugPrinter::addIdentifier(ret, _frameId, i);
    }
    ret.emplace_back("`)");
    ret.emplace_back("{");
    DebugPrinter::addBlocks(ret, _nodes.back()->debugPrint());
    ret.emplace_back("}");

    return ret;
}

size_t ELocalLambda::estimateSize() const {
    return sizeof(*this) + size_estimator::estimate(_nodes);
}


std::unique_ptr<EExpression> EFail::clone() const {
    return std::make_unique<EFail>(_code, getStringView(_messageTag, _messageVal));
}

vm::CodeFragment EFail::compileDirect(CompileCtx& ctx) const {
    vm::CodeFragment code;

    code.appendConstVal(value::TypeTags::NumberInt64,
                        value::bitcastFrom<int64_t>(static_cast<int64_t>(_code)));

    code.appendConstVal(_messageTag, _messageVal);

    code.appendFail();

    return code;
}

std::vector<DebugPrinter::Block> EFail::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    DebugPrinter::addKeyword(ret, "fail");

    ret.emplace_back("`(`");

    ret.emplace_back(std::to_string(_code));
    ret.emplace_back("`,");
    ret.emplace_back("\"`");
    ret.emplace_back(getStringView(_messageTag, _messageVal));
    ret.emplace_back("`\"`");

    ret.emplace_back("`)");

    return ret;
}

size_t EFail::estimateSize() const {
    return sizeof(*this) + size_estimator::estimate(_messageTag, _messageVal) +
        size_estimator::estimate(_nodes);
}

std::unique_ptr<EExpression> ENumericConvert::clone() const {
    return std::make_unique<ENumericConvert>(_nodes[0]->clone(), _target);
}

vm::CodeFragment ENumericConvert::compileDirect(CompileCtx& ctx) const {
    auto code = _nodes[0]->compileDirect(ctx);
    code.appendNumericConvert(_target);

    return code;
}

std::vector<DebugPrinter::Block> ENumericConvert::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    DebugPrinter::addKeyword(ret, "convert");

    ret.emplace_back("(");

    DebugPrinter::addBlocks(ret, _nodes[0]->debugPrint());

    ret.emplace_back("`,");

    switch (_target) {
        case value::TypeTags::NumberInt32:
            ret.emplace_back("int32");
            break;
        case value::TypeTags::NumberInt64:
            ret.emplace_back("int64");
            break;
        case value::TypeTags::NumberDouble:
            ret.emplace_back("double");
            break;
        case value::TypeTags::NumberDecimal:
            ret.emplace_back("decimal");
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(11122906);
    }

    ret.emplace_back("`)");
    return ret;
}

size_t ENumericConvert::estimateSize() const {
    return sizeof(*this) + size_estimator::estimate(_nodes);
}
}  // namespace sbe
}  // namespace mongo
