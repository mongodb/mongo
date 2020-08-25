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
#include "mongo/db/exec/sbe/values/id_generators.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace sbe {
using SpoolBuffer = std::vector<value::MaterializedRow>;

/**
 * A holder for slots and accessors which are used in a PlanStage tree but:
 *  - Cannot be made constants due to restrictions on the lifetime of such values (e.g., they're
 *    singleton instances owned somewhere else).
 *  - Can be changed in runtime outside of the PlanStage tree (e.g., a resume recordId changed by a
 *    PlanExecutor).
 *
 * A RuntimeEnvironment object is created once per an execution thread. That means that each
 * producer and consumer in a parallel plan will have their own compilation environment, with their
 * own slot accessors. However, slot accessors in each of such environment will access shared data,
 * which is the same across all environments.
 *
 * To avoid data races, the values stored in the runtime environment are considered read-only when
 * used with a parallel plan. An attempt to change any slot with 'resetValue' will result in a user
 * exception.
 *
 * If the runtime environment is used in a serial plan, modifications of the slots is allowed.
 */
class RuntimeEnvironment {
public:
    RuntimeEnvironment() = default;
    RuntimeEnvironment(RuntimeEnvironment&&) = delete;
    RuntimeEnvironment& operator=(const RuntimeEnvironment&) = delete;
    RuntimeEnvironment& operator=(const RuntimeEnvironment&&) = delete;
    ~RuntimeEnvironment();

    /**
     * Registers and returns a SlotId for the given slot 'type'. The 'slotIdGenerartor' is used to
     * generated a new SlotId for the given slot 'type', which is then registered with this
     * environment by creating a new SlotAccessor. The value 'val' is then stored within the
     * SlotAccessor and the newly generated SlotId is returned.
     *
     * Both owned and unowned values can be stored in the runtime environment.
     *
     * A user exception is raised if this slot 'type' has been already registered.
     */
    value::SlotId registerSlot(StringData type,
                               value::TypeTags tag,
                               value::Value val,
                               bool owned,
                               value::SlotIdGenerator* slotIdGenerator);

    /**
     * Returns a SlotId registered for the given slot 'type'. If the slot hasn't been registered
     * yet, a user exception is raised..
     */
    value::SlotId getSlot(StringData type);

    /**
     * Store the given value in the specified slot within this runtime environment instance.
     *
     * A user exception is raised if the SlotId is not registered within this environment, or
     * if this environment is used with a parallel plan.
     */
    void resetSlot(value::SlotId slot, value::TypeTags tag, value::Value val, bool owned);

    /**
     * Returns a SlotAccessor for the given SlotId which must be previously registered within this
     * Environment by invoking 'registerSlot' method.
     *
     * A user exception is raised if the SlotId is not registered within this environment.
     */
    value::SlotAccessor* getAccessor(value::SlotId slot);

    /**
     * Make a copy of his environment. The new environment will have its own set of SlotAccessors
     * pointing to the same shared data holding slot values.
     *
     * To create a copy of the runtime environment for a parallel execution plan, the 'isSmp' flag
     * must be set to 'true'. This will result in this environment being unconverted to a parallel
     * environment, as well as the newly created copy.
     */
    std::unique_ptr<RuntimeEnvironment> makeCopy(bool isSmp);

    /**
     * Dumps all the slots currently defined in this environment into the given string builder.
     */
    void debugString(StringBuilder* builder);

private:
    RuntimeEnvironment(const RuntimeEnvironment&);

    struct State {
        auto pushSlot(StringData type, value::SlotId slot) {
            auto index = vals.size();

            typeTags.push_back(value::TypeTags::Nothing);
            vals.push_back(0);
            owned.push_back(false);

            auto [it, inserted] = slots.emplace(type, std::make_pair(slot, index));
            uassert(4946302, str::stream() << "duplicate environment slot: " << slot, inserted);
            return index;
        }

        StringMap<std::pair<value::SlotId, size_t>> slots;
        std::vector<value::TypeTags> typeTags;
        std::vector<value::Value> vals;
        std::vector<bool> owned;
    };

    class Accessor final : public value::SlotAccessor {
    public:
        Accessor(RuntimeEnvironment* env, size_t index) : _env{env}, _index{index} {}

        std::pair<value::TypeTags, value::Value> getViewOfValue() const override {
            return {_env->_state->typeTags[_index], _env->_state->vals[_index]};
        }

        std::pair<value::TypeTags, value::Value> copyOrMoveValue() override {
            // Always make a copy.
            return copyValue(_env->_state->typeTags[_index], _env->_state->vals[_index]);
        }

        void reset(bool owned, value::TypeTags tag, value::Value val) {
            release();

            _env->_state->typeTags[_index] = tag;
            _env->_state->vals[_index] = val;
            _env->_state->owned[_index] = owned;
        }

    private:
        void release() {
            if (_env->_state->owned[_index]) {
                releaseValue(_env->_state->typeTags[_index], _env->_state->vals[_index]);
                _env->_state->owned[_index] = false;
            }
        }

        RuntimeEnvironment* const _env;
        const size_t _index;
    };

    void emplaceAccessor(value::SlotId slot, size_t index) {
        _accessors.emplace(slot, Accessor{this, index});
    }

    std::shared_ptr<State> _state{std::make_shared<State>()};
    value::SlotMap<Accessor> _accessors;
    bool _isSmp{false};

    friend class Accessor;
};

class PlanStage;
struct CompileCtx {
    CompileCtx(std::unique_ptr<RuntimeEnvironment> env) : env{std::move(env)} {}

    value::SlotAccessor* getAccessor(value::SlotId slot);
    std::shared_ptr<SpoolBuffer> getSpoolBuffer(SpoolId spool);

    void pushCorrelated(value::SlotId slot, value::SlotAccessor* accessor);
    void popCorrelated();

    CompileCtx makeCopy(bool isSmp);

    PlanStage* root{nullptr};
    value::SlotAccessor* accumulator{nullptr};
    std::vector<std::pair<value::SlotId, value::SlotAccessor*>> correlated;
    stdx::unordered_map<SpoolId, std::shared_ptr<SpoolBuffer>> spoolBuffers;
    bool aggExpression{false};

private:
    // Any data that a PlanStage needs from the RuntimeEnvironment should not be accessed directly
    // but insteady by looking up the corresponding slots. These slots are set up during the process
    // of building PlanStages, so the PlanStages themselves should never need to add new slots to
    // the RuntimeEnvironment.
    std::unique_ptr<RuntimeEnvironment> env;
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

/**
 * This is a type match expression. It checks if a variable's BSONType is present within a given
 * set of BSONTypes encoded as a bitmask (_typeMask). If the variable's BSONType is in the set,
 * this expression returns true, otherwise it returns false.
 */
class ETypeMatch final : public EExpression {
public:
    ETypeMatch(std::unique_ptr<EExpression> variable, uint32_t typeMask) : _typeMask(typeMask) {
        _nodes.emplace_back(std::move(variable));
        validateNodes();
    }

    std::unique_ptr<EExpression> clone() const override;

    std::unique_ptr<vm::CodeFragment> compile(CompileCtx& ctx) const override;

    std::vector<DebugPrinter::Block> debugPrint() const override;

private:
    uint32_t _typeMask;
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
