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

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <functional>
#include <sstream>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>

#include "mongo/bson/ordering.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/print_options.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/exec/sbe/vm/datetime.h"
#include "mongo/db/exec/sbe/vm/label.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

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

std::unique_ptr<EExpression> EPrimBinary::clone() const {
    if (_nodes.size() == 2) {
        return std::make_unique<EPrimBinary>(_op, _nodes[0]->clone(), _nodes[1]->clone());
    } else {
        invariant(_nodes.size() == 3);
        return std::make_unique<EPrimBinary>(
            _op, _nodes[0]->clone(), _nodes[1]->clone(), _nodes[2]->clone());
    }
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
vm::CodeFragment buildShortCircuitCode(CompileCtx& ctx,
                                       const std::vector<const EExpression*>& clauses,
                                       bool isDisjunction) {
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
        code.append(std::move(finalClause), std::move(resultBranch));
        code.appendLabel(endLabel);
        return code;
    });
}

vm::CodeFragment EPrimBinary::compileDirect(CompileCtx& ctx) const {
    const bool hasCollatorArg = (_nodes.size() == 3);

    invariant(!hasCollatorArg || isComparisonOp(_op));

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
            MONGO_UNREACHABLE;
    }
    return code;
}

std::vector<const EExpression*> EPrimBinary::collectOrClauses() const {
    invariant(_op == EPrimBinary::Op::logicOr);

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

    invariant(!hasCollatorArg || isComparisonOp(_op));

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
            MONGO_UNREACHABLE;
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
        case negate:
            code.appendNegate(param);
            break;
        case EPrimUnary::logicNot:
            code.appendNot(param);
            break;
        default:
            MONGO_UNREACHABLE;
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
            MONGO_UNREACHABLE;
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
    return std::make_unique<EFunction>(_name, std::move(args));
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
 */
static stdx::unordered_map<std::string, BuiltinFn> kBuiltinFunctions = {
    {"dateDiff",
     BuiltinFn{[](size_t n) { return n == 5 || n == 6; }, vm::Builtin::dateDiff, false}},
    {"dateParts", BuiltinFn{[](size_t n) { return n == 9; }, vm::Builtin::dateParts, false}},
    {"dateToParts",
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::dateToParts, false}},
    {"isoDateToParts",
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::isoDateToParts, false}},
    {"dayOfYear",
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::dayOfYear, false}},
    {"dayOfMonth",
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::dayOfMonth, false}},
    {"dayOfWeek",
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::dayOfWeek, false}},
    {"year", BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::year, false}},
    {"month", BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::month, false}},
    {"hour", BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::hour, false}},
    {"minute", BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::minute, false}},
    {"second", BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::second, false}},
    {"millisecond",
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::millisecond, false}},
    {"week", BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::week, false}},
    {"isoWeekYear",
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::isoWeekYear, false}},
    {"isoDayOfWeek",
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::isoDayOfWeek, false}},
    {"isoWeek", BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::isoWeek, false}},
    {"datePartsWeekYear",
     BuiltinFn{[](size_t n) { return n == 9; }, vm::Builtin::datePartsWeekYear, false}},
    {"dateToString", BuiltinFn{[](size_t n) { return n == 4; }, vm::Builtin::dateToString, false}},
    {"dateFromString",
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::dateFromString, false}},
    {"dateFromStringNoThrow",
     BuiltinFn{
         [](size_t n) { return n == 3 || n == 4; }, vm::Builtin::dateFromStringNoThrow, false}},
    {"split", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::split, false}},
    {"regexMatch", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::regexMatch, false}},
    {"replaceOne", BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::replaceOne, false}},
    {"dropFields", BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::dropFields, false}},
    {"newArray", BuiltinFn{kAnyNumberOfArgs, vm::Builtin::newArray, false}},
    {"keepFields", BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::keepFields, false}},
    {"newArrayFromRange",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::newArrayFromRange, false}},
    {"newObj", BuiltinFn{[](size_t n) { return n % 2 == 0; }, vm::Builtin::newObj, false}},
    {"newBsonObj", BuiltinFn{[](size_t n) { return n % 2 == 0; }, vm::Builtin::newBsonObj, false}},
    {"makeBsonObj", BuiltinFn{[](size_t n) { return n >= 3; }, vm::Builtin::makeBsonObj, false}},
    {"ksToString", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::ksToString, false}},
    {"ks",
     BuiltinFn{[](size_t n) { return n >= 3 && n <= Ordering::kMaxCompoundIndexKeys + 3; },
               vm::Builtin::newKs,
               false}},
    {"collKs",
     BuiltinFn{[](size_t n) { return n >= 4 && n < Ordering::kMaxCompoundIndexKeys + 4; },
               vm::Builtin::collNewKs,
               false}},
    {"abs", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::abs, false}},
    {"ceil", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::ceil, false}},
    {"floor", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::floor, false}},
    {"trunc", BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::trunc, false}},
    {"exp", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::exp, false}},
    {"ln", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::ln, false}},
    {"log10", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::log10, false}},
    {"sqrt", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::sqrt, false}},
    {"pow", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::pow, false}},
    {"addToArray", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::addToArray, true}},
    {"addToArrayCapped",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::addToArrayCapped, true}},
    {"mergeObjects", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::mergeObjects, true}},
    {"addToSet", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::addToSet, true}},
    {"addToSetCapped",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::addToSetCapped, true}},
    {"collAddToSet", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::collAddToSet, true}},
    {"collAddToSetCapped",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::collAddToSetCapped, true}},
    {"setToArray", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::setToArray, false}},
    {"doubleDoubleSum",
     BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::doubleDoubleSum, false}},
    {"convertSimpleSumToDoubleDoubleSum",
     BuiltinFn{
         [](size_t n) { return n == 1; }, vm::Builtin::convertSimpleSumToDoubleDoubleSum, true}},
    {"aggDoubleDoubleSum",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggDoubleDoubleSum, true}},
    {"aggMergeDoubleDoubleSums",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggMergeDoubleDoubleSums, true}},
    {"doubleDoubleSumFinalize",
     BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::doubleDoubleSumFinalize, false}},
    {"doubleDoublePartialSumFinalize",
     BuiltinFn{[](size_t n) { return n > 0; }, vm::Builtin::doubleDoublePartialSumFinalize, false}},
    {"aggStdDev", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggStdDev, true}},
    {"aggMergeStdDevs",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggMergeStdDevs, true}},
    {"stdDevPopFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::stdDevPopFinalize, false}},
    {"stdDevSampFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::stdDevSampFinalize, false}},
    {"bitTestZero", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::bitTestZero, false}},
    {"bitTestMask", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::bitTestMask, false}},
    {"bitTestPosition",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::bitTestPosition, false}},
    {"bsonSize", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::bsonSize, false}},
    {"strLenBytes", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::strLenBytes, false}},
    {"toLower", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::toLower, false}},
    {"toUpper", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::toUpper, false}},
    {"trim", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::trim, false}},
    {"ltrim", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::ltrim, false}},
    {"rtrim", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::rtrim, false}},
    {"coerceToBool", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::coerceToBool, false}},
    {"coerceToString",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::coerceToString, false}},
    {"acos", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::acos, false}},
    {"acosh", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::acosh, false}},
    {"asin", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::asin, false}},
    {"asinh", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::asinh, false}},
    {"atan", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::atan, false}},
    {"atanh", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::atanh, false}},
    {"atan2", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::atan2, false}},
    {"cos", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::cos, false}},
    {"cosh", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::cosh, false}},
    {"degreesToRadians",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::degreesToRadians, false}},
    {"radiansToDegrees",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::radiansToDegrees, false}},
    {"sin", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::sin, false}},
    {"sinh", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::sinh, false}},
    {"tan", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::tan, false}},
    {"tanh", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::tanh, false}},
    {"round", BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::round, false}},
    {"concat", BuiltinFn{kAnyNumberOfArgs, vm::Builtin::concat, false}},
    {"concatArrays", BuiltinFn{kAnyNumberOfArgs, vm::Builtin::concatArrays, false}},
    {"aggConcatArraysCapped",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggConcatArraysCapped, true}},
    {"isMember", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::isMember, false}},
    {"indexOfBytes",
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::indexOfBytes, false}},
    {"indexOfCP",
     BuiltinFn{[](size_t n) { return n == 3 || n == 4; }, vm::Builtin::indexOfCP, false}},
    {"isDayOfWeek", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::isDayOfWeek, false}},
    {"isTimeUnit", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::isTimeUnit, false}},
    {"isTimezone", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::isTimezone, false}},
    {"isValidToStringFormat",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::isValidToStringFormat, false}},
    {"validateFromStringFormat",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::validateFromStringFormat, false}},
    {"setUnion", BuiltinFn{kAnyNumberOfArgs, vm::Builtin::setUnion, false}},
    {"setIntersection", BuiltinFn{kAnyNumberOfArgs, vm::Builtin::setIntersection, false}},
    {"setDifference",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::setDifference, false}},
    {"setEquals", BuiltinFn{[](size_t n) { return n >= 2; }, vm::Builtin::setEquals, false}},
    {"setIsSubset", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::setIsSubset, false}},
    {"collSetUnion", BuiltinFn{[](size_t n) { return n >= 1; }, vm::Builtin::collSetUnion, false}},
    {"collSetIntersection",
     BuiltinFn{[](size_t n) { return n >= 1; }, vm::Builtin::collSetIntersection, false}},
    {"collSetDifference",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::collSetDifference, false}},
    {"collSetEquals",
     BuiltinFn{[](size_t n) { return n >= 3; }, vm::Builtin::collSetEquals, false}},
    {"collSetIsSubset",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::collSetIsSubset, false}},
    {"aggSetUnion", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggSetUnion, true}},
    {"aggSetUnionCapped",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggSetUnionCapped, true}},
    {"aggCollSetUnion",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggCollSetUnion, true}},
    {"aggCollSetUnionCapped",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::aggCollSetUnionCapped, true}},
    {"runJsPredicate",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::runJsPredicate, false}},
    {"regexCompile", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::regexCompile, false}},
    {"regexFind", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::regexFind, false}},
    {"regexFindAll", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::regexFindAll, false}},
    {"getRegexPattern",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::getRegexPattern, false}},
    {"getRegexFlags",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::getRegexFlags, false}},
    {"shardFilter", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::shardFilter, false}},
    {"shardHash", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::shardHash, false}},
    {"extractSubArray",
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::extractSubArray, false}},
    {"isArrayEmpty", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::isArrayEmpty, false}},
    {"reverseArray", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::reverseArray, false}},
    {"sortArray",
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::sortArray, false}},
    {"dateAdd", BuiltinFn{[](size_t n) { return n == 5; }, vm::Builtin::dateAdd, false}},
    {"hasNullBytes", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::hasNullBytes, false}},
    {"hash", BuiltinFn{kAnyNumberOfArgs, vm::Builtin::hash, false}},
    {"ftsMatch", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::ftsMatch, false}},
    {"generateSortKey",
     BuiltinFn{[](size_t n) { return n == 2 || n == 3; }, vm::Builtin::generateSortKey, false}},
    {"generateCheapSortKey",
     BuiltinFn{
         [](size_t n) { return n == 2 || n == 3; }, vm::Builtin::generateCheapSortKey, false}},
    {"sortKeyComponentVectorGetElement",
     BuiltinFn{
         [](size_t n) { return n == 2; }, vm::Builtin::sortKeyComponentVectorGetElement, false}},
    {"sortKeyComponentVectorToArray",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::sortKeyComponentVectorToArray, false}},
    {"tsSecond", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::tsSecond, false}},
    {"tsIncrement", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::tsIncrement, false}},
    {"typeMatch", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::typeMatch, false}},
    {"dateTrunc", BuiltinFn{[](size_t n) { return n == 6; }, vm::Builtin::dateTrunc, false}},
    {"_internalLeast",
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::internalLeast, false}},
    {"_internalGreatest",
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::internalGreatest, false}},
    {"objectToArray",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::objectToArray, false}},
    {"arrayToObject",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::arrayToObject, false}},
    {"array", BuiltinFn{kAnyNumberOfArgs, vm::Builtin::newArray, false}},
    {"aggFirstNNeedsMoreInput",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggFirstNNeedsMoreInput, false}},
    {"aggFirstN", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggFirstN, false}},
    {"aggFirstNMerge",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggFirstNMerge, true}},
    {"aggFirstNFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggFirstNFinalize, false}},
    {"aggLastN", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggLastN, true}},
    {"aggLastNMerge", BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggLastNMerge, true}},
    {"aggLastNFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggLastNFinalize, false}},
    {"aggTopN", BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::aggTopN, true}},
    {"aggTopNMerge", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggTopNMerge, true}},
    {"aggTopNFinalize",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggTopNFinalize, false}},
    {"aggBottomN", BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::aggBottomN, true}},
    {"aggBottomNMerge",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggBottomNMerge, true}},
    {"aggBottomNFinalize",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggBottomNFinalize, false}},
    {"aggMaxN", BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::aggMaxN, true}},
    {"aggMaxNMerge",
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::aggMaxNMerge, true}},
    {"aggMaxNFinalize",
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::aggMaxNFinalize, false}},
    {"aggMinN", BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::aggMinN, true}},
    {"aggMinNMerge",
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::aggMinNMerge, true}},
    {"aggMinNFinalize",
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::aggMinNFinalize, false}},
    {"aggRank", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRank, true}},
    {"aggRankColl", BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::aggRankColl, true}},
    {"aggDenseRank", BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggDenseRank, true}},
    {"aggDenseRankColl",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::aggDenseRankColl, true}},
    {"aggRankFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRankFinalize, false}},
    {"aggExpMovingAvg",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggExpMovingAvg, true}},
    {"aggExpMovingAvgFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggExpMovingAvgFinalize, false}},
    {"aggRemovableSumAdd",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableSumAdd, true}},
    {"aggRemovableSumRemove",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableSumRemove, true}},
    {"aggRemovableSumFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableSumFinalize, false}},
    {"aggIntegralInit",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggIntegralInit, false}},
    {"aggIntegralAdd",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggIntegralAdd, true}},
    {"aggIntegralRemove",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggIntegralRemove, true}},
    {"aggIntegralFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggIntegralFinalize, false}},
    {"aggDerivativeFinalize",
     BuiltinFn{[](size_t n) { return n == 5; }, vm::Builtin::aggDerivativeFinalize, false}},
    {"aggCovarianceAdd",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggCovarianceAdd, true}},
    {"aggCovarianceRemove",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggCovarianceRemove, true}},
    {"aggCovarianceSampFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggCovarianceSampFinalize, false}},
    {"aggCovariancePopFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggCovariancePopFinalize, false}},
    {"aggRemovablePushAdd",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovablePushAdd, true}},
    {"aggRemovablePushRemove",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovablePushRemove, true}},
    {"aggRemovablePushFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovablePushFinalize, false}},
    {"aggRemovableStdDevAdd",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableStdDevAdd, true}},
    {"aggRemovableStdDevRemove",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableStdDevRemove, true}},
    {"aggRemovableStdDevSampFinalize",
     BuiltinFn{
         [](size_t n) { return n == 1; }, vm::Builtin::aggRemovableStdDevSampFinalize, false}},
    {"aggRemovableStdDevPopFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableStdDevPopFinalize, false}},
    {"aggRemovableAvgFinalize",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableAvgFinalize, false}},
    {"aggLinearFillCanAdd",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggLinearFillCanAdd, false}},
    {"aggLinearFillAdd",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggLinearFillAdd, true}},
    {"aggLinearFillFinalize",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggLinearFillFinalize, false}},
    {"aggRemovableFirstNInit",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableFirstNInit, false}},
    {"aggRemovableFirstNAdd",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableFirstNAdd, true}},
    {"aggRemovableFirstNRemove",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableFirstNRemove, true}},
    {"aggRemovableFirstNFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableFirstNFinalize, false}},
    {"aggRemovableLastNInit",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableLastNInit, false}},
    {"aggRemovableLastNAdd",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableLastNAdd, true}},
    {"aggRemovableLastNRemove",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableLastNRemove, true}},
    {"aggRemovableLastNFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableLastNFinalize, false}},
    {"aggRemovableAddToSetInit",
     BuiltinFn{[](size_t n) { return n == 0; }, vm::Builtin::aggRemovableAddToSetInit, false}},
    {"aggRemovableAddToSetCollInit",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableAddToSetCollInit, false}},
    {"aggRemovableAddToSetAdd",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableAddToSetAdd, true}},
    {"aggRemovableAddToSetRemove",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableAddToSetRemove, true}},
    {"aggRemovableAddToSetFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableAddToSetFinalize, false}},
    {"aggRemovableMinMaxNCollInit",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::aggRemovableMinMaxNCollInit, false}},
    {"aggRemovableMinMaxNInit",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableMinMaxNInit, false}},
    {"aggRemovableMinMaxNAdd",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableMinMaxNAdd, true}},
    {"aggRemovableMinMaxNRemove",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableMinMaxNRemove, true}},
    {"aggRemovableMinNFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableMinNFinalize, false}},
    {"aggRemovableMaxNFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableMaxNFinalize, false}},
    {"aggRemovableTopNInit",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableTopNInit, false}},
    {"aggRemovableTopNAdd",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableTopNAdd, true}},
    {"aggRemovableTopNRemove",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableTopNRemove, true}},
    {"aggRemovableTopNFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableTopNFinalize, false}},
    {"aggRemovableBottomNInit",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableBottomNInit, false}},
    {"aggRemovableBottomNAdd",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableBottomNAdd, true}},
    {"aggRemovableBottomNRemove",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::aggRemovableBottomNRemove, true}},
    {"aggRemovableBottomNFinalize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::aggRemovableBottomNFinalize, false}},
    {"valueBlockExists",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::valueBlockExists, false}},
    {"valueBlockTypeMatch",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockTypeMatch, false}},
    {"valueBlockIsTimezone",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockIsTimezone, false}},
    {"valueBlockFillEmpty",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockFillEmpty, false}},
    {"valueBlockFillEmptyBlock",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockFillEmptyBlock, false}},
    {"valueBlockAggMin",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockAggMin, false}},
    {"valueBlockAggMax",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockAggMax, false}},
    {"valueBlockAggCount",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::valueBlockAggCount, false}},
    {"valueBlockDateDiff",
     BuiltinFn{[](size_t n) { return n == 6 || n == 7; }, vm::Builtin::valueBlockDateDiff, false}},
    {"valueBlockDateTrunc",
     BuiltinFn{[](size_t n) { return n == 7; }, vm::Builtin::valueBlockDateTrunc, false}},
    {"valueBlockDateAdd",
     BuiltinFn{[](size_t n) { return n == 6; }, vm::Builtin::valueBlockDateAdd, false}},
    {"valueBlockAggSum",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockAggSum, false}},
    {"valueBlockAdd",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::valueBlockAdd, false}},
    {"valueBlockSub",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::valueBlockSub, false}},
    {"valueBlockMult",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::valueBlockMult, false}},
    {"valueBlockDiv",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::valueBlockDiv, false}},
    {"valueBlockGtScalar",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockGtScalar, false}},
    {"valueBlockGteScalar",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockGteScalar, false}},
    {"valueBlockEqScalar",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockEqScalar, false}},
    {"valueBlockNeqScalar",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockNeqScalar, false}},
    {"valueBlockLtScalar",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockLtScalar, false}},
    {"valueBlockLteScalar",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockLteScalar, false}},
    {"valueBlockCmp3wScalar",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockCmp3wScalar, false}},
    {"valueBlockCombine",
     BuiltinFn{[](size_t n) { return n == 3; }, vm::Builtin::valueBlockCombine, false}},
    {"valueBlockLogicalAnd",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockLogicalAnd, false}},
    {"valueBlockLogicalOr",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockLogicalOr, false}},
    {"valueBlockLogicalNot",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::valueBlockLogicalNot, false}},
    {"valueBlockNewFill",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockNewFill, false}},
    {"valueBlockSize",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::valueBlockSize, false}},
    {"valueBlockNone",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockNone, false}},
    {"valueBlockIsMember",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockIsMember, false}},
    {"valueBlockCoerceToBool",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::valueBlockCoerceToBool, false}},
    {"valueBlockTrunc",
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::valueBlockTrunc, false}},
    {"valueBlockRound",
     BuiltinFn{[](size_t n) { return n == 1 || n == 2; }, vm::Builtin::valueBlockRound, false}},
    {"valueBlockMod",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockMod, false}},
    {"valueBlockConvert",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::valueBlockConvert, false}},
    {"cellFoldValues_F",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::cellFoldValues_F, false}},
    {"cellFoldValues_P",
     BuiltinFn{[](size_t n) { return n == 2; }, vm::Builtin::cellFoldValues_P, false}},
    {"cellBlockGetFlatValuesBlock",
     BuiltinFn{[](size_t n) { return n == 1; }, vm::Builtin::cellBlockGetFlatValuesBlock, false}},
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

    invariant(nodes.size() == arity);

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
                                 value::bitcastTo<bool>(val) ? vm::Instruction::True
                                                             : vm::Instruction::False);
            return code;
        });
    }

    return generatorLegacy<&vm::CodeFragment::appendTraverseF>(ctx, nodes, false);
}

