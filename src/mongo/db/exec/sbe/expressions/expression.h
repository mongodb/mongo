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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/slots_provider.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {

struct CompileCtx;

/**
 * This is an abstract base class of all expression types in SBE. The expression types derived form
 * this base must implement two fundamental operations:
 *   - compile method that generates bytecode that is executed by the VM during runtime
 *   - clone method that creates a complete copy of the expression
 *
 * The debugPrint method generates textual representation of the expression for internal debugging
 * purposes.
 */
class EExpression {
public:
    /**
     * Let's optimistically assume a nice binary tree.
     */
    using Vector = absl::InlinedVector<std::unique_ptr<EExpression>, 2>;

    virtual ~EExpression() = default;

    /**
     * The idiomatic C++ pattern of object cloning. Expressions must be fully copyable as every
     * thread in parallel execution needs its own private copy.
     */
    virtual std::unique_ptr<EExpression> clone() const = 0;

    /**
     * Returns bytecode directly executable by VM.
     */
    std::unique_ptr<vm::CodeFragment> compile(CompileCtx& ctx) const;

    virtual vm::CodeFragment compileDirect(CompileCtx& ctx) const = 0;

    virtual std::vector<DebugPrinter::Block> debugPrint() const = 0;

    /**
     * Estimates the size of the current expression node and its children.
     */
    virtual size_t estimateSize() const = 0;

    /**
     * Utility for casting to derived types.
     */
    template <typename T>
    T* as() {
        return dynamic_cast<T*>(this);
    }

    /**
     * Utility for casting to derived types.
     */
    template <typename T>
    const T* as() const {
        return dynamic_cast<const T*>(this);
    }

    // For printing from an interactive debugger.
    std::string toString() const;

protected:
    Vector _nodes;

    /**
     * Expressions can never be constructed with nullptr children.
     */
    void validateNodes() {
        for (auto& node : _nodes) {
            invariant(node);
        }
    }

    template <typename P>
    void collectDescendants(P&& expandPredicate, std::vector<const EExpression*>* acc) const {
        if (expandPredicate(this)) {
            for (auto& child : _nodes) {
                child->collectDescendants(expandPredicate, acc);
            }
        } else {
            acc->push_back(this);
        }
    }
};

using SlotExprPair = std::pair<value::SlotId, std::unique_ptr<EExpression>>;
using SlotExprPairVector = std::vector<SlotExprPair>;

struct AggExprPair {
    std::unique_ptr<EExpression> init;
    std::unique_ptr<EExpression> agg;
};

struct BlockAggExprTuple {
    std::unique_ptr<EExpression> init;
    std::unique_ptr<EExpression> blockAgg;
    std::unique_ptr<EExpression> agg;
};

using AggExprVector = std::vector<std::pair<value::SlotId, AggExprPair>>;
using BlockAggExprTupleVector = std::vector<std::pair<value::SlotId, BlockAggExprTuple>>;

template <typename T, typename... Args>
inline std::unique_ptr<EExpression> makeE(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename... Ts>
inline auto makeEs(Ts&&... pack) {
    EExpression::Vector exprs;

    (exprs.emplace_back(std::forward<Ts>(pack)), ...);

    return exprs;
}

template <typename... Ts>
inline auto makeVEs(Ts&&... pack) {
    std::vector<std::unique_ptr<EExpression>> exprs;

    (exprs.emplace_back(std::forward<Ts>(pack)), ...);

    return exprs;
}

namespace detail {
// base case
template <typename R>
inline void makeSlotExprPairHelper(R& result,
                                   value::SlotId slot,
                                   std::unique_ptr<EExpression> expr) {
    result.emplace_back(slot, std::move(expr));
}

// recursive case
template <typename R, typename... Ts>
inline void makeSlotExprPairHelper(R& result,
                                   value::SlotId slot,
                                   std::unique_ptr<EExpression> expr,
                                   Ts&&... rest) {
    result.emplace_back(slot, std::move(expr));
    makeSlotExprPairHelper(result, std::forward<Ts>(rest)...);
}

// base case
template <typename R>
inline void makeAggExprPairHelper(R& result,
                                  value::SlotId slot,
                                  std::unique_ptr<EExpression> initExpr,
                                  std::unique_ptr<EExpression> accExpr) {
    if constexpr (std::is_same_v<R, value::SlotMap<AggExprPair>>) {
        result.emplace(slot, AggExprPair{std::move(initExpr), std::move(accExpr)});
    } else {
        static_assert(std::is_same_v<R, AggExprVector>);
        result.push_back(
            std::make_pair(slot, AggExprPair{std::move(initExpr), std::move(accExpr)}));
    }
}

// recursive case
template <typename R, typename... Ts>
inline void makeAggExprPairHelper(R& result,
                                  value::SlotId slot,
                                  std::unique_ptr<EExpression> initExpr,
                                  std::unique_ptr<EExpression> accExpr,
                                  Ts&&... rest) {
    if constexpr (std::is_same_v<R, value::SlotMap<AggExprPair>>) {
        result.emplace(slot, AggExprPair{std::move(initExpr), std::move(accExpr)});
    } else {
        static_assert(std::is_same_v<R, AggExprVector>);
        result.push_back(
            std::make_pair(slot, AggExprPair{std::move(initExpr), std::move(accExpr)}));
    }
    makeAggExprPairHelper(result, std::forward<Ts>(rest)...);
}
}  // namespace detail

template <typename... Args>
auto makeSV(Args&&... args) {
    value::SlotVector v;
    v.reserve(sizeof...(Args));
    (v.push_back(std::forward<Args>(args)), ...);
    return v;
}

template <typename... Ts>
auto makeSlotExprPairVec(Ts&&... pack) {
    SlotExprPairVector v;
    if constexpr (sizeof...(pack) > 0) {
        v.reserve(sizeof...(Ts) / 2);
        detail::makeSlotExprPairHelper(v, std::forward<Ts>(pack)...);
    }
    return v;
}

/**
 * Used only by unit tests. Expects input arguments in threes: (slotId, initExpr, accExpr).
 */
template <typename... Ts>
auto makeAggExprVector(Ts&&... pack) {
    AggExprVector v;
    if constexpr (sizeof...(pack) > 0) {
        v.reserve(sizeof...(Ts) / 3);
        detail::makeAggExprPairHelper(v, std::forward<Ts>(pack)...);
    }
    return v;
}

/**
 * This is a constant expression. It assumes the ownership of the input constant.
 */
class EConstant final : public EExpression {
public:
    EConstant(value::TypeTags tag, value::Value val) : _tag(tag), _val(val) {}
    EConstant(StringData str) {
        // Views are non-owning so we have to make a copy.
        std::tie(_tag, _val) = value::makeNewString(str);
    }

    ~EConstant() override {
        value::releaseValue(_tag, _val);
    }

    std::unique_ptr<EExpression> clone() const override;

    vm::CodeFragment compileDirect(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;
    size_t estimateSize() const final;
    std::pair<value::TypeTags, value::Value> getConstant() const {
        return {_tag, _val};
    }

private:
    value::TypeTags _tag;
    value::Value _val;
};

/**
 * This is an expression representing a variable. The variable can point to a slot as defined by a
 * SBE plan stages or to a slot defined by a local bind (a.k.a. let) expression. The local binds are
 * identified by the frame id.
 */
class EVariable final : public EExpression {
public:
    EVariable(value::SlotId var) : _var(var), _frameId(boost::none), _moveFrom(false) {}
    EVariable(FrameId frameId, value::SlotId var, bool moveFrom = false)
        : _var(var), _frameId(frameId), _moveFrom(moveFrom) {}

    std::unique_ptr<EExpression> clone() const override;

    vm::CodeFragment compileDirect(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;
    size_t estimateSize() const final {
        return sizeof(*this);
    }
    boost::optional<FrameId> getFrameId() const {
        return _frameId;
    }
    value::SlotId getSlotId() const {
        return _var;
    }

    bool isMoveFrom() const {
        return _moveFrom;
    }

private:
    value::SlotId _var;
    boost::optional<FrameId> _frameId;
    // If true then accessing this variable will take over the ownership of value. The flag has to
    // be used carefully only when the variable is used exactly once. When used with slots then the
    // expression must be guaranteed to be the last use of the slot. Essentially we are simulating
    // linear type system here.
    bool _moveFrom{false};
};

/**
 * This is a n-ary primitive (builtin) operation.
 */
class EPrimNary final : public EExpression {
public:
    enum Op {
        // Logical operations. These operations are short-circuiting.
        logicAnd,
        logicOr,

        // Math operations.
        add,
        mul,
    };

    EPrimNary(Op op, std::vector<std::unique_ptr<EExpression>> args) : _op(op) {
        tassert(10217100, "Expected at least two operands", args.size() >= 2);
        _nodes.reserve(args.size());
        for (auto&& arg : args) {
            _nodes.emplace_back(std::move(arg));
        }
        validateNodes();
    }

    std::unique_ptr<EExpression> clone() const override;

    vm::CodeFragment compileDirect(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

    size_t estimateSize() const final;

private:
    Op _op;
};

/**
 * This is a binary primitive (builtin) operation.
 */
class EPrimBinary final : public EExpression {
public:
    enum Op {
        // Logical operations. These operations are short-circuiting.
        logicAnd,  // TODO: remove with SERVER-100579
        logicOr,   // TODO: remove with SERVER-100579

        // Nothing-handling operation. This is short-circuiting like logicOr,
        // but it checks Nothing / non-Nothing instead of false / true.
        fillEmpty,

        // Math operations.
        add,  // TODO: remove with SERVER-100579
        sub,
        mul,  // TODO: remove with SERVER-100579
        div,

        // Comparison operations. These operations support taking a third "collator" arg.
        // If you add or remove comparison operations or change their order, make sure you
        // update isComparisonOp() accordingly.
        less,
        lessEq,
        greater,
        greaterEq,
        eq,
        neq,
        cmp3w,
    };

    EPrimBinary(Op op,
                std::unique_ptr<EExpression> lhs,
                std::unique_ptr<EExpression> rhs,
                std::unique_ptr<EExpression> collator = nullptr)
        : _op(op) {
        _nodes.emplace_back(std::move(lhs));
        _nodes.emplace_back(std::move(rhs));

        if (collator) {
            invariant(isComparisonOp(_op));
            _nodes.emplace_back(std::move(collator));
        }

        validateNodes();
    }

    static bool isComparisonOp(Op op) {
        return (op >= less && op <= cmp3w);
    }

    std::unique_ptr<EExpression> clone() const override;

    vm::CodeFragment compileDirect(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

    size_t estimateSize() const final;

private:
    std::vector<const EExpression*> collectOrClauses() const;
    std::vector<const EExpression*> collectAndClauses() const;

private:
    Op _op;
};

/**
 * This is a unary primitive (builtin) operation.
 */
class EPrimUnary final : public EExpression {
public:
    enum Op {
        logicNot,
        negate,
    };

    EPrimUnary(Op op, std::unique_ptr<EExpression> operand) : _op(op) {
        _nodes.emplace_back(std::move(operand));
        validateNodes();
    }

    std::unique_ptr<EExpression> clone() const override;

    vm::CodeFragment compileDirect(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

    size_t estimateSize() const final;

private:
    Op _op;
};

/**
 * This is a function call expression. Functions can have arbitrary arity and arguments are
 * evaluated right to left. They are identified simply by a name and we have a dictionary of all
 * supported (builtin) functions.
 */
class EFunction final : public EExpression {
public:
    EFunction(StringData name, EExpression::Vector args) : _name(name) {
        _nodes = std::move(args);
        validateNodes();
    }

    std::unique_ptr<EExpression> clone() const override;

    vm::CodeFragment compileDirect(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

    size_t estimateSize() const final;

private:
    std::string _name;
};

/**
 * This is a conditional (a.k.a. ite) expression.
 */
class EIf final : public EExpression {
public:
    EIf(std::unique_ptr<EExpression> cond,
        std::unique_ptr<EExpression> thenBranch,
        std::unique_ptr<EExpression> elseBranch) {
        _nodes.emplace_back(std::move(cond));
        _nodes.emplace_back(std::move(thenBranch));
        _nodes.emplace_back(std::move(elseBranch));
        validateNodes();
    }

    std::unique_ptr<EExpression> clone() const override;

    vm::CodeFragment compileDirect(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

    size_t estimateSize() const final;
};

/**
 * This is a multi-conditional (a.k.a. if-then-elif-...-else) expression.
 */
class ESwitch final : public EExpression {
public:
    /**
     * Create a Switch multi-conditional expression by providing a flat list of expressions. These
     * are taken as pairs of (condition, thenBranch) followed by a final 'default' expression.
     */
    ESwitch(std::vector<std::unique_ptr<sbe::EExpression>> nodes) {
        // Enforce that the list is not empty and contains an odd number of expressions.
        // When it contains a single expression, it represents a switch where only the 'default'
        // branch is present.
        tassert(10130700,
                "switch created with a wrong number of expressions",
                nodes.size() > 1 && ((nodes.size() - 1) % 2) == 0);
        _nodes.reserve(nodes.size());
        for (auto&& n : nodes) {
            _nodes.emplace_back(std::move(n));
        }
        validateNodes();
    }

    std::unique_ptr<EExpression> clone() const override;

    vm::CodeFragment compileDirect(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

    size_t estimateSize() const final;

    /**
     * Computes the number of pairs (condition, thenBranch) contained in the flat list of
     * expressions. i.e. exclude the final 'default' expression, then divide by two.
     */
    size_t getNumBranches() const {
        return (_nodes.size() - 1) / 2;
    }

    /**
     * Extract from the flat list of expressions the expression representing the idx-th condition.
     */
    const EExpression* getCondition(size_t idx) const {
        return _nodes[idx * 2].get();
    }

    /**
     * Extract from the flat list of expressions the expression representing the idx-th branch, i.e.
     * right after the idx-th condition.
     */
    const EExpression* getThenBranch(size_t idx) const {
        return _nodes[idx * 2 + 1].get();
    }

    /**
     * Extract from the flat list of expressions the expression representing the default branch,
     * i.e. the last item of the list.
     */
    const EExpression* getDefault() const {
        return _nodes.back().get();
    }
};

/**
 * This is a let expression that can be used to define local variables.
 */
class ELocalBind final : public EExpression {
public:
    ELocalBind(FrameId frameId, EExpression::Vector binds, std::unique_ptr<EExpression> in)
        : _frameId(frameId) {
        _nodes = std::move(binds);
        _nodes.emplace_back(std::move(in));
        validateNodes();
    }

    std::unique_ptr<EExpression> clone() const override;

    vm::CodeFragment compileDirect(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

    size_t estimateSize() const final;

private:
    FrameId _frameId;
};

/**
 * A simple lambda value with no captures.
 */
class ELocalLambda final : public EExpression {
public:
    ELocalLambda(FrameId frameId, std::unique_ptr<EExpression> body) : _frameId(frameId) {
        _nodes.emplace_back(std::move(body));
        validateNodes();
    }

    std::unique_ptr<EExpression> clone() const override;

    vm::CodeFragment compileDirect(CompileCtx& ctx) const override;
    vm::CodeFragment compileBodyDirect(CompileCtx& ctx) const;
    std::vector<DebugPrinter::Block> debugPrint() const override;

    size_t estimateSize() const final;

private:
    FrameId _frameId;
};

/**
 * Evaluating this expression will throw an exception with given error code and message.
 */
class EFail final : public EExpression {
public:
    EFail(ErrorCodes::Error code, StringData message) : _code(code) {
        std::tie(_messageTag, _messageVal) = value::makeNewString(message);
    }

    ~EFail() override {
        value::releaseValue(_messageTag, _messageVal);
    }

    std::unique_ptr<EExpression> clone() const override;

    vm::CodeFragment compileDirect(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

    size_t estimateSize() const final;

private:
    ErrorCodes::Error _code;
    value::TypeTags _messageTag;
    value::Value _messageVal;
};

/**
 * This is a numeric conversion expression. It supports both narrowing and widening conversion under
 * no loss of precision. If a given conversion loses precision the expression results in Nothing.
 * ENumericConvert can be instantiated for the following source to target tags,
 *
 *  NumberInt32 -> NumberInt64, NumberInt32 -> NumberDouble, NumberInt32 -> NumberDecimal
 *  NumberInt64 -> NumberInt32, NumberInt64 -> NumberDouble, NumberInt64 -> NumberDecimal
 *  NumberDouble -> NumberInt32, NumberDouble -> NumberInt64, NumberDouble -> NumberDecimal
 *  NumberDecimal -> NumberInt32, NumberDecimal -> NumberInt64, NumberDecimal -> NumberDouble
 */
class ENumericConvert final : public EExpression {
public:
    ENumericConvert(std::unique_ptr<EExpression> source, value::TypeTags target) : _target(target) {
        _nodes.emplace_back(std::move(source));
        validateNodes();
        invariant(
            target == value::TypeTags::NumberInt32 || target == value::TypeTags::NumberInt64 ||
            target == value::TypeTags::NumberDouble || target == value::TypeTags::NumberDecimal);
    }

    std::unique_ptr<EExpression> clone() const override;

    vm::CodeFragment compileDirect(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

    size_t estimateSize() const final;

private:
    value::TypeTags _target;
};

/**
 * Behavior variants for bit tests supported by match expressions $bitsAllClear, $bitsAllSet,
 * $bitsAnyClear, $bitsAnySet.
 */
enum class BitTestBehavior : int32_t {
    AllSet,
    AnyClear,
    AllClear,
    AnySet,
};
}  // namespace sbe
}  // namespace mongo
