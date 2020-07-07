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

#include <memory>
#include <string>
#include <vector>

#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {
namespace sbe {
using SpoolBuffer = std::vector<value::MaterializedRow>;

class PlanStage;
struct CompileCtx {
    value::SlotAccessor* getAccessor(value::SlotId slot);
    std::shared_ptr<SpoolBuffer> getSpoolBuffer(SpoolId spool);

    void pushCorrelated(value::SlotId slot, value::SlotAccessor* accessor);
    void popCorrelated();

    PlanStage* root{nullptr};
    value::SlotAccessor* accumulator{nullptr};
    std::vector<std::pair<value::SlotId, value::SlotAccessor*>> correlated;
    stdx::unordered_map<SpoolId, std::shared_ptr<SpoolBuffer>> spoolBuffers;
    bool aggExpression{false};
};

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
    virtual ~EExpression() = default;

    /**
     * The idiomatic C++ pattern of object cloning. Expressions must be fully copyable as every
     * thread in parallel execution needs its own private copy.
     */
    virtual std::unique_ptr<EExpression> clone() const = 0;

    /**
     * Returns bytecode directly executable by VM.
     */
    virtual std::unique_ptr<vm::CodeFragment> compile(CompileCtx& ctx) const = 0;

    virtual std::vector<DebugPrinter::Block> debugPrint() const = 0;

protected:
    std::vector<std::unique_ptr<EExpression>> _nodes;

    /**
     * Expressions can never be constructed with nullptr children.
     */
    void validateNodes() {
        for (auto& node : _nodes) {
            invariant(node);
        }
    }
};

template <typename T, typename... Args>
inline std::unique_ptr<EExpression> makeE(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename... Ts>
inline std::vector<std::unique_ptr<EExpression>> makeEs(Ts&&... pack) {
    std::vector<std::unique_ptr<EExpression>> exprs;

    (exprs.emplace_back(std::forward<Ts>(pack)), ...);

    return exprs;
}

namespace detail {
// base case
inline void makeEM_unwind(value::SlotMap<std::unique_ptr<EExpression>>& result,
                          value::SlotId slot,
                          std::unique_ptr<EExpression> expr) {
    result.emplace(slot, std::move(expr));
}

// recursive case
template <typename... Ts>
inline void makeEM_unwind(value::SlotMap<std::unique_ptr<EExpression>>& result,
                          value::SlotId slot,
                          std::unique_ptr<EExpression> expr,
                          Ts&&... rest) {
    result.emplace(slot, std::move(expr));
    makeEM_unwind(result, std::forward<Ts>(rest)...);
}
}  // namespace detail

template <typename... Ts>
auto makeEM(Ts&&... pack) {
    value::SlotMap<std::unique_ptr<EExpression>> result;
    if constexpr (sizeof...(pack) > 0) {
        result.reserve(sizeof...(Ts) / 2);
        detail::makeEM_unwind(result, std::forward<Ts>(pack)...);
    }
    return result;
}

template <typename... Args>
auto makeSV(Args&&... args) {
    value::SlotVector v;
    v.reserve(sizeof...(Args));
    (v.push_back(std::forward<Args>(args)), ...);
    return v;
}

/**
 * This is a constant expression. It assumes the ownership of the input constant.
 */
class EConstant final : public EExpression {
public:
    EConstant(value::TypeTags tag, value::Value val) : _tag(tag), _val(val) {}
    EConstant(std::string_view str) {
        // Views are non-owning so we have to make a copy.
        auto [tag, val] = value::makeNewString(str);

        _tag = tag;
        _val = val;
    }

    ~EConstant() override {
        value::releaseValue(_tag, _val);
    }

    std::unique_ptr<EExpression> clone() const override;

    std::unique_ptr<vm::CodeFragment> compile(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

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
    EVariable(value::SlotId var) : _var(var), _frameId(boost::none) {}
    EVariable(FrameId frameId, value::SlotId var) : _var(var), _frameId(frameId) {}

    std::unique_ptr<EExpression> clone() const override;

    std::unique_ptr<vm::CodeFragment> compile(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

private:
    value::SlotId _var;
    boost::optional<FrameId> _frameId;
};

/**
 * This is a binary primitive (builtin) operation.
 */
class EPrimBinary final : public EExpression {
public:
    enum Op {
        add,
        sub,

        mul,
        div,

        lessEq,
        less,
        greater,
        greaterEq,

        eq,
        neq,

        cmp3w,

        // Logical operations are short - circuiting.
        logicAnd,
        logicOr,
    };

    EPrimBinary(Op op, std::unique_ptr<EExpression> lhs, std::unique_ptr<EExpression> rhs)
        : _op(op) {
        _nodes.emplace_back(std::move(lhs));
        _nodes.emplace_back(std::move(rhs));
        validateNodes();
    }

    std::unique_ptr<EExpression> clone() const override;

    std::unique_ptr<vm::CodeFragment> compile(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

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

    std::unique_ptr<vm::CodeFragment> compile(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

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
    EFunction(std::string_view name, std::vector<std::unique_ptr<EExpression>> args) : _name(name) {
        _nodes = std::move(args);
        validateNodes();
    }

    std::unique_ptr<EExpression> clone() const override;

    std::unique_ptr<vm::CodeFragment> compile(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

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

    std::unique_ptr<vm::CodeFragment> compile(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;
};

/**
 * This is a let expression that can be used to define local variables.
 */
class ELocalBind final : public EExpression {
public:
    ELocalBind(FrameId frameId,
               std::vector<std::unique_ptr<EExpression>> binds,
               std::unique_ptr<EExpression> in)
        : _frameId(frameId) {
        _nodes = std::move(binds);
        _nodes.emplace_back(std::move(in));
        validateNodes();
    }

    std::unique_ptr<EExpression> clone() const override;

    std::unique_ptr<vm::CodeFragment> compile(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

private:
    FrameId _frameId;
};

/**
 * Evaluating this expression will throw an exception with given error code and message.
 */
class EFail final : public EExpression {
public:
    EFail(ErrorCodes::Error code, std::string message)
        : _code(code), _message(std::move(message)) {}

    std::unique_ptr<EExpression> clone() const override;

    std::unique_ptr<vm::CodeFragment> compile(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

private:
    ErrorCodes::Error _code;
    std::string _message;
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

    std::unique_ptr<vm::CodeFragment> compile(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

private:
    value::TypeTags _target;
};
}  // namespace sbe
}  // namespace mongo