vm::CodeFragment generateTraverseCellValues(CompileCtx& ctx,
                                            const EExpression::Vector& nodes,
                                            bool) {
    if (nodes[1]->as<ELocalLambda>()) {
        return withNewLabel(ctx, [&](vm::LabelId afterBodyLabel) {
            vm::CodeFragment code;
            auto lambda = nodes[1]->as<ELocalLambda>();

            // Jump around the body.
            code.appendLabelJump(afterBodyLabel);

            // Remember the position and append the body.
            auto bodyPosition = code.instrs().size();
            code.appendNoStack(lambda->compileBodyDirect(ctx));

            // Append the traverseCellValues call.
            code.appendLabel(afterBodyLabel);
            code.append(nodes[0]->compileDirect(ctx));
            code.appendTraverseCellValues(bodyPosition);
            return code;
        });
    }

    return generatorLegacy<&vm::CodeFragment::appendTraverseCellValues>(ctx, nodes, false);
}

vm::CodeFragment generateTraverseCellTypes(CompileCtx& ctx,
                                           const EExpression::Vector& nodes,
                                           bool) {
    if (nodes[1]->as<ELocalLambda>()) {
        return withNewLabel(ctx, [&](vm::LabelId afterBodyLabel) {
            vm::CodeFragment code;
            auto lambda = nodes[1]->as<ELocalLambda>();

            // Jump around the body.
            code.appendLabelJump(afterBodyLabel);

            // Remember the position and append the body.
            auto bodyPosition = code.instrs().size();
            code.appendNoStack(lambda->compileBodyDirect(ctx));

            // Append the traverseCellTypes call.
            code.appendLabel(afterBodyLabel);
            code.append(nodes[0]->compileDirect(ctx));
            code.appendTraverseCellTypes(bodyPosition);

            return code;
        });
    }

    return generatorLegacy<&vm::CodeFragment::appendTraverseCellTypes>(ctx, nodes, false);
}

