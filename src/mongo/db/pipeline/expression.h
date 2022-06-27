/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/data_range.h"
#include "mongo/platform/basic.h"

#include <algorithm>
#include <boost/intrusive_ptr.hpp>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/db/server_options.h"
#include "mongo/db/update/pattern_cmp.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/pcre.h"
#include "mongo/util/str.h"

namespace mongo {

class BSONArrayBuilder;
class BSONElement;
class BSONObjBuilder;
class DocumentSource;

/**
 * Registers a Parser so it can be called from parseExpression and friends.
 *
 * As an example, if your expression looks like {"$foo": [1,2,3]} you would add this line:
 * REGISTER_STABLE_EXPRESSION(foo, ExpressionFoo::parse);
 *
 * An expression registered this way can be used in any featureCompatibilityVersion and will be
 * considered part of the stable API.
 */
#define REGISTER_STABLE_EXPRESSION(key, parser)                       \
    MONGO_INITIALIZER_GENERAL(addToExpressionParserMap_##key,         \
                              ("BeginExpressionRegistration"),        \
                              ("EndExpressionRegistration"))          \
    (InitializerContext*) {                                           \
        Expression::registerExpression("$" #key,                      \
                                       (parser),                      \
                                       AllowedWithApiStrict::kAlways, \
                                       AllowedWithClientType::kAny,   \
                                       boost::none);                  \
    }

/**
 * Registers a Parser so it can be called from parseExpression and friends. Use this version if your
 * expression can only be persisted to a catalog data structure in a feature compatibility version
 * >= X.
 *
 * As an example, if your expression looks like {"$foo": [1,2,3]}, and can only be used in a feature
 * compatibility version >= X, you would add this line:
 * REGISTER_EXPRESSION_WITH_MIN_VERSION(
 *  foo,
 *  ExpressionFoo::parse,
 *  AllowedWithApiStrict::kNeverInVersion1,
 *  AllowedWithClientType::kAny,
 *  X);
 *
 * Generally new language features should be excluded from the stable API for a stabilization period
 * to allow for incorporating feedback or fixing accidental semantics bugs.
 *
 * If 'allowedWithApiStrict' is set to 'kSometimes', this expression is expected to register its own
 * parser and enforce the 'sometimes' behavior during that invocation. No extra validation will be
 * done here.
 */
#define REGISTER_EXPRESSION_WITH_MIN_VERSION(                                               \
    key, parser, allowedWithApiStrict, allowedClientType, minVersion)                       \
    MONGO_INITIALIZER_GENERAL(addToExpressionParserMap_##key,                               \
                              ("BeginExpressionRegistration"),                              \
                              ("EndExpressionRegistration"))                                \
    (InitializerContext*) {                                                                 \
        Expression::registerExpression(                                                     \
            "$" #key, (parser), (allowedWithApiStrict), (allowedClientType), (minVersion)); \
    }

/**
 * Registers a Parser only if test commands are enabled. Use this if your expression is only used
 * for testing purposes.
 */
#define REGISTER_TEST_EXPRESSION(key, allowedWithApiStrict, allowedClientType, parser)       \
    MONGO_INITIALIZER_GENERAL(addToExpressionParserMap_##key,                                \
                              ("BeginExpressionRegistration"),                               \
                              ("EndExpressionRegistration"))                                 \
    (InitializerContext*) {                                                                  \
        if (!getTestCommandsEnabled()) {                                                     \
            return;                                                                          \
        }                                                                                    \
        Expression::registerExpression(                                                      \
            "$" #key, (parser), (allowedWithApiStrict), (allowedClientType), (boost::none)); \
    }

/**
 * Like REGISTER_EXPRESSION_WITH_MIN_VERSION, except you can also specify a condition,
 * evaluated during startup, that decides whether to register the parser.
 *
 * For example, you could check a feature flag, and register the parser only when it's enabled.
 *
 * Note that the condition is evaluated only once, during a MONGO_INITIALIZER. Don't specify
 * a condition that can change at runtime, such as FCV. (Feature flags are ok, because they
 * cannot be toggled at runtime.)
 *
 * This is the most general REGISTER_EXPRESSION* macro, which all others should delegate to.
 */
#define REGISTER_EXPRESSION_CONDITIONALLY(                                                  \
    key, parser, allowedWithApiStrict, allowedClientType, minVersion, ...)                  \
    MONGO_INITIALIZER_GENERAL(addToExpressionParserMap_##key,                               \
                              ("BeginExpressionRegistration"),                              \
                              ("EndExpressionRegistration"))                                \
    (InitializerContext*) {                                                                 \
        if (!(__VA_ARGS__)) {                                                               \
            return;                                                                         \
        }                                                                                   \
        Expression::registerExpression(                                                     \
            "$" #key, (parser), (allowedWithApiStrict), (allowedClientType), (minVersion)); \
    }

class Expression : public RefCountable {
public:
    using Parser = std::function<boost::intrusive_ptr<Expression>(
        ExpressionContext* const, BSONElement, const VariablesParseState&)>;

    /**
     * Represents new paths computed by an expression. Computed paths are partitioned into renames
     * and non-renames. See the comments for Expression::getComputedPaths() for more information.
     */
    struct ComputedPaths {
        // Non-rename computed paths.
        std::set<std::string> paths;

        // Mappings from the old name of a path before applying this expression, to the new one
        // after applying this expression.
        StringMap<std::string> renames;
    };

    virtual ~Expression(){};

    /**
     * Optimize the Expression.
     *
     * This provides an opportunity to do constant folding, or to collapse nested operators that
     * have the same precedence, such as $add, $and, or $or.
     *
     * The Expression will be replaced with the return value, which may or may not be the same
     * object. In the case of constant folding, a computed expression may be replaced by a constant.
     *
     * Returns the optimized Expression.
     */
    virtual boost::intrusive_ptr<Expression> optimize() {
        return this;
    }

    /**
     * Add the fields and variables used in this expression to 'deps'. References to variables which
     * are local to a particular expression will be filtered out of the tracker upon return.
     */
    void addDependencies(DepsTracker* deps) const {
        _doAddDependencies(deps);

        // Filter out references to any local variables.
        if (_boundaryVariableId) {
            deps->vars.erase(deps->vars.upper_bound(*_boundaryVariableId), deps->vars.end());
        }
    }

    /**
     * Convenience wrapper around addDependencies.
     */
    DepsTracker getDependencies() const {
        DepsTracker deps;
        addDependencies(&deps);
        return deps;
    }

    /**
     * Serialize the Expression tree recursively.
     *
     * If 'explain' is false, the returned Value must result in the same Expression when parsed by
     * parseOperand().
     */
    virtual Value serialize(bool explain) const = 0;

    /**
     * Evaluate the expression with respect to the Document given by 'root' and the Variables given
     * by 'variables'. It is an error to supply a Variables argument whose built-in variables (like
     * $$NOW) are not set. This method is thread-safe, so long as the 'variables' passed in here is
     * not shared between threads.
     */
    virtual Value evaluate(const Document& root, Variables* variables) const = 0;

    /**
     * Returns information about the paths computed by this expression. This only needs to be
     * overridden by expressions that have renaming semantics, where optimization code could take
     * advantage of knowledge of these renames.
     *
     * Partitions paths involved in this expression into the set of computed paths and the set of
     * ("new" => "old") rename mappings. Here "new" refers to the name of the path after applying
     * this expression, whereas "old" refers to the name of the path before applying this
     * expression.
     *
     * The 'exprFieldPath' is the field path at which the result of this expression will be stored.
     * This is used to determine the value of the "new" path created by the rename.
     *
     * The 'renamingVar' is needed for checking whether a field path is a rename. For example, at
     * the top level only field paths that begin with the ROOT variable, as in "$$ROOT.path", are
     * renames. A field path such as "$$var.path" is not a rename.
     *
     * Now consider the example of a rename expressed via a $map:
     *
     *    {$map: {input: "$array", as: "iter", in: {...}}}
     *
     * In this case, only field paths inside the "in" clause beginning with "iter", such as
     * "$$iter.path", are renames.
     */
    virtual ComputedPaths getComputedPaths(const std::string& exprFieldPath,
                                           Variables::Id renamingVar = Variables::kRootId) const {
        return {{exprFieldPath}, {}};
    }

    /**
     * This allows an arbitrary class to implement logic which gets dispatched to at runtime
     * depending on the type of the Expression.
     */
    virtual void acceptVisitor(ExpressionMutableVisitor* visitor) = 0;
    virtual void acceptVisitor(ExpressionConstVisitor* visitor) const = 0;

    /**
     * Parses a BSON Object that could represent an object literal or a functional expression like
     * $add.
     *
     * Calls parseExpression() on any sub-document (including possibly the entire document) which
     * consists of a single field name starting with a '$'.
     */
    static boost::intrusive_ptr<Expression> parseObject(ExpressionContext* expCtx,
                                                        BSONObj obj,
                                                        const VariablesParseState& vps);

    /**
     * Parses a BSONObj which has already been determined to be a functional expression.
     *
     * Throws an error if 'obj' does not contain exactly one field, or if that field's name does not
     * match a registered expression name.
     */
    static boost::intrusive_ptr<Expression> parseExpression(ExpressionContext* expCtx,
                                                            BSONObj obj,
                                                            const VariablesParseState& vps);

    /**
     * Parses a BSONElement which is an argument to an Expression.
     *
     * An argument is allowed to be another expression, or a literal value, so this can call
     * parseObject(), ExpressionFieldPath::parse(), ExpressionArray::parse(), or
     * ExpressionConstant::parse() as necessary.
     */
    static boost::intrusive_ptr<Expression> parseOperand(ExpressionContext* expCtx,
                                                         BSONElement exprElement,
                                                         const VariablesParseState& vps);

    /**
     * Return whether 'name' refers to an expression in the language.
     */
    static bool isExpressionName(StringData name);

    /*
      Produce a field path std::string with the field prefix removed.

      Throws an error if the field prefix is not present.

      @param prefixedField the prefixed field
      @returns the field path with the prefix removed
     */
    static std::string removeFieldPrefix(const std::string& prefixedField);

    /**
     * Registers an Parser so it can be called from parseExpression.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_EXPRESSION macro defined in this
     * file.
     */
    static void registerExpression(
        std::string key,
        Parser parser,
        AllowedWithApiStrict allowedWithApiStrict,
        AllowedWithClientType allowedWithClientType,
        boost::optional<multiversion::FeatureCompatibilityVersion> requiredMinVersion);

    const auto& getChildren() const {
        return _children;
    }
    auto& getChildren() {
        return _children;
    }

    auto getExpressionContext() const {
        return _expCtx;
    }

protected:
    using ExpressionVector = std::vector<boost::intrusive_ptr<Expression>>;

    Expression(ExpressionContext* const expCtx) : Expression(expCtx, {}) {}

    Expression(ExpressionContext* const expCtx, ExpressionVector&& children)
        : _children(std::move(children)), _expCtx(expCtx) {
        auto varIds = _expCtx->variablesParseState.getDefinedVariableIDs();
        if (!varIds.empty()) {
            _boundaryVariableId = *std::prev(varIds.end());
        }
    }

    virtual void _doAddDependencies(DepsTracker* deps) const = 0;

    /**
     * Owning container for all sub-Expressions.
     *
     * Some derived classes contain named fields since they originate from user syntax containing
     * field names. These classes contain alternate data structures or object members for accessing
     * children. These structures or object members are expected to reference this data structure.
     * In addition this structure should not be modified by named-field derived classes to avoid
     * invalidating references.
     */
    ExpressionVector _children;

private:
    boost::optional<Variables::Id> _boundaryVariableId;
    ExpressionContext* const _expCtx;
};

/**
 * Inherit from ExpressionVariadic or ExpressionFixedArity instead of directly from this class.
 */
class ExpressionNary : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() override;
    Value serialize(bool explain) const override;

    /*
      Add an operand to the n-ary expression.

      @param pExpression the expression to add
    */
    virtual void addOperand(const boost::intrusive_ptr<Expression>& pExpression);

    virtual bool isAssociative() const {
        return false;
    }

    virtual bool isCommutative() const {
        return false;
    }

    virtual const char* getOpName() const = 0;

    virtual void validateArguments(const ExpressionVector& args) const {}

    static ExpressionVector parseArguments(ExpressionContext* expCtx,
                                           BSONElement bsonExpr,
                                           const VariablesParseState& vps);

    const ExpressionVector& getOperandList() const {
        return _children;
    }

protected:
    explicit ExpressionNary(ExpressionContext* const expCtx) : Expression(expCtx) {}
    ExpressionNary(ExpressionContext* const expCtx, ExpressionVector&& children)
        : Expression(expCtx, std::move(children)) {}

    void _doAddDependencies(DepsTracker* deps) const override;
};

/// Inherit from ExpressionVariadic or ExpressionFixedArity instead of directly from this class.
template <typename SubClass>
class ExpressionNaryBase : public ExpressionNary {
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* const expCtx,
                                                  BSONElement bsonExpr,
                                                  const VariablesParseState& vps) {
        auto expr = make_intrusive<SubClass>(expCtx);
        ExpressionVector args = parseArguments(expCtx, bsonExpr, vps);
        expr->validateArguments(args);
        expr->_children = std::move(args);
        return expr;
    }

protected:
    explicit ExpressionNaryBase(ExpressionContext* const expCtx) : ExpressionNary(expCtx) {}
    ExpressionNaryBase(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionNary(expCtx, std::move(children)) {}
};

/// Inherit from this class if your expression takes a variable number of arguments.
template <typename SubClass>
class ExpressionVariadic : public ExpressionNaryBase<SubClass> {
public:
    explicit ExpressionVariadic(ExpressionContext* const expCtx)
        : ExpressionNaryBase<SubClass>(expCtx) {}
    ExpressionVariadic(ExpressionContext* const expCtx, Expression::ExpressionVector&& children)
        : ExpressionNaryBase<SubClass>(expCtx, std::move(children)) {}
};

/**
 * Inherit from this class if your expression can take a range of arguments, e.g. if it has some
 * optional arguments.
 */
template <typename SubClass, int MinArgs, int MaxArgs>
class ExpressionRangedArity : public ExpressionNaryBase<SubClass> {
public:
    explicit ExpressionRangedArity(ExpressionContext* const expCtx)
        : ExpressionNaryBase<SubClass>(expCtx) {}
    ExpressionRangedArity(ExpressionContext* const expCtx, Expression::ExpressionVector&& children)
        : ExpressionNaryBase<SubClass>(expCtx, std::move(children)) {}

    void validateArguments(const Expression::ExpressionVector& args) const override {
        uassert(28667,
                str::stream() << "Expression " << this->getOpName() << " takes at least " << MinArgs
                              << " arguments, and at most " << MaxArgs << ", but " << args.size()
                              << " were passed in.",
                MinArgs <= args.size() && args.size() <= MaxArgs);
    }
};

/// Inherit from this class if your expression takes a fixed number of arguments.
template <typename SubClass, int NArgs>
class ExpressionFixedArity : public ExpressionNaryBase<SubClass> {
public:
    explicit ExpressionFixedArity(ExpressionContext* const expCtx)
        : ExpressionNaryBase<SubClass>(expCtx) {}
    ExpressionFixedArity(ExpressionContext* const expCtx, Expression::ExpressionVector&& children)
        : ExpressionNaryBase<SubClass>(expCtx, std::move(children)) {}

    void validateArguments(const Expression::ExpressionVector& args) const override {
        uassert(16020,
                str::stream() << "Expression " << this->getOpName() << " takes exactly " << NArgs
                              << " arguments. " << args.size() << " were passed in.",
                args.size() == NArgs);
    }
};

/**
 * Used to make Accumulators available as Expressions, e.g., to make $sum available as an Expression
 * use "REGISTER_STABLE_EXPRESSION(sum, ExpressionAccumulator<AccumulatorSum>::parse);".
 */
template <typename AccumulatorState>
class ExpressionFromAccumulator
    : public ExpressionVariadic<ExpressionFromAccumulator<AccumulatorState>> {
public:
    explicit ExpressionFromAccumulator(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionFromAccumulator<AccumulatorState>>(expCtx) {
        expCtx->sbeCompatible = false;
    }

    Value evaluate(const Document& root, Variables* variables) const final {
        AccumulatorState accum(this->getExpressionContext());
        const auto n = this->_children.size();
        // If a single array arg is given, loop through it passing each member to the accumulator.
        // If a single, non-array arg is given, pass it directly to the accumulator.
        if (n == 1) {
            Value singleVal = this->_children[0]->evaluate(root, variables);
            if (singleVal.getType() == Array) {
                for (const Value& val : singleVal.getArray()) {
                    accum.process(val, false);
                }
            } else {
                accum.process(singleVal, false);
            }
        } else {
            // If multiple arguments are given, pass all arguments to the accumulator.
            for (auto&& argument : this->_children) {
                accum.process(argument->evaluate(root, variables), false);
            }
        }
        return accum.getValue(false);
    }

    bool isAssociative() const final {
        // Return false if a single argument is given to avoid a single array argument being treated
        // as an array instead of as a list of arguments.
        if (this->_children.size() == 1) {
            return false;
        }
        return AccumulatorState(this->getExpressionContext()).isAssociative();
    }

    bool isCommutative() const final {
        return AccumulatorState(this->getExpressionContext()).isCommutative();
    }

    const char* getOpName() const final {
        return AccumulatorState::kName.rawData();
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};
template <typename AccumulatorN>
class ExpressionFromAccumulatorN : public Expression {
public:
    explicit ExpressionFromAccumulatorN(ExpressionContext* const expCtx,
                                        boost::intrusive_ptr<Expression> n,
                                        boost::intrusive_ptr<Expression> output)
        : Expression(expCtx, {n, output}), _n(n), _output(output) {
        expCtx->sbeCompatible = false;
    }

    const char* getOpName() const {
        return AccumulatorN::kName.rawData();
    }

    Value serialize(bool explain) const {
        MutableDocument md;
        AccumulatorN::serializeHelper(_n, _output, explain, md);
        return Value(DOC(getOpName() << md.freeze()));
    }

    Value evaluate(const Document& root, Variables* variables) const {
        AccumulatorN accum(this->getExpressionContext());

        // Evaluate and initialize 'n'.
        accum.startNewGroup(_n->evaluate(root, variables));

        // Verify that '_output' produces an array and pass each element to 'process'.
        auto output = _output->evaluate(root, variables);
        uassert(5788200, "Input must be an array", output.isArray());
        for (auto item : output.getArray()) {
            accum.process(item, false);
        }
        return accum.getValue(false);
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const {
        _n->addDependencies(deps);
        _output->addDependencies(deps);
    }

private:
    boost::intrusive_ptr<Expression> _n;
    boost::intrusive_ptr<Expression> _output;
};

/**
 * Inherit from this class if your expression takes exactly one numeric argument.
 */
template <typename SubClass>
class ExpressionSingleNumericArg : public ExpressionFixedArity<SubClass, 1> {
public:
    explicit ExpressionSingleNumericArg(ExpressionContext* const expCtx)
        : ExpressionFixedArity<SubClass, 1>(expCtx) {}
    explicit ExpressionSingleNumericArg(ExpressionContext* const expCtx,
                                        Expression::ExpressionVector&& children)
        : ExpressionFixedArity<SubClass, 1>(expCtx, std::move(children)) {}

    virtual ~ExpressionSingleNumericArg() = default;

    Value evaluate(const Document& root, Variables* variables) const final {
        Value arg = this->_children[0]->evaluate(root, variables);
        if (arg.nullish())
            return Value(BSONNULL);

        uassert(28765,
                str::stream() << this->getOpName() << " only supports numeric types, not "
                              << typeName(arg.getType()),
                arg.numeric());

        return evaluateNumericArg(arg);
    }

    virtual Value evaluateNumericArg(const Value& numericArg) const = 0;
};

/**
 * Inherit from this class if your expression takes exactly two numeric arguments.
 */
template <typename SubClass>
class ExpressionTwoNumericArgs : public ExpressionFixedArity<SubClass, 2> {
public:
    explicit ExpressionTwoNumericArgs(ExpressionContext* const expCtx)
        : ExpressionFixedArity<SubClass, 2>(expCtx) {}
    ExpressionTwoNumericArgs(ExpressionContext* const expCtx,
                             Expression::ExpressionVector&& children)
        : ExpressionFixedArity<SubClass, 2>(expCtx, std::move(children)) {}

    virtual ~ExpressionTwoNumericArgs() = default;

    /**
     * Evaluate performs the type checking necessary to make sure that both arguments are numeric,
     * then calls the evaluateNumericArgs on the two numeric args:
     * 1. If either input is nullish, it returns null.
     * 2. If either input is not numeric, it throws an error.
     * 3. Call evaluateNumericArgs on the two numeric args.
     */
    Value evaluate(const Document& root, Variables* variables) const final {
        Value arg1 = this->_children[0]->evaluate(root, variables);
        if (arg1.nullish())
            return Value(BSONNULL);
        uassert(51044,
                str::stream() << this->getOpName() << " only supports numeric types, not "
                              << typeName(arg1.getType()),
                arg1.numeric());
        Value arg2 = this->_children[1]->evaluate(root, variables);
        if (arg2.nullish())
            return Value(BSONNULL);
        uassert(51045,
                str::stream() << this->getOpName() << " only supports numeric types, not "
                              << typeName(arg2.getType()),
                arg2.numeric());

        return evaluateNumericArgs(arg1, arg2);
    }

    /**
     *  Evaluate the expression on exactly two numeric arguments.
     */
    virtual Value evaluateNumericArgs(const Value& numericArg1, const Value& numericArg2) const = 0;
};

/**
 * A constant expression. Repeated calls to evaluate() will always return the same thing.
 */
class ExpressionConstant final : public Expression {
public:
    ExpressionConstant(ExpressionContext* expCtx, const Value& value);

    boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root, Variables* variables) const final;
    Value serialize(bool explain) const final;

    const char* getOpName() const;

    /**
     * Creates a new ExpressionConstant with value 'value'.
     */
    static boost::intrusive_ptr<ExpressionConstant> create(ExpressionContext* expCtx,
                                                           const Value& value);

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement bsonExpr,
                                                  const VariablesParseState& vps);

    /**
     * Returns true if 'expression' is nullptr or if 'expression' is an instance of an
     * ExpressionConstant.
     */
    static bool isNullOrConstant(boost::intrusive_ptr<Expression> expression) {
        return !expression || dynamic_cast<ExpressionConstant*>(expression.get());
    }

    /**
     * Returns true if every expression in 'expressions' is either a nullptr or an instance of an
     * ExpressionConstant.
     */
    static bool allNullOrConstant(
        const std::initializer_list<boost::intrusive_ptr<Expression>>& expressions) {
        return std::all_of(expressions.begin(), expressions.end(), [](auto exp) {
            return ExpressionConstant::isNullOrConstant(exp);
        });
    }

    /**
     * Returns the constant value represented by this Expression.
     */
    Value getValue() const {
        return _value;
    }

    void setValue(const Value& value) {
        _value = value;
    };

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const override;

private:
    Value _value;
};

/**
 * Inherit from this class if your expression works with date types, and accepts either a single
 * argument which is a date, or an object {date: <date>, timezone: <string>}.
 */
template <typename SubClass>
class DateExpressionAcceptingTimeZone : public Expression {
public:
    virtual ~DateExpressionAcceptingTimeZone() {}

    Value evaluate(const Document& root, Variables* variables) const final {
        auto dateVal = _date->evaluate(root, variables);
        if (dateVal.nullish()) {
            return Value(BSONNULL);
        }
        auto date = dateVal.coerceToDate();

        if (!_timeZone) {
            return evaluateDate(date, TimeZoneDatabase::utcZone());
        }
        auto timeZoneId = _timeZone->evaluate(root, variables);
        if (timeZoneId.nullish()) {
            return Value(BSONNULL);
        }

        uassert(40533,
                str::stream() << _opName
                              << " requires a string for the timezone argument, but was given a "
                              << typeName(timeZoneId.getType()) << " (" << timeZoneId.toString()
                              << ")",
                timeZoneId.getType() == BSONType::String);

        invariant(getExpressionContext()->timeZoneDatabase);
        auto timeZone =
            getExpressionContext()->timeZoneDatabase->getTimeZone(timeZoneId.getString());
        return evaluateDate(date, timeZone);
    }

    /**
     * Always serializes to the full {date: <date arg>, timezone: <timezone arg>} format, leaving
     * off the timezone if not specified.
     */
    Value serialize(bool explain) const final {
        auto timezone = _timeZone ? _timeZone->serialize(explain) : Value();
        return Value(Document{
            {_opName,
             Document{{"date", _date->serialize(explain)}, {"timezone", std::move(timezone)}}}});
    }

    boost::intrusive_ptr<Expression> optimize() final {
        _date = _date->optimize();
        if (_timeZone) {
            _timeZone = _timeZone->optimize();
        }
        if (ExpressionConstant::allNullOrConstant({_date, _timeZone})) {
            // Everything is a constant, so we can turn into a constant.
            return ExpressionConstant::create(
                getExpressionContext(), evaluate(Document{}, &(getExpressionContext()->variables)));
        }
        return this;
    }

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* const expCtx,
                                                  BSONElement operatorElem,
                                                  const VariablesParseState& variablesParseState) {
        if (operatorElem.type() == BSONType::Object) {
            if (operatorElem.embeddedObject().firstElementFieldName()[0] == '$') {
                // Assume this is an expression specification representing the date argument
                // like {$add: [<date>, 1000]}.
                return new SubClass(expCtx,
                                    Expression::parseObject(expCtx,
                                                            operatorElem.embeddedObject(),
                                                            variablesParseState));
            } else {
                // It's an object specifying the date and timezone options like {date: <date>,
                // timezone: <timezone>}.
                auto opName = operatorElem.fieldNameStringData();
                boost::intrusive_ptr<Expression> date;
                boost::intrusive_ptr<Expression> timeZone;
                for (const auto& subElem : operatorElem.embeddedObject()) {
                    auto argName = subElem.fieldNameStringData();
                    if (argName == "date"_sd) {
                        date = Expression::parseOperand(expCtx, subElem, variablesParseState);
                    } else if (argName == "timezone"_sd) {
                        timeZone = Expression::parseOperand(expCtx, subElem, variablesParseState);
                    } else {
                        uasserted(40535,
                                  str::stream() << "unrecognized option to " << opName << ": \""
                                                << argName << "\"");
                    }
                }
                uassert(40539,
                        str::stream() << "missing 'date' argument to " << opName
                                      << ", provided: " << operatorElem,
                        date);
                return new SubClass(expCtx, std::move(date), std::move(timeZone));
            }
        } else if (operatorElem.type() == BSONType::Array) {
            auto elems = operatorElem.Array();
            uassert(
                40536,
                str::stream() << operatorElem.fieldNameStringData()
                              << " accepts exactly one argument if given an array, but was given "
                              << elems.size(),
                elems.size() == 1);
            // We accept an argument wrapped in a single array. For example, either {$week: <date>}
            // or {$week: [<date>]} are valid, but not {$week: [{date: <date>}]}.
            return new SubClass(expCtx,
                                Expression::parseOperand(expCtx, elems[0], variablesParseState));
        }
        // Exhausting the other possibilities, we are left with a literal value which should be
        // treated as the date argument.
        return new SubClass(expCtx,
                            Expression::parseOperand(expCtx, operatorElem, variablesParseState));
    }

protected:
    explicit DateExpressionAcceptingTimeZone(ExpressionContext* const expCtx,
                                             const StringData opName,
                                             boost::intrusive_ptr<Expression> date,
                                             boost::intrusive_ptr<Expression> timeZone)
        : Expression(expCtx, {date, timeZone}),
          _opName(opName),
          _date(_children[0]),
          _timeZone(_children[1]) {}

    void _doAddDependencies(DepsTracker* deps) const final {
        _date->addDependencies(deps);
        if (_timeZone) {
            _timeZone->addDependencies(deps);
        }
    }

    /**
     * Subclasses should implement this to do their actual date-related logic. Uses 'timezone' to
     * evaluate the expression against 'data'. If the user did not specify a time zone, 'timezone'
     * will represent the UTC zone.
     */
    virtual Value evaluateDate(Date_t date, const TimeZone& timezone) const = 0;

private:
    // The name of this expression, e.g. $week or $month.
    StringData _opName;

    // The expression representing the date argument.
    boost::intrusive_ptr<Expression>& _date;
    // The expression representing the timezone argument.
    boost::intrusive_ptr<Expression>& _timeZone;
};

class ExpressionAbs final : public ExpressionSingleNumericArg<ExpressionAbs> {
public:
    explicit ExpressionAbs(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg<ExpressionAbs>(expCtx) {}
    explicit ExpressionAbs(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionSingleNumericArg<ExpressionAbs>(expCtx, std::move(children)) {}

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionAdd final : public ExpressionVariadic<ExpressionAdd> {
public:
    /**
     * Adds two values as if by {$add: [{$const: lhs}, {$const: rhs}]}.
     *
     * If either argument is nullish, returns BSONNULL.
     *
     * Otherwise, returns ErrorCodes::TypeMismatch.
     */
    static StatusWith<Value> apply(Value lhs, Value rhs);

    explicit ExpressionAdd(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionAdd>(expCtx) {}

    ExpressionAdd(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionAdd>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionAllElementsTrue final : public ExpressionFixedArity<ExpressionAllElementsTrue, 1> {
public:
    explicit ExpressionAllElementsTrue(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionAllElementsTrue, 1>(expCtx) {
        expCtx->sbeCompatible = false;
    }
    ExpressionAllElementsTrue(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionAllElementsTrue, 1>(expCtx, std::move(children)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionAnd final : public ExpressionVariadic<ExpressionAnd> {
public:
    explicit ExpressionAnd(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionAnd>(expCtx) {}

    ExpressionAnd(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionAnd>(expCtx, std::move(children)) {}

    boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionAnyElementTrue final : public ExpressionFixedArity<ExpressionAnyElementTrue, 1> {
public:
    explicit ExpressionAnyElementTrue(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionAnyElementTrue, 1>(expCtx) {}
    ExpressionAnyElementTrue(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionAnyElementTrue, 1>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionArray final : public ExpressionVariadic<ExpressionArray> {
public:
    explicit ExpressionArray(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionArray>(expCtx) {
        expCtx->sbeCompatible = false;
    }

    ExpressionArray(ExpressionContext* const expCtx,
                    std::vector<boost::intrusive_ptr<Expression>>&& children)
        : ExpressionVariadic<ExpressionArray>(expCtx) {
        _children = std::move(children);
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    Value serialize(bool explain) const final;

    static boost::intrusive_ptr<ExpressionArray> create(
        ExpressionContext* const expCtx, std::vector<boost::intrusive_ptr<Expression>>&& children) {
        return make_intrusive<ExpressionArray>(expCtx, std::move(children));
    }

    boost::intrusive_ptr<Expression> optimize() final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionArrayElemAt final : public ExpressionFixedArity<ExpressionArrayElemAt, 2> {
public:
    explicit ExpressionArrayElemAt(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionArrayElemAt, 2>(expCtx) {}

    ExpressionArrayElemAt(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionArrayElemAt, 2>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionFirst final : public ExpressionFixedArity<ExpressionFirst, 1> {
public:
    explicit ExpressionFirst(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionFirst, 1>(expCtx) {}

    ExpressionFirst(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionFirst, 1>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionLast final : public ExpressionFixedArity<ExpressionLast, 1> {
public:
    explicit ExpressionLast(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionLast, 1>(expCtx) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionObjectToArray final : public ExpressionFixedArity<ExpressionObjectToArray, 1> {
public:
    explicit ExpressionObjectToArray(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionObjectToArray, 1>(expCtx) {
        expCtx->sbeCompatible = false;
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionArrayToObject final : public ExpressionFixedArity<ExpressionArrayToObject, 1> {
public:
    explicit ExpressionArrayToObject(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionArrayToObject, 1>(expCtx) {
        expCtx->sbeCompatible = false;
    }

    ExpressionArrayToObject(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionArrayToObject, 1>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionBsonSize final : public ExpressionFixedArity<ExpressionBsonSize, 1> {
public:
    explicit ExpressionBsonSize(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionBsonSize, 1>(expCtx) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final {
        return "$bsonSize";
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionCeil final : public ExpressionSingleNumericArg<ExpressionCeil> {
public:
    explicit ExpressionCeil(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg<ExpressionCeil>(expCtx) {}
    explicit ExpressionCeil(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionSingleNumericArg<ExpressionCeil>(expCtx, std::move(children)) {}

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionCoerceToBool final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root, Variables* variables) const final;
    Value serialize(bool explain) const final;

    static boost::intrusive_ptr<ExpressionCoerceToBool> create(
        ExpressionContext* expCtx, boost::intrusive_ptr<Expression> pExpression);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    ExpressionCoerceToBool(ExpressionContext* expCtx, boost::intrusive_ptr<Expression> pExpression);

    boost::intrusive_ptr<Expression>& pExpression;
};


class ExpressionCompare final : public ExpressionFixedArity<ExpressionCompare, 2> {
public:
    /**
     * Enumeration of comparison operators. Any changes to these values require adjustment of
     * the lookup table in the implementation.
     */
    enum CmpOp {
        EQ = 0,   // return true for a == b, false otherwise
        NE = 1,   // return true for a != b, false otherwise
        GT = 2,   // return true for a > b, false otherwise
        GTE = 3,  // return true for a >= b, false otherwise
        LT = 4,   // return true for a < b, false otherwise
        LTE = 5,  // return true for a <= b, false otherwise
        CMP = 6,  // return -1, 0, 1 for a < b, a == b, a > b
    };

    ExpressionCompare(ExpressionContext* const expCtx, CmpOp cmpOp)
        : ExpressionFixedArity(expCtx), cmpOp(cmpOp) {}
    ExpressionCompare(ExpressionContext* const expCtx, CmpOp cmpOp, ExpressionVector&& children)
        : ExpressionFixedArity(expCtx, std::move(children)), cmpOp(cmpOp) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    CmpOp getOp() const {
        return cmpOp;
    }

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement bsonExpr,
                                                  const VariablesParseState& vps,
                                                  CmpOp cmpOp);

    static boost::intrusive_ptr<ExpressionCompare> create(
        ExpressionContext* expCtx,
        CmpOp cmpOp,
        const boost::intrusive_ptr<Expression>& exprLeft,
        const boost::intrusive_ptr<Expression>& exprRight);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    CmpOp cmpOp;
};


class ExpressionConcat final : public ExpressionVariadic<ExpressionConcat> {
public:
    explicit ExpressionConcat(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionConcat>(expCtx) {}
    ExpressionConcat(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionConcat>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionConcatArrays final : public ExpressionVariadic<ExpressionConcatArrays> {
public:
    explicit ExpressionConcatArrays(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionConcatArrays>(expCtx) {}

    ExpressionConcatArrays(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionConcatArrays>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionCond final : public ExpressionFixedArity<ExpressionCond, 3> {
public:
    explicit ExpressionCond(ExpressionContext* const expCtx) : Base(expCtx) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;
    boost::intrusive_ptr<Expression> optimize() final;

    static boost::intrusive_ptr<Expression> create(
        ExpressionContext* expCtx,
        boost::intrusive_ptr<Expression> ifExp,
        boost::intrusive_ptr<Expression> elseExpr,
        boost::intrusive_ptr<Expression> thenExpr = nullptr);

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    typedef ExpressionFixedArity<ExpressionCond, 3> Base;
};

class ExpressionDateFromString final : public Expression {
public:
    ExpressionDateFromString(ExpressionContext* expCtx,
                             boost::intrusive_ptr<Expression> dateString,
                             boost::intrusive_ptr<Expression> timeZone,
                             boost::intrusive_ptr<Expression> format,
                             boost::intrusive_ptr<Expression> onNull,
                             boost::intrusive_ptr<Expression> onError);

    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root, Variables* variables) const final;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    boost::intrusive_ptr<Expression>& _dateString;
    boost::intrusive_ptr<Expression>& _timeZone;
    boost::intrusive_ptr<Expression>& _format;
    boost::intrusive_ptr<Expression>& _onNull;
    boost::intrusive_ptr<Expression>& _onError;
};

class ExpressionDateFromParts final : public Expression {
public:
    ExpressionDateFromParts(ExpressionContext* expCtx,
                            boost::intrusive_ptr<Expression> year,
                            boost::intrusive_ptr<Expression> month,
                            boost::intrusive_ptr<Expression> day,
                            boost::intrusive_ptr<Expression> hour,
                            boost::intrusive_ptr<Expression> minute,
                            boost::intrusive_ptr<Expression> second,
                            boost::intrusive_ptr<Expression> millisecond,
                            boost::intrusive_ptr<Expression> isoWeekYear,
                            boost::intrusive_ptr<Expression> isoWeek,
                            boost::intrusive_ptr<Expression> isoDayOfWeek,
                            boost::intrusive_ptr<Expression> timeZone);

    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root, Variables* variables) const final;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    /**
     * This function checks whether a field is a number.
     *
     * If 'field' is null, the default value is returned trough the 'returnValue' out
     * parameter and the function returns true.
     *
     * If 'field' is not null:
     * - if the value is "nullish", the function returns false.
     * - if the value can not be coerced to an integral value, a UserException is thrown.
     * - otherwise, the coerced integral value is returned through the 'returnValue'
     *   out parameter, and the function returns true.
     */
    bool evaluateNumberWithDefault(const Document& root,
                                   const Expression* field,
                                   StringData fieldName,
                                   long long defaultValue,
                                   long long* returnValue,
                                   Variables* variables) const;

    /**
     * This function has the same behavior as evaluteNumberWithDefault(), except that it uasserts if
     * the resulting value is not in the range defined by kMaxValueForDatePart and
     * kMinValueForDatePart.
     */
    bool evaluateNumberWithDefaultAndBounds(const Document& root,
                                            const Expression* field,
                                            StringData fieldName,
                                            long long defaultValue,
                                            long long* returnValue,
                                            Variables* variables) const;

    boost::intrusive_ptr<Expression>& _year;
    boost::intrusive_ptr<Expression>& _month;
    boost::intrusive_ptr<Expression>& _day;
    boost::intrusive_ptr<Expression>& _hour;
    boost::intrusive_ptr<Expression>& _minute;
    boost::intrusive_ptr<Expression>& _second;
    boost::intrusive_ptr<Expression>& _millisecond;
    boost::intrusive_ptr<Expression>& _isoWeekYear;
    boost::intrusive_ptr<Expression>& _isoWeek;
    boost::intrusive_ptr<Expression>& _isoDayOfWeek;
    boost::intrusive_ptr<Expression>& _timeZone;

    // Some date conversions spend a long time iterating through date tables when dealing with large
    // input numbers, so we place a reasonable limit on the magnitude of any argument to
    // $dateFromParts: inputs that fit within a 16-bit int are permitted.
    static constexpr long long kMaxValueForDatePart = std::numeric_limits<int16_t>::max();
    static constexpr long long kMinValueForDatePart = std::numeric_limits<int16_t>::lowest();
};

class ExpressionDateToParts final : public Expression {
public:
    /**
     * The iso8601 argument controls whether to output ISO8601 elements or natural calendar.
     */
    ExpressionDateToParts(ExpressionContext* expCtx,
                          boost::intrusive_ptr<Expression> date,
                          boost::intrusive_ptr<Expression> timeZone,
                          boost::intrusive_ptr<Expression> iso8601);

    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root, Variables* variables) const final;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    boost::optional<int> evaluateIso8601Flag(const Document& root, Variables* variables) const;

    boost::intrusive_ptr<Expression>& _date;
    boost::intrusive_ptr<Expression>& _timeZone;
    boost::intrusive_ptr<Expression>& _iso8601;
};

class ExpressionDateToString final : public Expression {
public:
    ExpressionDateToString(ExpressionContext* expCtx,
                           boost::intrusive_ptr<Expression> format,
                           boost::intrusive_ptr<Expression> date,
                           boost::intrusive_ptr<Expression> timeZone,
                           boost::intrusive_ptr<Expression> onNull);
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root, Variables* variables) const final;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    boost::intrusive_ptr<Expression>& _format;
    boost::intrusive_ptr<Expression>& _date;
    boost::intrusive_ptr<Expression>& _timeZone;
    boost::intrusive_ptr<Expression>& _onNull;
};

class ExpressionDayOfMonth final : public DateExpressionAcceptingTimeZone<ExpressionDayOfMonth> {
public:
    explicit ExpressionDayOfMonth(ExpressionContext* const expCtx,
                                  boost::intrusive_ptr<Expression> date,
                                  boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionDayOfMonth>(
              expCtx, "$dayOfMonth", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).dayOfMonth);
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionDayOfWeek final : public DateExpressionAcceptingTimeZone<ExpressionDayOfWeek> {
public:
    explicit ExpressionDayOfWeek(ExpressionContext* const expCtx,
                                 boost::intrusive_ptr<Expression> date,
                                 boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionDayOfWeek>(
              expCtx, "$dayOfWeek", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dayOfWeek(date));
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionDayOfYear final : public DateExpressionAcceptingTimeZone<ExpressionDayOfYear> {
public:
    explicit ExpressionDayOfYear(ExpressionContext* const expCtx,
                                 boost::intrusive_ptr<Expression> date,
                                 boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionDayOfYear>(
              expCtx, "$dayOfYear", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dayOfYear(date));
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

/**
 * $dateDiff expression that determines a difference between two time instants.
 */
class ExpressionDateDiff final : public Expression {
public:
    /**
     * startDate - an expression that resolves to a Value that is coercible to date.
     * endDate - an expression that resolves to a Value that is coercible to date.
     * unit - expression defining a length of time interval to measure the difference in that
     * resolves to a string Value.
     * timezone - expression defining a timezone to perform the operation in that resolves to a
     * string Value. Can be nullptr.
     * startOfWeek - expression defining the week start day that resolves to a string Value. Can be
     * nullptr.
     */
    ExpressionDateDiff(ExpressionContext* expCtx,
                       boost::intrusive_ptr<Expression> startDate,
                       boost::intrusive_ptr<Expression> endDate,
                       boost::intrusive_ptr<Expression> unit,
                       boost::intrusive_ptr<Expression> timezone,
                       boost::intrusive_ptr<Expression> startOfWeek);
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root, Variables* variables) const final;
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);
    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    /**
     * Returns true if this expression has parameter 'timezone' specified, otherwise false.
     */
    bool isTimezoneSpecified() const {
        return static_cast<bool>(_timeZone);
    }

    /**
     * Returns true if this expression has parameter 'startOfWeek' specified, otherwise false.
     */
    bool isStartOfWeekSpecified() const {
        return static_cast<bool>(_startOfWeek);
    }

private:
    /**
     * Converts 'value' to Date_t type for $dateDiff expression for parameter 'parameterName'.
     */
    static Date_t convertToDate(const Value& value, StringData parameterName);

    void _doAddDependencies(DepsTracker* deps) const final;

    // Starting time instant expression. Accepted types: Date_t, Timestamp, OID.
    boost::intrusive_ptr<Expression>& _startDate;

    // Ending time instant expression. Accepted types the same as for '_startDate'.
    boost::intrusive_ptr<Expression>& _endDate;

    // Length of time interval to measure the difference. Accepted type: std::string. Accepted
    // values: enumerators from TimeUnit enumeration.
    boost::intrusive_ptr<Expression>& _unit;

    // Timezone to use for the difference calculation. Accepted type: std::string. If not specified,
    // UTC is used.
    boost::intrusive_ptr<Expression>& _timeZone;

    // First/start day of the week to use for the date difference calculation when time unit is the
    // week. Accepted type: std::string. If not specified, "sunday" is used.
    boost::intrusive_ptr<Expression>& _startOfWeek;
};

class ExpressionDivide final : public ExpressionFixedArity<ExpressionDivide, 2> {
public:
    /**
     * Divides two values as if by {$divide: [{$const: numerator}, {$const: denominator]}.
     *
     * Returns BSONNULL if either argument is nullish.
     *
     * Returns ErrorCodes::TypeMismatch if either argument is non-nullish and non-numeric.
     * Returns ErrorCodes::BadValue if the denominator is zero.
     */
    static StatusWith<Value> apply(Value numerator, Value denominator);

    explicit ExpressionDivide(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionDivide, 2>(expCtx) {}
    explicit ExpressionDivide(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionDivide, 2>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionExp final : public ExpressionSingleNumericArg<ExpressionExp> {
public:
    explicit ExpressionExp(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg<ExpressionExp>(expCtx) {}
    explicit ExpressionExp(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionSingleNumericArg<ExpressionExp>(expCtx, std::move(children)) {}

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionFieldPath : public Expression {
public:
    /**
     * Checks whether this field path is exactly "$$ROOT".
     */
    bool isROOT() const {
        return _variable == Variables::kRootId && _fieldPath.getPathLength() == 1;
    }

    /**
     * Checks whether this field path starts with a variable besides ROOT.
     *
     * For example, these are variable references:
     *   "$$NOW"
     *   "$$NOW.x"
     * and these are not:
     *   "$x"
     *   "$$ROOT"
     *   "$$ROOT.x"
     */
    bool isVariableReference() const {
        return _variable != Variables::kRootId;
    }

    boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root, Variables* variables) const;
    Value serialize(bool explain) const final;

    /*
      Create a field path expression using old semantics (rooted off of CURRENT).

      // NOTE: this method is deprecated and only used by tests
      // TODO remove this method in favor of parse()

      Evaluation will extract the value associated with the given field
      path from the source document.

      @param fieldPath the field path string, without any leading document
        indicator
      @returns the newly created field path expression
     */
    static boost::intrusive_ptr<ExpressionFieldPath> deprecatedCreate(ExpressionContext* expCtx,
                                                                      const std::string& fieldPath);

    // Parse from the raw std::string from the user with the "$" prefixes.
    static boost::intrusive_ptr<ExpressionFieldPath> parse(ExpressionContext* expCtx,
                                                           const std::string& raw,
                                                           const VariablesParseState& vps);
    // Create from a non-prefixed string. Assumes path not variable.
    static boost::intrusive_ptr<ExpressionFieldPath> createPathFromString(
        ExpressionContext* expCtx, const std::string& raw, const VariablesParseState& vps);
    // Create from a non-prefixed string. Assumes variable not path.
    static boost::intrusive_ptr<ExpressionFieldPath> createVarFromString(
        ExpressionContext* expCtx, const std::string& raw, const VariablesParseState& vps);

    /**
     * Returns true if this expression logically represents the path 'dottedPath'. For example, if
     * 'dottedPath' is 'a.b' and this FieldPath is '$$CURRENT.a.b', returns true.
     */
    bool representsPath(const std::string& dottedPath) const;

    const FieldPath& getFieldPath() const {
        return _fieldPath;
    }

    Variables::Id getVariableId() const {
        return _variable;
    }

    auto getFieldPathWithoutCurrentPrefix() const {
        return _fieldPath.tail();
    }

    ComputedPaths getComputedPaths(const std::string& exprFieldPath,
                                   Variables::Id renamingVar) const final;

    /**
     * Finds an applicable rename from 'renameList' and creates a copy of ExpressionFieldPath in
     * which the the rename is substituted. If there is no applicable rename, returns nullptr. Each
     * pair in 'renameList' specifies a path prefix that should be renamed (as the first element)
     * and the path components that should replace the renamed prefix (as the second element).
     */
    std::unique_ptr<Expression> copyWithSubstitution(
        const StringMap<std::string>& renameList) const;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;
    ExpressionFieldPath(ExpressionContext* expCtx,
                        const std::string& fieldPath,
                        Variables::Id variable);


private:
    /*
      Internal implementation of evaluate(), used recursively.

      The internal implementation doesn't just use a loop because of
      the possibility that we need to skip over an array.  If the path
      is "a.b.c", and a is an array, then we fan out from there, and
      traverse "b.c" for each element of a:[...].  This requires that
      a be an array of objects in order to navigate more deeply.

      @param index current path field index to extract
      @param input current document traversed to (not the top-level one)
      @returns the field found; could be an array
     */
    Value evaluatePath(size_t index, const Document& input) const;

    // Helper for evaluatePath to handle Array case
    Value evaluatePathArray(size_t index, const Value& input) const;

    const FieldPath _fieldPath;
    const Variables::Id _variable;
};

class ExpressionFilter final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root, Variables* variables) const final;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    ExpressionFilter(ExpressionContext* expCtx,
                     std::string varName,
                     Variables::Id varId,
                     boost::intrusive_ptr<Expression> input,
                     boost::intrusive_ptr<Expression> cond,
                     boost::intrusive_ptr<Expression> limit = nullptr);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    Variables::Id getVariableId() const {
        return _varId;
    }

    bool hasLimit() const {
        return this->_limit ? true : false;
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    // The name of the variable to set to each element in the array.
    std::string _varName;
    // The id of the variable to set.
    Variables::Id _varId;
    // The array to iterate over.
    boost::intrusive_ptr<Expression>& _input;
    // The expression determining whether each element should be present in the result array.
    boost::intrusive_ptr<Expression>& _cond;
    // The optional expression determining how many elements should be present in the result array.
    boost::optional<boost::intrusive_ptr<Expression>&> _limit;
};


class ExpressionFloor final : public ExpressionSingleNumericArg<ExpressionFloor> {
public:
    explicit ExpressionFloor(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg<ExpressionFloor>(expCtx) {}
    explicit ExpressionFloor(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionSingleNumericArg<ExpressionFloor>(expCtx, std::move(children)) {}

    static StatusWith<Value> apply(Value lhs);

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionHour final : public DateExpressionAcceptingTimeZone<ExpressionHour> {
public:
    explicit ExpressionHour(ExpressionContext* const expCtx,
                            boost::intrusive_ptr<Expression> date,
                            boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionHour>(
              expCtx, "$hour", std::move(date), std::move(timeZone)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).hour);
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionIfNull final : public ExpressionVariadic<ExpressionIfNull> {
public:
    explicit ExpressionIfNull(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionIfNull>(expCtx) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;
    void validateArguments(const ExpressionVector& args) const final;
    boost::intrusive_ptr<Expression> optimize() final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionIn final : public ExpressionFixedArity<ExpressionIn, 2> {
public:
    explicit ExpressionIn(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionIn, 2>(expCtx) {
        expCtx->sbeCompatible = false;
    }

    ExpressionIn(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionIn, 2>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionIndexOfArray : public ExpressionRangedArity<ExpressionIndexOfArray, 2, 4> {
public:
    explicit ExpressionIndexOfArray(ExpressionContext* const expCtx)
        : ExpressionRangedArity<ExpressionIndexOfArray, 2, 4>(expCtx) {
        expCtx->sbeCompatible = false;
    }

    ExpressionIndexOfArray(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionRangedArity<ExpressionIndexOfArray, 2, 4>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const;
    boost::intrusive_ptr<Expression> optimize() final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    struct Arguments {
        Arguments(Value targetOfSearch, int startIndex, int endIndex)
            : targetOfSearch(targetOfSearch), startIndex(startIndex), endIndex(endIndex) {}

        Value targetOfSearch;
        int startIndex;
        int endIndex;
    };
    /**
     * When given 'operands' which correspond to the arguments to $indexOfArray, evaluates and
     * validates the target value, starting index, and ending index arguments and returns their
     * values as a Arguments struct. The starting index and ending index are optional, so as default
     * 'startIndex' will be 0 and 'endIndex' will be the length of the input array. Throws a
     * UserException if the values are found to be invalid in some way, e.g. if the indexes are not
     * numbers.
     */
    Arguments evaluateAndValidateArguments(const Document& root,
                                           const ExpressionVector& operands,
                                           size_t arrayLength,
                                           Variables* variables) const;

private:
    class Optimized;
};


class ExpressionIndexOfBytes final : public ExpressionRangedArity<ExpressionIndexOfBytes, 2, 4> {
public:
    explicit ExpressionIndexOfBytes(ExpressionContext* const expCtx)
        : ExpressionRangedArity<ExpressionIndexOfBytes, 2, 4>(expCtx) {}
    ExpressionIndexOfBytes(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionRangedArity<ExpressionIndexOfBytes, 2, 4>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


/**
 * Implements indexOf behavior for strings with UTF-8 encoding.
 */
class ExpressionIndexOfCP final : public ExpressionRangedArity<ExpressionIndexOfCP, 2, 4> {
public:
    explicit ExpressionIndexOfCP(ExpressionContext* const expCtx)
        : ExpressionRangedArity<ExpressionIndexOfCP, 2, 4>(expCtx) {}
    ExpressionIndexOfCP(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionRangedArity<ExpressionIndexOfCP, 2, 4>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionLet final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root, Variables* variables) const final;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    struct NameAndExpression {
        std::string name;
        boost::intrusive_ptr<Expression>& expression;
    };

    typedef std::map<Variables::Id, NameAndExpression> VariableMap;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    auto& getOrderedVariableIds() const {
        return _orderedVariableIds;
    }

    auto& getVariableMap() const {
        return _variables;
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    ExpressionLet(ExpressionContext* expCtx,
                  VariableMap&& vars,
                  std::vector<boost::intrusive_ptr<Expression>> children,
                  std::vector<Variables::Id> orderedVariableIds);

    VariableMap _variables;

    // These ids are ordered to match their corresponding _children expressions.
    std::vector<Variables::Id> _orderedVariableIds;

    // Reference to the last element in the '_children' list.
    boost::intrusive_ptr<Expression>& _subExpression;
};

class ExpressionLn final : public ExpressionSingleNumericArg<ExpressionLn> {
public:
    explicit ExpressionLn(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg<ExpressionLn>(expCtx) {}
    ExpressionLn(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionSingleNumericArg<ExpressionLn>(expCtx, std::move(children)) {}

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionLog final : public ExpressionFixedArity<ExpressionLog, 2> {
public:
    explicit ExpressionLog(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionLog, 2>(expCtx) {
        expCtx->sbeCompatible = false;
    }
    ExpressionLog(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionLog, 2>(expCtx, std::move(children)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionLog10 final : public ExpressionSingleNumericArg<ExpressionLog10> {
public:
    explicit ExpressionLog10(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg<ExpressionLog10>(expCtx) {}
    ExpressionLog10(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionSingleNumericArg<ExpressionLog10>(expCtx, std::move(children)) {}

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionInternalFLEEqual final : public Expression {
public:
    ExpressionInternalFLEEqual(ExpressionContext* expCtx,
                               boost::intrusive_ptr<Expression> field,
                               ConstDataRange serverToken,
                               int64_t contentionFactor,
                               ConstDataRange edcToken);
    Value serialize(bool explain) const final;

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);
    void _doAddDependencies(DepsTracker* deps) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    std::array<std::uint8_t, 32> _serverToken;
    std::array<std::uint8_t, 32> _edcToken;
    int64_t _contentionFactor;
    stdx::unordered_set<std::array<std::uint8_t, 32>> _cachedEDCTokens;
};

class ExpressionMap final : public Expression {
public:
    ExpressionMap(
        ExpressionContext* expCtx,
        const std::string& varName,              // name of variable to set
        Variables::Id varId,                     // id of variable to set
        boost::intrusive_ptr<Expression> input,  // yields array to iterate
        boost::intrusive_ptr<Expression> each);  // yields results to be added to output array

    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root, Variables* variables) const final;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    ComputedPaths getComputedPaths(const std::string& exprFieldPath,
                                   Variables::Id renamingVar) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    std::string _varName;
    Variables::Id _varId;
    boost::intrusive_ptr<Expression>& _input;
    boost::intrusive_ptr<Expression>& _each;
};

class ExpressionMeta final : public Expression {
public:
    ExpressionMeta(ExpressionContext* expCtx, DocumentMetadataFields::MetaType metaType);

    Value serialize(bool explain) const final;
    Value evaluate(const Document& root, Variables* variables) const final;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    DocumentMetadataFields::MetaType getMetaType() const {
        return _metaType;
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    DocumentMetadataFields::MetaType _metaType;
};

class ExpressionMillisecond final : public DateExpressionAcceptingTimeZone<ExpressionMillisecond> {
public:
    explicit ExpressionMillisecond(ExpressionContext* const expCtx,
                                   boost::intrusive_ptr<Expression> date,
                                   boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionMillisecond>(
              expCtx, "$millisecond", std::move(date), std::move(timeZone)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).millisecond);
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionMinute final : public DateExpressionAcceptingTimeZone<ExpressionMinute> {
public:
    explicit ExpressionMinute(ExpressionContext* const expCtx,
                              boost::intrusive_ptr<Expression> date,
                              boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionMinute>(
              expCtx, "$minute", std::move(date), std::move(timeZone)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).minute);
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionMod final : public ExpressionFixedArity<ExpressionMod, 2> {
public:
    explicit ExpressionMod(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionMod, 2>(expCtx) {}
    ExpressionMod(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionMod, 2>(expCtx, std::move(children)) {}

    static StatusWith<Value> apply(Value lhs, Value rhs);

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionMultiply final : public ExpressionVariadic<ExpressionMultiply> {
public:
    /**
     * Multiplies two values together as if by evaluate() on
     *     {$multiply: [{$const: lhs}, {$const: rhs}]}.
     *
     * Note that evaluate() does not use apply() directly, because when $multiply takes more than
     * two arguments, it uses a wider intermediate state than Value.
     *
     * Returns BSONNULL if either argument is nullish.
     *
     * Returns ErrorCodes::TypeMismatch if any argument is non-nullish, non-numeric.
     */
    static StatusWith<Value> apply(Value lhs, Value rhs);

    explicit ExpressionMultiply(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionMultiply>(expCtx) {}
    ExpressionMultiply(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionMultiply>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionMonth final : public DateExpressionAcceptingTimeZone<ExpressionMonth> {
public:
    explicit ExpressionMonth(ExpressionContext* const expCtx,
                             boost::intrusive_ptr<Expression> date,
                             boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionMonth>(
              expCtx, "$month", std::move(date), std::move(timeZone)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).month);
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionNot final : public ExpressionFixedArity<ExpressionNot, 1> {
public:
    explicit ExpressionNot(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionNot, 1>(expCtx) {}

    ExpressionNot(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionNot, 1>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


/**
 * This class is used to represent expressions that create object literals, such as the value of
 * '_id' in this group stage:
 *   {$group: {
 *     _id: {b: "$a", c: {$add: [4, "$c"]}}  <- This is represented as an ExpressionObject.
 *     ...
 *   }}
 */
class ExpressionObject final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root, Variables* variables) const final;
    Value serialize(bool explain) const final;

    static boost::intrusive_ptr<ExpressionObject> create(
        ExpressionContext* expCtx,
        std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>>&&
            expressionsWithChildrenInPlace);

    /**
     * Parses and constructs an ExpressionObject from 'obj'.
     */
    static boost::intrusive_ptr<ExpressionObject> parse(ExpressionContext* expCtx,
                                                        BSONObj obj,
                                                        const VariablesParseState& vps);

    /**
     * This ExpressionObject must outlive the returned vector.
     */
    const auto& getChildExpressions() const {
        return _expressions;
    }

    ComputedPaths getComputedPaths(const std::string& exprFieldPath,
                                   Variables::Id renamingVar) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    ExpressionObject(
        ExpressionContext* expCtx,
        std::vector<boost::intrusive_ptr<Expression>> children,
        std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>&>>&& expressions);

    // The mapping from field name to expression within this object. This needs to respect the order
    // in which the fields were specified in the input BSON.
    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>&>> _expressions;
};


class ExpressionOr final : public ExpressionVariadic<ExpressionOr> {
public:
    explicit ExpressionOr(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionOr>(expCtx) {}

    ExpressionOr(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionOr>(expCtx, std::move(children)) {}

    boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionPow final : public ExpressionFixedArity<ExpressionPow, 2> {
public:
    explicit ExpressionPow(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionPow, 2>(expCtx) {
        expCtx->sbeCompatible = false;
    }
    ExpressionPow(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionPow, 2>(expCtx, std::move(children)) {}

    static boost::intrusive_ptr<Expression> create(ExpressionContext* expCtx,
                                                   Value base,
                                                   Value exp);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;
};


class ExpressionRange final : public ExpressionRangedArity<ExpressionRange, 2, 3> {
public:
    explicit ExpressionRange(ExpressionContext* const expCtx)
        : ExpressionRangedArity<ExpressionRange, 2, 3>(expCtx) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionReduce final : public Expression {
public:
    ExpressionReduce(ExpressionContext* const expCtx,
                     boost::intrusive_ptr<Expression> input,
                     boost::intrusive_ptr<Expression> initial,
                     boost::intrusive_ptr<Expression> in,
                     Variables::Id thisVar,
                     Variables::Id valueVar)
        : Expression(expCtx, {std::move(input), std::move(initial), std::move(in)}),
          _input(_children[0]),
          _initial(_children[1]),
          _in(_children[2]),
          _thisVar(thisVar),
          _valueVar(valueVar) {
        expCtx->sbeCompatible = false;
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);
    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    boost::intrusive_ptr<Expression>& _input;
    boost::intrusive_ptr<Expression>& _initial;
    boost::intrusive_ptr<Expression>& _in;

    Variables::Id _thisVar;
    Variables::Id _valueVar;
};


class ExpressionReplaceBase : public Expression {
public:
    ExpressionReplaceBase(ExpressionContext* const expCtx,
                          boost::intrusive_ptr<Expression> input,
                          boost::intrusive_ptr<Expression> find,
                          boost::intrusive_ptr<Expression> replacement)
        : Expression(expCtx, {std::move(input), std::move(find), std::move(replacement)}),
          _input(_children[0]),
          _find(_children[1]),
          _replacement(_children[2]) {}

    virtual const char* getOpName() const = 0;
    Value evaluate(const Document& root, Variables* variables) const final;
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;

protected:
    void _doAddDependencies(DepsTracker* deps) const final;
    virtual Value _doEval(StringData input, StringData find, StringData replacement) const = 0;

    // These are owned by this->Expression::_children. They are references to intrusive_ptr instead
    // of direct references to Expression because we need to be able to replace each child in
    // optimize() without invalidating the references.
    boost::intrusive_ptr<Expression>& _input;
    boost::intrusive_ptr<Expression>& _find;
    boost::intrusive_ptr<Expression>& _replacement;
};


class ExpressionReplaceOne final : public ExpressionReplaceBase {
public:
    using ExpressionReplaceBase::ExpressionReplaceBase;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    static constexpr const char* const opName = "$replaceOne";
    const char* getOpName() const final {
        return opName;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    Value _doEval(StringData input, StringData find, StringData replacement) const final;
};

class ExpressionReplaceAll final : public ExpressionReplaceBase {
public:
    ExpressionReplaceAll(ExpressionContext* const expCtx,
                         boost::intrusive_ptr<Expression> input,
                         boost::intrusive_ptr<Expression> find,
                         boost::intrusive_ptr<Expression> replacement)
        : ExpressionReplaceBase(expCtx, input, find, replacement) {
        expCtx->sbeCompatible = false;
    }

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    static constexpr const char* const opName = "$replaceAll";
    const char* getOpName() const final {
        return opName;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    Value _doEval(StringData input, StringData find, StringData replacement) const final;
};

class ExpressionSecond final : public DateExpressionAcceptingTimeZone<ExpressionSecond> {
public:
    ExpressionSecond(ExpressionContext* const expCtx,
                     boost::intrusive_ptr<Expression> date,
                     boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionSecond>(
              expCtx, "$second", std::move(date), std::move(timeZone)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).second);
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionSetDifference final : public ExpressionFixedArity<ExpressionSetDifference, 2> {
public:
    explicit ExpressionSetDifference(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionSetDifference, 2>(expCtx) {}
    ExpressionSetDifference(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionSetDifference, 2>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionSetEquals final : public ExpressionVariadic<ExpressionSetEquals> {
public:
    explicit ExpressionSetEquals(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionSetEquals>(expCtx) {}
    ExpressionSetEquals(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionSetEquals>(expCtx, std::move(children)) {}

    boost::intrusive_ptr<Expression> optimize() override;
    Value evaluate(const Document& root, Variables* variables) const override;
    const char* getOpName() const final;
    void validateArguments(const ExpressionVector& args) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    // The first element in the pair represent the position on the constant in the '_children'
    // array. The second element is the constant set.
    boost::optional<std::pair<size_t, ValueUnorderedSet>> _cachedConstant;
};


class ExpressionSetIntersection final : public ExpressionVariadic<ExpressionSetIntersection> {
public:
    explicit ExpressionSetIntersection(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionSetIntersection>(expCtx) {}
    ExpressionSetIntersection(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionSetIntersection>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


// Not final, inherited from for optimizations.
class ExpressionSetIsSubset : public ExpressionFixedArity<ExpressionSetIsSubset, 2> {
public:
    explicit ExpressionSetIsSubset(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionSetIsSubset, 2>(expCtx) {
        expCtx->sbeCompatible = false;
    }
    ExpressionSetIsSubset(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionSetIsSubset, 2>(expCtx, std::move(children)) {}

    boost::intrusive_ptr<Expression> optimize() override;
    Value evaluate(const Document& root, Variables* variables) const override;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    class Optimized;
};


class ExpressionSetUnion final : public ExpressionVariadic<ExpressionSetUnion> {
public:
    explicit ExpressionSetUnion(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionSetUnion>(expCtx) {}
    ExpressionSetUnion(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionSetUnion>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        // Only commutative when performing binary string comparison. The first value entered when
        // multiple collation-equal but binary-unequal values are added will dictate what is stored
        // in the set.
        return getExpressionContext()->getCollator() == nullptr;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionSize final : public ExpressionFixedArity<ExpressionSize, 1> {
public:
    explicit ExpressionSize(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionSize, 1>(expCtx) {
        expCtx->sbeCompatible = false;
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionReverseArray final : public ExpressionFixedArity<ExpressionReverseArray, 1> {
public:
    explicit ExpressionReverseArray(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionReverseArray, 1>(expCtx) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionSortArray final : public Expression {
public:
    static constexpr auto kName = "$sortArray"_sd;
    ExpressionSortArray(ExpressionContext* const expCtx,
                        boost::intrusive_ptr<Expression> input,
                        const PatternValueCmp& sortBy)
        : Expression(expCtx, {std::move(input)}), _input(_children[0]), _sortBy(sortBy) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);
    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const char* getOpName() const;

    BSONObj getSortPattern() const {
        return _sortBy.sortPattern;
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    boost::intrusive_ptr<Expression>& _input;
    PatternValueCmp _sortBy;
};

class ExpressionSlice final : public ExpressionRangedArity<ExpressionSlice, 2, 3> {
public:
    explicit ExpressionSlice(ExpressionContext* const expCtx)
        : ExpressionRangedArity<ExpressionSlice, 2, 3>(expCtx) {
        expCtx->sbeCompatible = false;
    }
    ExpressionSlice(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionRangedArity<ExpressionSlice, 2, 3>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionIsArray final : public ExpressionFixedArity<ExpressionIsArray, 1> {
public:
    explicit ExpressionIsArray(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionIsArray, 1>(expCtx) {}

    ExpressionIsArray(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionIsArray, 1>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

/**
 * Expression used for distinct only. This expression unwinds all singly nested arrays along the
 * specified path, but does not descend into doubly nested arrays. The resulting array of values
 * is placed into a specially named field that is consumed by distinct.
 *
 * Aggregation's distinct behavior must match Find's, so numeric path components can be treated
 * as both array indexes and field names.
 */
class ExpressionInternalFindAllValuesAtPath final
    : public ExpressionFixedArity<ExpressionInternalFindAllValuesAtPath, 1> {
public:
    explicit ExpressionInternalFindAllValuesAtPath(ExpressionContext* expCtx)
        : ExpressionFixedArity<ExpressionInternalFindAllValuesAtPath, 1>(expCtx) {}

    explicit ExpressionInternalFindAllValuesAtPath(ExpressionContext* expCtx,
                                                   ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionInternalFindAllValuesAtPath, 1>(expCtx,
                                                                         std::move(children)) {}
    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const {
        return "$_internalFindAllValuesAtPath";
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    /**
     * The base class' optimize will think this expression is const because the argument to it must
     * be const. However, the results still change based on the document. Therefore skip optimizing.
     */
    boost::intrusive_ptr<Expression> optimize() override {
        return this;
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const override {
        auto fp = getFieldPath();
        // We require everything below the first field.
        deps->fields.insert(std::string(fp.getSubpath(0)));
    }

private:
    FieldPath getFieldPath() const {
        auto inputConstExpression = dynamic_cast<ExpressionConstant*>(_children[0].get());
        uassert(5511201,
                "Expected const expression as argument to _internalUnwindAllAlongPath",
                inputConstExpression);
        auto constVal = inputConstExpression->getValue();
        // getString asserts if type != string, which is the correct behavior for what we want.
        return FieldPath(constVal.getString());
    }
};

class ExpressionRound final : public ExpressionRangedArity<ExpressionRound, 1, 2> {
public:
    explicit ExpressionRound(ExpressionContext* const expCtx)
        : ExpressionRangedArity<ExpressionRound, 1, 2>(expCtx) {
        expCtx->sbeCompatible = false;
    }
    ExpressionRound(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionRangedArity<ExpressionRound, 1, 2>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionSplit final : public ExpressionFixedArity<ExpressionSplit, 2> {
public:
    explicit ExpressionSplit(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionSplit, 2>(expCtx) {
        expCtx->sbeCompatible = false;
    }
    ExpressionSplit(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionSplit, 2>(expCtx, std::move(children)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionSqrt final : public ExpressionSingleNumericArg<ExpressionSqrt> {
public:
    explicit ExpressionSqrt(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg<ExpressionSqrt>(expCtx) {}
    ExpressionSqrt(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionSingleNumericArg<ExpressionSqrt>(expCtx, std::move(children)) {}

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionStrcasecmp final : public ExpressionFixedArity<ExpressionStrcasecmp, 2> {
public:
    explicit ExpressionStrcasecmp(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionStrcasecmp, 2>(expCtx) {
        expCtx->sbeCompatible = false;
    }
    ExpressionStrcasecmp(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionStrcasecmp, 2>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionSubstrBytes final : public ExpressionFixedArity<ExpressionSubstrBytes, 3> {
public:
    explicit ExpressionSubstrBytes(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionSubstrBytes, 3>(expCtx) {
        expCtx->sbeCompatible = false;
    }
    ExpressionSubstrBytes(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionSubstrBytes, 3>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionSubstrCP final : public ExpressionFixedArity<ExpressionSubstrCP, 3> {
public:
    explicit ExpressionSubstrCP(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionSubstrCP, 3>(expCtx) {
        expCtx->sbeCompatible = false;
    }
    ExpressionSubstrCP(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionSubstrCP, 3>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionStrLenBytes final : public ExpressionFixedArity<ExpressionStrLenBytes, 1> {
public:
    explicit ExpressionStrLenBytes(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionStrLenBytes, 1>(expCtx) {
        expCtx->sbeCompatible = false;
    }

    ExpressionStrLenBytes(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionStrLenBytes, 1>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionBinarySize final : public ExpressionFixedArity<ExpressionBinarySize, 1> {
public:
    ExpressionBinarySize(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionBinarySize, 1>(expCtx) {
        expCtx->sbeCompatible = false;
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionStrLenCP final : public ExpressionFixedArity<ExpressionStrLenCP, 1> {
public:
    explicit ExpressionStrLenCP(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionStrLenCP, 1>(expCtx) {
        expCtx->sbeCompatible = false;
    }
    ExpressionStrLenCP(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionStrLenCP, 1>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionSubtract final : public ExpressionFixedArity<ExpressionSubtract, 2> {
public:
    /**
     * Subtracts two values as if by {$subtract: [{$const: lhs}, {$const: rhs}]}.
     *
     * If either argument is nullish, returns BSONNULL.
     *
     * Otherwise, the arguments can be either:
     *     (numeric, numeric)
     *     (Date, Date)       Returns the time difference in milliseconds.
     *     (Date, numeric)    Returns the date shifted earlier by that many milliseconds.
     *
     * Otherwise, returns ErrorCodes::TypeMismatch.
     */
    static StatusWith<Value> apply(Value lhs, Value rhs);

    explicit ExpressionSubtract(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionSubtract, 2>(expCtx) {
        expCtx->sbeCompatible = false;
    }
    ExpressionSubtract(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionSubtract, 2>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionSwitch final : public Expression {
public:
    using ExpressionPair =
        std::pair<boost::intrusive_ptr<Expression>&, boost::intrusive_ptr<Expression>&>;

    ExpressionSwitch(ExpressionContext* const expCtx,
                     std::vector<boost::intrusive_ptr<Expression>> children,
                     std::vector<ExpressionPair> branches)
        : Expression(expCtx, std::move(children)),
          _default(_children.back()),
          _branches(std::move(branches)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);
    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    boost::intrusive_ptr<Expression>& _default;
    std::vector<ExpressionPair> _branches;
};


class ExpressionToLower final : public ExpressionFixedArity<ExpressionToLower, 1> {
public:
    explicit ExpressionToLower(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionToLower, 1>(expCtx) {}

    ExpressionToLower(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionToLower, 1>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionToUpper final : public ExpressionFixedArity<ExpressionToUpper, 1> {
public:
    explicit ExpressionToUpper(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionToUpper, 1>(expCtx) {}

    ExpressionToUpper(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionToUpper, 1>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


/**
 * This class is used to implement all three trim expressions: $trim, $ltrim, and $rtrim.
 */
class ExpressionTrim final : public Expression {
public:
    enum class TrimType {
        kBoth,
        kLeft,
        kRight,
    };
    ExpressionTrim(ExpressionContext* const expCtx,
                   TrimType trimType,
                   StringData name,
                   boost::intrusive_ptr<Expression> input,
                   boost::intrusive_ptr<Expression> charactersToTrim)
        : Expression(expCtx, {std::move(input), std::move(charactersToTrim)}),
          _trimType(trimType),
          _name(name.toString()),
          _input(_children[0]),
          _characters(_children[1]) {
        expCtx->sbeCompatible = false;
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);
    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    /**
     * Returns true if the unicode character found at index 'indexIntoInput' of 'input' is equal to
     * 'testCP'.
     */
    static bool codePointMatchesAtIndex(const StringData& input,
                                        std::size_t indexIntoInput,
                                        const StringData& testCP);

    /**
     * Given the input string and the code points to trim from that string, returns a substring of
     * 'input' with any code point from 'trimCPs' trimmed from the left.
     */
    static StringData trimFromLeft(StringData input, const std::vector<StringData>& trimCPs);

    /**
     * Given the input string and the code points to trim from that string, returns a substring of
     * 'input' with any code point from 'trimCPs' trimmed from the right.
     */
    static StringData trimFromRight(StringData input, const std::vector<StringData>& trimCPs);

    /**
     * Returns the trimmed version of 'input', with all code points in 'trimCPs' removed from the
     * front, back, or both - depending on _trimType.
     */
    StringData doTrim(StringData input, const std::vector<StringData>& trimCPs) const;

    TrimType _trimType;
    std::string _name;  // "$trim", "$ltrim", or "$rtrim".
    boost::intrusive_ptr<Expression>& _input;
    boost::intrusive_ptr<Expression>& _characters;  // Optional, null if not specified.
};


class ExpressionTrunc final : public ExpressionRangedArity<ExpressionTrunc, 1, 2> {
public:
    explicit ExpressionTrunc(ExpressionContext* const expCtx)
        : ExpressionRangedArity<ExpressionTrunc, 1, 2>(expCtx) {
        expCtx->sbeCompatible = false;
    }
    ExpressionTrunc(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionRangedArity<ExpressionTrunc, 1, 2>(expCtx, std::move(children)) {}

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement elem,
                                                  const VariablesParseState& vps);
    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionType final : public ExpressionFixedArity<ExpressionType, 1> {
public:
    explicit ExpressionType(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionType, 1>(expCtx) {
        expCtx->sbeCompatible = false;
    }

    ExpressionType(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionType, 1>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionIsNumber final : public ExpressionFixedArity<ExpressionIsNumber, 1> {
public:
    explicit ExpressionIsNumber(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionIsNumber, 1>(expCtx) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionWeek final : public DateExpressionAcceptingTimeZone<ExpressionWeek> {
public:
    ExpressionWeek(ExpressionContext* const expCtx,
                   boost::intrusive_ptr<Expression> date,
                   boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionWeek>(
              expCtx, "$week", std::move(date), std::move(timeZone)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.week(date));
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionIsoWeekYear final : public DateExpressionAcceptingTimeZone<ExpressionIsoWeekYear> {
public:
    ExpressionIsoWeekYear(ExpressionContext* const expCtx,
                          boost::intrusive_ptr<Expression> date,
                          boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionIsoWeekYear>(
              expCtx, "$isoWeekYear", std::move(date), std::move(timeZone)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.isoYear(date));
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionIsoDayOfWeek final
    : public DateExpressionAcceptingTimeZone<ExpressionIsoDayOfWeek> {
public:
    ExpressionIsoDayOfWeek(ExpressionContext* const expCtx,
                           boost::intrusive_ptr<Expression> date,
                           boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionIsoDayOfWeek>(
              expCtx, "$isoDayOfWeek", std::move(date), std::move(timeZone)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.isoDayOfWeek(date));
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionIsoWeek final : public DateExpressionAcceptingTimeZone<ExpressionIsoWeek> {
public:
    ExpressionIsoWeek(ExpressionContext* const expCtx,
                      boost::intrusive_ptr<Expression> date,
                      boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionIsoWeek>(
              expCtx, "$isoWeek", std::move(date), std::move(timeZone)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.isoWeek(date));
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionYear final : public DateExpressionAcceptingTimeZone<ExpressionYear> {
public:
    ExpressionYear(ExpressionContext* const expCtx,
                   boost::intrusive_ptr<Expression> date,
                   boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionYear>(
              expCtx, "$year", std::move(date), std::move(timeZone)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).year);
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionZip final : public Expression {
public:
    ExpressionZip(ExpressionContext* const expCtx,
                  bool useLongestLength,
                  std::vector<boost::intrusive_ptr<Expression>> children,
                  std::vector<std::reference_wrapper<boost::intrusive_ptr<Expression>>> inputs,
                  std::vector<std::reference_wrapper<boost::intrusive_ptr<Expression>>> defaults)
        : Expression(expCtx, std::move(children)),
          _useLongestLength(useLongestLength),
          _inputs(std::move(inputs)),
          _defaults(std::move(defaults)) {
        expCtx->sbeCompatible = false;
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);
    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    bool _useLongestLength;
    std::vector<std::reference_wrapper<boost::intrusive_ptr<Expression>>> _inputs;
    std::vector<std::reference_wrapper<boost::intrusive_ptr<Expression>>> _defaults;
};

class ExpressionConvert final : public Expression {
public:
    ExpressionConvert(ExpressionContext* expCtx,
                      boost::intrusive_ptr<Expression> input,
                      boost::intrusive_ptr<Expression> to,
                      boost::intrusive_ptr<Expression> onError,
                      boost::intrusive_ptr<Expression> onNull);
    /**
     * Creates a $convert expression converting from 'input' to the type given by 'toType'. Leaves
     * 'onNull' and 'onError' unspecified.
     */
    static boost::intrusive_ptr<Expression> create(ExpressionContext*,
                                                   boost::intrusive_ptr<Expression> input,
                                                   BSONType toType);

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);

    Value evaluate(const Document& root, Variables* variables) const final;
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    BSONType computeTargetType(Value typeName) const;
    Value performConversion(BSONType targetType, Value inputValue) const;

    boost::intrusive_ptr<Expression>& _input;
    boost::intrusive_ptr<Expression>& _to;
    boost::intrusive_ptr<Expression>& _onError;
    boost::intrusive_ptr<Expression>& _onNull;
};

class ExpressionRegex : public Expression {
public:
    /**
     * Object to hold data that is required when calling 'execute()' or 'nextMatch()'.
     */
    struct RegexExecutionState {
        /**
         * The regex pattern, options, and captures buffer for the current execution context.
         */
        boost::optional<std::string> pattern;
        boost::optional<std::string> options;
        std::vector<int> capturesBuffer;
        int numCaptures = 0;

        /**
         * If 'regex' is constant, 'pcrePtr' will be shared between the active RegexExecutionState
         * and '_initialExecStateForConstantRegex'. If not, then the active RegexExecutionState is
         * the sole owner.
         */
        std::shared_ptr<pcre::Regex> pcrePtr;

        /**
         * The input text and starting position for the current execution context.
         */
        boost::optional<std::string> input;
        int startCodePointPos = 0;
        int startBytePos = 0;

        /**
         * If either the text input or regex pattern is nullish, then we consider the operation as a
         * whole nullish.
         */
        bool nullish() {
            return !input || !pattern;
        }
    };

    /**
     * Validates the structure of input passed in 'inputExpr'. If valid, generates an initial
     * execution state. This returned object can later be used for calling execute() or nextMatch().
     */
    RegexExecutionState buildInitialState(const Document& root, Variables* variables) const;

    /**
     * Checks if there is a match for the input, options, and pattern of 'executionState'.
     * Returns the pcre::MatchData yielded by that match operation.
     * Will uassert for any errors other than `pcre::Errc::ERROR_NOMATCH`.
     */
    pcre::MatchData execute(RegexExecutionState* executionState) const;

    /**
     * Finds the next possible match for the given input and pattern that are part of
     * 'executionState'. If there is a match, the function will return a 'Value' object
     * encapsulating the matched string, the code point index of the matched string and a vector
     * representing all the captured substrings. The function will also update the parameters
     * 'startBytePos' and 'startCodePointPos' to the corresponding new indices. If there is no
     * match, the function will return null 'Value' object.
     */
    Value nextMatch(RegexExecutionState* executionState) const;

    /**
     * Optimizes '$regex*' expressions. If the expression has constant 'regex' and 'options' fields,
     * then it can be optimized. Stores the optimized regex in '_initialExecStateForConstantRegex'
     * so that it can be reused during expression evaluation.
     */
    boost::intrusive_ptr<Expression> optimize();

    bool hasConstantRegex() const {
        return _initialExecStateForConstantRegex.has_value();
    }

    bool hasOptions() const {
        return (_options.get() != nullptr);
    }

    /**
     * If pattern or options are not constants, returns boost::none. Otherwise, return value
     * contains regex pattern and options if they are not null.
     */
    boost::optional<std::pair<boost::optional<std::string>, std::string>>
    getConstantPatternAndOptions() const;

    Value serialize(bool explain) const;

    const std::string& getOpName() const {
        return _opName;
    }

    ExpressionRegex(ExpressionContext* const expCtx,
                    boost::intrusive_ptr<Expression> input,
                    boost::intrusive_ptr<Expression> regex,
                    boost::intrusive_ptr<Expression> options,
                    const StringData opName)
        : Expression(expCtx, {std::move(input), std::move(regex), std::move(options)}),
          _input(_children[0]),
          _regex(_children[1]),
          _options(_children[2]),
          _opName(opName) {}

private:
    void _extractInputField(RegexExecutionState* executionState, const Value& textInput) const;
    void _extractRegexAndOptions(RegexExecutionState* executionState,
                                 const Value& regexPattern,
                                 const Value& regexOptions) const;

    void _compile(RegexExecutionState* executionState) const;

    void _doAddDependencies(DepsTracker* deps) const final;

    /**
     * Expressions which, when evaluated for a given document, produce the the regex pattern, the
     * regex option flags, and the input text to which the regex should be applied.
     */
    boost::intrusive_ptr<Expression>& _input;
    boost::intrusive_ptr<Expression>& _regex;
    boost::intrusive_ptr<Expression>& _options;

    /**
     * This variable will be set when the $regex* expressions have constant values for their 'regex'
     * and 'options' fields, allowing us to pre-compile the regex and re-use it across the
     * Expression's lifetime.
     */
    boost::optional<RegexExecutionState> _initialExecStateForConstantRegex;

    /**
     * Name of the regex expression.
     */
    std::string _opName;
};

class ExpressionRegexFind final : public ExpressionRegex {
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);

    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    using ExpressionRegex::ExpressionRegex;
};

class ExpressionRegexFindAll final : public ExpressionRegex {
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);

    Value evaluate(const Document& root, Variables* variables) const final;
    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    using ExpressionRegex::ExpressionRegex;
};

class ExpressionRegexMatch final : public ExpressionRegex {
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);

    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    using ExpressionRegex::ExpressionRegex;
};

/**
 * Returns a double-valued random number from 0.0 to 1.0.
 */
class ExpressionRandom final : public Expression {

    static constexpr double kMinValue = 0.0;
    static constexpr double kMaxValue = 1.0;

public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement exprElement,
                                                  const VariablesParseState& vps);

    Value serialize(bool explain) const final;

    Value evaluate(const Document& root, Variables* variables) const final;

    boost::intrusive_ptr<Expression> optimize() final;

    const char* getOpName() const;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final override;

private:
    explicit ExpressionRandom(ExpressionContext* expCtx);

    double getRandomValue() const;
};

class ExpressionToHashedIndexKey : public Expression {
public:
    ExpressionToHashedIndexKey(ExpressionContext* const expCtx,
                               boost::intrusive_ptr<Expression> inputExpression)
        : Expression(expCtx, {inputExpression}) {
        expCtx->sbeCompatible = false;
    };

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    Value evaluate(const Document& root, Variables* variables) const;
    Value serialize(bool explain) const final;

protected:
    void _doAddDependencies(DepsTracker* deps) const final;
};

class ExpressionDateArithmetics : public Expression {
public:
    ExpressionDateArithmetics(ExpressionContext* const expCtx,
                              boost::intrusive_ptr<Expression> startDate,
                              boost::intrusive_ptr<Expression> unit,
                              boost::intrusive_ptr<Expression> amount,
                              boost::intrusive_ptr<Expression> timezone,
                              const StringData opName)
        : Expression(
              expCtx,
              {std::move(startDate), std::move(unit), std::move(amount), std::move(timezone)}),
          _startDate(_children[0]),
          _unit(_children[1]),
          _amount(_children[2]),
          _timeZone(_children[3]),
          _opName(opName) {}

    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root, Variables* variables) const final;

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

    /**
     * Subclasses should implement this to do their actual date arithmetics.
     */
    virtual Value evaluateDateArithmetics(Date_t date,
                                          TimeUnit unit,
                                          long long amount,
                                          const TimeZone& timezone) const = 0;

private:
    // The expression representing the startDate argument.
    boost::intrusive_ptr<Expression>& _startDate;

    // Unit of time: year, quarter, week, etc.
    boost::intrusive_ptr<Expression>& _unit;

    // Amount of units to be added or subtracted.
    boost::intrusive_ptr<Expression>& _amount;

    // The expression representing the timezone argument.
    boost::intrusive_ptr<Expression>& _timeZone;

    // The name of this expression, e.g. $dateAdd or $dateSubtract.
    StringData _opName;
};

class ExpressionDateAdd final : public ExpressionDateArithmetics {
public:
    using ExpressionDateArithmetics::ExpressionDateArithmetics;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    virtual Value evaluateDateArithmetics(Date_t date,
                                          TimeUnit unit,
                                          long long amount,
                                          const TimeZone& timezone) const override;
};

class ExpressionDateSubtract final : public ExpressionDateArithmetics {
public:
    using ExpressionDateArithmetics::ExpressionDateArithmetics;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    virtual Value evaluateDateArithmetics(Date_t date,
                                          TimeUnit unit,
                                          long long amount,
                                          const TimeZone& timezone) const override;
};

struct SubstituteFieldPathWalker {
    SubstituteFieldPathWalker(const StringMap<std::string>& renameList) : renameList(renameList) {}

    auto postVisit(Expression* exp) {
        if (auto fieldPathExpr = dynamic_cast<ExpressionFieldPath*>(exp)) {
            return fieldPathExpr->copyWithSubstitution(renameList);
        }
        return std::unique_ptr<Expression>{};
    }

    const StringMap<std::string>& renameList;
};

/**
 * $dateTrunc expression that maps a date to a lower bound of a bin of a certain size that the date
 * belongs to. It uses 2000-01-01T00:00:00.000 as a reference point.
 */
class ExpressionDateTrunc final : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);
    /**
     * date - an expression that resolves to a Value that is coercible to a Date.
     * unit - an expression defining units of bin size that resolves to a string Value.
     * binSize - an expression defining a size of bins in given units. Resolves to a Value coercible
     * to a 64-bit integer. Can be nullptr.
     * timezone - an expression defining a timezone to perform the operation in that resolves to a
     * string Value. Can be nullptr.
     * startOfWeek - an expression defining the week start day that resolves to a string Value. Can
     * be nullptr.
     */
    ExpressionDateTrunc(ExpressionContext* expCtx,
                        boost::intrusive_ptr<Expression> date,
                        boost::intrusive_ptr<Expression> unit,
                        boost::intrusive_ptr<Expression> binSize,
                        boost::intrusive_ptr<Expression> timezone,
                        boost::intrusive_ptr<Expression> startOfWeek);
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root, Variables* variables) const final;
    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    /**
     * Returns true if this expression has parameter 'timezone' specified, otherwise false.
     */
    bool isTimezoneSpecified() const {
        return static_cast<bool>(_timeZone);
    }

    /**
     * Returns true if this expression has parameter 'startOfWeek' specified, otherwise false.
     */
    bool isStartOfWeekSpecified() const {
        return static_cast<bool>(_startOfWeek);
    }

    /**
     * Returns true if this expression has parameter 'binSize' specified, otherwise false.
     */
    bool isBinSizeSpecified() const {
        return static_cast<bool>(_binSize);
    }

private:
    /**
     * Converts $dateTrunc expression parameter "date" 'value' to Date_t type.
     */
    static Date_t convertToDate(const Value& value);

    /**
     * Converts $dateTrunc expression parameter "binSize" 'value' to 64-bit integer.
     */
    static unsigned long long convertToBinSize(const Value& value);

    void _doAddDependencies(DepsTracker* deps) const final;

    // Expression that evaluates to a date to truncate. Accepted BSON types: Date, bsonTimestamp,
    // jstOID.
    boost::intrusive_ptr<Expression>& _date;

    // Time units used to describe the size of bins. Accepted BSON type: String. Accepted values:
    // enumerators from TimeUnit enumeration.
    boost::intrusive_ptr<Expression>& _unit;

    // Size of bins in time units '_unit'. Accepted BSON types: NumberInt, NumberLong, NumberDouble,
    // NumberDecimal. Accepted are only values that can be coerced to a 64-bit integer without loss.
    // If not specified, 1 is used.
    boost::intrusive_ptr<Expression>& _binSize;

    // Timezone to use for the truncation operation. Accepted BSON type: String. If not specified,
    // UTC is used.
    boost::intrusive_ptr<Expression>& _timeZone;

    // First/start day of the week to use for date truncation when the time unit is the week.
    // Accepted BSON type: String. If not specified, "sunday" is used.
    boost::intrusive_ptr<Expression>& _startOfWeek;
};

class ExpressionGetField final : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement exprElement,
                                                  const VariablesParseState& vps);

    /**
     * Constructs a $getField expression where 'field' is an expression resolving to a constant
     * string Value and 'input' is an expression resolving to an object Value (or null).
     *
     * If 'input' is nullish (but not missing), $getField evaluates to null. Furthermore, if 'input'
     * does not contain 'field', then $getField returns missing.
     */
    ExpressionGetField(ExpressionContext* const expCtx,
                       boost::intrusive_ptr<Expression> field,
                       boost::intrusive_ptr<Expression> input)
        : Expression(expCtx, {std::move(field), std::move(input)}),
          _field(_children[0]),
          _input(_children[1]) {
        expCtx->sbeCompatible = false;
    }

    Value serialize(bool explain) const final;

    Value evaluate(const Document& root, Variables* variables) const final;

    boost::intrusive_ptr<Expression> optimize() final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    static constexpr auto kExpressionName = "$getField"_sd;

protected:
    void _doAddDependencies(DepsTracker* deps) const final override;

private:
    boost::intrusive_ptr<Expression>& _field;
    boost::intrusive_ptr<Expression>& _input;
};

class ExpressionSetField final : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement exprElement,
                                                  const VariablesParseState& vps);

    /**
     * Constructs a $setField expression where 'field' is a constant string, 'input' is an
     * expression resolving to an object Value (or null), and 'value' is any expression.
     */
    ExpressionSetField(ExpressionContext* const expCtx,
                       boost::intrusive_ptr<Expression> field,
                       boost::intrusive_ptr<Expression> input,
                       boost::intrusive_ptr<Expression> value)
        : Expression(expCtx, {std::move(field), std::move(input), std::move(value)}),
          _field(_children[0]),
          _input(_children[1]),
          _value(_children[2]) {
        expCtx->sbeCompatible = false;
    }

    Value serialize(bool explain) const final;

    Value evaluate(const Document& root, Variables* variables) const final;

    boost::intrusive_ptr<Expression> optimize() final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    static constexpr auto kExpressionName = "$setField"_sd;

protected:
    void _doAddDependencies(DepsTracker* deps) const final override;

private:
    boost::intrusive_ptr<Expression>& _field;
    boost::intrusive_ptr<Expression>& _input;
    boost::intrusive_ptr<Expression>& _value;
};

class ExpressionTsSecond final : public ExpressionFixedArity<ExpressionTsSecond, 1> {
public:
    static constexpr const char* const opName = "$tsSecond";

    explicit ExpressionTsSecond(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionTsSecond, 1>(expCtx) {}

    ExpressionTsSecond(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionTsSecond, 1>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

    const char* getOpName() const final {
        return opName;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionTsIncrement final : public ExpressionFixedArity<ExpressionTsIncrement, 1> {
public:
    static constexpr const char* const opName = "$tsIncrement";

    explicit ExpressionTsIncrement(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionTsIncrement, 1>(expCtx) {}

    ExpressionTsIncrement(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionTsIncrement, 1>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

    const char* getOpName() const final {
        return opName;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

}  // namespace mongo