/**
 * The map of functions that resolve directly to instructions.
 */
static stdx::unordered_map<std::string, InstrFn> kInstrFunctions = {
    {"getElement", InstrFn{2, generator<2, &vm::CodeFragment::appendGetElement>, false}},
    {"getField", InstrFn{2, generateGetField, false}},
    {"getArraySize", InstrFn{1, generator<1, &vm::CodeFragment::appendGetArraySize>, false}},
    {"collComparisonKey",
     InstrFn{2, generator<2, &vm::CodeFragment::appendCollComparisonKey>, false}},
    {"getFieldOrElement",
     InstrFn{2, generator<2, &vm::CodeFragment::appendGetFieldOrElement>, false}},
    {"traverseP", InstrFn{3, generateTraverseP, false}},
    {"traverseF", InstrFn{3, generateTraverseF, false}},
    {"magicTraverseF", InstrFn{5, generatorLegacy<&vm::CodeFragment::appendMagicTraverseF>, false}},
    {"traverseCsiCellValues", InstrFn{2, generateTraverseCellValues, false}},
    {"traverseCsiCellTypes", InstrFn{2, generateTraverseCellTypes, false}},
    {"setField", InstrFn{3, generatorLegacy<&vm::CodeFragment::appendSetField>, false}},
    {"sum", InstrFn{1, generatorLegacy<&vm::CodeFragment::appendSum>, true}},
    {"min", InstrFn{1, generatorLegacy<&vm::CodeFragment::appendMin>, true}},
    {"max", InstrFn{1, generatorLegacy<&vm::CodeFragment::appendMax>, true}},
    {"first", InstrFn{1, generatorLegacy<&vm::CodeFragment::appendFirst>, true}},
    {"last", InstrFn{1, generatorLegacy<&vm::CodeFragment::appendLast>, true}},
    {"collMin", InstrFn{2, generatorLegacy<&vm::CodeFragment::appendCollMin>, true}},
    {"collMax", InstrFn{2, generatorLegacy<&vm::CodeFragment::appendCollMax>, true}},
    {"exists", InstrFn{1, generator<1, &vm::CodeFragment::appendExists>, false}},
    {"mod", InstrFn{2, generator<2, &vm::CodeFragment::appendMod>, false}},
    {"isDate", InstrFn{1, generator<1, &vm::CodeFragment::appendIsDate>, false}},
    {"isNumber", InstrFn{1, generator<1, &vm::CodeFragment::appendIsNumber>, false}},
    {"isNull", InstrFn{1, generator<1, &vm::CodeFragment::appendIsNull>, false}},
    {"isObject", InstrFn{1, generator<1, &vm::CodeFragment::appendIsObject>, false}},
    {"isArray", InstrFn{1, generator<1, &vm::CodeFragment::appendIsArray>, false}},
    {"isInListData", InstrFn{1, generator<1, &vm::CodeFragment::appendIsInListData>, false}},
    {"isString", InstrFn{1, generator<1, &vm::CodeFragment::appendIsString>, false}},
    {"isBinData", InstrFn{1, generator<1, &vm::CodeFragment::appendIsBinData>, false}},
    {"isNaN", InstrFn{1, generator<1, &vm::CodeFragment::appendIsNaN>, false}},
    {"isInfinity", InstrFn{1, generator<1, &vm::CodeFragment::appendIsInfinity>, false}},
    {"isRecordId", InstrFn{1, generator<1, &vm::CodeFragment::appendIsRecordId>, false}},
    {"isMinKey", InstrFn{1, generator<1, &vm::CodeFragment::appendIsMinKey>, false}},
    {"isMaxKey", InstrFn{1, generator<1, &vm::CodeFragment::appendIsMaxKey>, false}},
    {"isTimestamp", InstrFn{1, generator<1, &vm::CodeFragment::appendIsTimestamp>, false}},
    {"valueBlockApplyLambda",
     InstrFn{3, generatorLegacy<&vm::CodeFragment::appendValueBlockApplyLambda>, false}},
};
}  // namespace

vm::CodeFragment EFunction::compileDirect(CompileCtx& ctx) const {
    if (auto it = kBuiltinFunctions.find(_name); it != kBuiltinFunctions.end()) {
        auto arity = _nodes.size();
        if (!it->second.arityTest(arity)) {
            uasserted(4822843,
                      str::stream() << "function call: " << _name << " has wrong arity: " << arity);
        }
        vm::CodeFragment code;

        // Optimize well known set of functions with constant arguments and generate their
        // specialized variants.
        if (_name == "typeMatch" && _nodes[1]->as<EConstant>()) {
            auto [tag, val] = _nodes[1]->as<EConstant>()->getConstant();
            if (tag == value::TypeTags::NumberInt32) {
                auto mask = static_cast<uint32_t>(value::bitcastTo<int32_t>(val));
                auto param = appendParameter(code, ctx, _nodes[0].get());
                code.appendTypeMatch(param, mask);

                return code;
            }
        } else if (_name == "dateTrunc" && _nodes[2]->as<EConstant>() &&
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
                    str::stream() << "aggregate function call: " << _name
                                  << " occurs in the non-aggregate context.",
                    ctx.aggExpression && ctx.accumulator);

            code.appendMoveVal(ctx.accumulator);
            ++arity;
        }

        code.appendFunction(it->second.builtin, arity);

        return code;
    }

    if (auto it = kInstrFunctions.find(_name); it != kInstrFunctions.end()) {
        if (it->second.arity != _nodes.size()) {
            uasserted(4822845,
                      str::stream()
                          << "function call: " << _name << " has wrong arity: " << _nodes.size());
        }
        if (it->second.aggregate) {
            uassert(4822846,
                    str::stream() << "aggregate function call: " << _name
                                  << " occurs in the non-aggregate context.",
                    ctx.aggExpression && ctx.accumulator);
        }

        return (it->second.generate)(ctx, _nodes, it->second.aggregate);
    }

    if (_name == "aggState") {
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

    uasserted(4822847, str::stream() << "unknown function call: " << _name);
}

std::vector<DebugPrinter::Block> EFunction::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    DebugPrinter::addKeyword(ret, _name);

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
    return sizeof(*this) + size_estimator::estimate(_name) + size_estimator::estimate(_nodes);
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
     * @else:     elseBranch
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
        code.append(std::move(elseCodeBranch), std::move(thenCodeBranch));
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
    return std::make_unique<ELocalLambda>(_frameId, _nodes.back()->clone());
}

vm::CodeFragment ELocalLambda::compileBodyDirect(CompileCtx& ctx) const {
    // Compile the body first so we know its size.
    auto inner = _nodes.back()->compileDirect(ctx);
    vm::CodeFragment body;

    // Declare the frame containing lambda variable.
    // The variable is expected to be already on the stack so declare the frame just below the
    // current top of the stack.
    body.declareFrame(_frameId, -1);

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
        code.appendLocalLambda(bodyPosition);

        return code;
    });
}

std::vector<DebugPrinter::Block> ELocalLambda::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;

    DebugPrinter::addKeyword(ret, "lambda");
    ret.emplace_back("`(`");
    DebugPrinter::addIdentifier(ret, _frameId, 0);
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
            MONGO_UNREACHABLE;
    }

    ret.emplace_back("`)");
    return ret;
}

size_t ENumericConvert::estimateSize() const {
    return sizeof(*this) + size_estimator::estimate(_nodes);
}
}  // namespace sbe
}  // namespace mongo
