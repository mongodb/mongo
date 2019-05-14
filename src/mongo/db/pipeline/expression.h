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

#include "mongo/platform/basic.h"

#include <algorithm>
#include <boost/intrusive_ptr.hpp>
#include <map>
#include <pcre.h>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/server_options.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/intrusive_counter.h"
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
 * REGISTER_EXPRESSION(foo, ExpressionFoo::parse);
 *
 * An expression registered this way can be used in any featureCompatibilityVersion.
 */
#define REGISTER_EXPRESSION(key, parser)                                     \
    MONGO_INITIALIZER(addToExpressionParserMap_##key)(InitializerContext*) { \
        Expression::registerExpression("$" #key, (parser), boost::none);     \
        return Status::OK();                                                 \
    }

/**
 * Registers a Parser so it can be called from parseExpression and friends. Use this version if your
 * expression can only be persisted to a catalog data structure in a feature compatibility version
 * >= X.
 *
 * As an example, if your expression looks like {"$foo": [1,2,3]}, and can only be used in a feature
 * compatibility version >= X, you would add this line:
 * REGISTER_EXPRESSION_WITH_MIN_VERSION(foo, ExpressionFoo::parse, X);
 */
#define REGISTER_EXPRESSION_WITH_MIN_VERSION(key, parser, minVersion)        \
    MONGO_INITIALIZER(addToExpressionParserMap_##key)(InitializerContext*) { \
        Expression::registerExpression("$" #key, (parser), (minVersion));    \
        return Status::OK();                                                 \
    }

class Expression : public RefCountable {
public:
    using Parser = stdx::function<boost::intrusive_ptr<Expression>(
        const boost::intrusive_ptr<ExpressionContext>&, BSONElement, const VariablesParseState&)>;

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
    void addDependencies(DepsTracker* deps) {
        _doAddDependencies(deps);

        // Filter out references to any local variables.
        if (_boundaryVariableId) {
            deps->vars.erase(deps->vars.upper_bound(*_boundaryVariableId), deps->vars.end());
        }
    }

    /**
     * Serialize the Expression tree recursively.
     *
     * If 'explain' is false, the returned Value must result in the same Expression when parsed by
     * parseOperand().
     */
    virtual Value serialize(bool explain) const = 0;

    /**
     * Evaluate expression with respect to the Document given by 'root', and return the result.
     */
    virtual Value evaluate(const Document& root) const = 0;

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
    virtual void acceptVisitor(ExpressionVisitor* visitor) = 0;

    /**
     * Parses a BSON Object that could represent an object literal or a functional expression like
     * $add.
     *
     * Calls parseExpression() on any sub-document (including possibly the entire document) which
     * consists of a single field name starting with a '$'.
     */
    static boost::intrusive_ptr<Expression> parseObject(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONObj obj,
        const VariablesParseState& vps);

    /**
     * Parses a BSONObj which has already been determined to be a functional expression.
     *
     * Throws an error if 'obj' does not contain exactly one field, or if that field's name does not
     * match a registered expression name.
     */
    static boost::intrusive_ptr<Expression> parseExpression(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONObj obj,
        const VariablesParseState& vps);

    /**
     * Parses a BSONElement which is an argument to an Expression.
     *
     * An argument is allowed to be another expression, or a literal value, so this can call
     * parseObject(), ExpressionFieldPath::parse(), ExpressionArray::parse(), or
     * ExpressionConstant::parse() as necessary.
     */
    static boost::intrusive_ptr<Expression> parseOperand(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement exprElement,
        const VariablesParseState& vps);

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
        boost::optional<ServerGlobalParams::FeatureCompatibility::Version> requiredMinVersion);

    const auto& getChildren() const {
        return _children;
    }

protected:
    using ExpressionVector = std::vector<boost::intrusive_ptr<Expression>>;

    Expression(const boost::intrusive_ptr<ExpressionContext>& expCtx) : Expression(expCtx, {}) {}

    Expression(const boost::intrusive_ptr<ExpressionContext>& expCtx, ExpressionVector&& children)
        : _children(std::move(children)), _expCtx(expCtx) {
        auto varIds = _expCtx->variablesParseState.getDefinedVariableIDs();
        if (!varIds.empty()) {
            _boundaryVariableId = *std::prev(varIds.end());
        }
    }


    const boost::intrusive_ptr<ExpressionContext>& getExpressionContext() const {
        return _expCtx;
    }

    virtual void _doAddDependencies(DepsTracker* deps) const = 0;

    /**
     * Owning container for all sub-Expressions.
     *
     * Some derived classes contain named fields since they orginate from user syntax containing
     * field names. These classes contain alternate data structures or object members for accessing
     * children. These structures or object memebers are expected to reference this data structure.
     * In addition this structure should not be modified by named-field derivied classes to avoid
     * invalidating references.
     */
    ExpressionVector _children;

private:
    boost::optional<Variables::Id> _boundaryVariableId;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
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

    static ExpressionVector parseArguments(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           BSONElement bsonExpr,
                                           const VariablesParseState& vps);

    const ExpressionVector& getOperandList() const {
        return _children;
    }

protected:
    explicit ExpressionNary(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : Expression(expCtx) {}

    void _doAddDependencies(DepsTracker* deps) const override;
};

/// Inherit from ExpressionVariadic or ExpressionFixedArity instead of directly from this class.
template <typename SubClass>
class ExpressionNaryBase : public ExpressionNary {
public:
    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement bsonExpr,
        const VariablesParseState& vps) {
        auto expr = make_intrusive<SubClass>(expCtx);
        ExpressionVector args = parseArguments(expCtx, bsonExpr, vps);
        expr->validateArguments(args);
        expr->_children = std::move(args);
        return std::move(expr);
    }

protected:
    explicit ExpressionNaryBase(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionNary(expCtx) {}
};

/// Inherit from this class if your expression takes a variable number of arguments.
template <typename SubClass>
class ExpressionVariadic : public ExpressionNaryBase<SubClass> {
public:
    explicit ExpressionVariadic(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionNaryBase<SubClass>(expCtx) {}
};

/**
 * Inherit from this class if your expression can take a range of arguments, e.g. if it has some
 * optional arguments.
 */
template <typename SubClass, int MinArgs, int MaxArgs>
class ExpressionRangedArity : public ExpressionNaryBase<SubClass> {
public:
    explicit ExpressionRangedArity(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionNaryBase<SubClass>(expCtx) {}

    void validateArguments(const Expression::ExpressionVector& args) const override {
        uassert(28667,
                str::stream() << "Expression " << this->getOpName() << " takes at least " << MinArgs
                              << " arguments, and at most "
                              << MaxArgs
                              << ", but "
                              << args.size()
                              << " were passed in.",
                MinArgs <= args.size() && args.size() <= MaxArgs);
    }
};

/// Inherit from this class if your expression takes a fixed number of arguments.
template <typename SubClass, int NArgs>
class ExpressionFixedArity : public ExpressionNaryBase<SubClass> {
public:
    explicit ExpressionFixedArity(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionNaryBase<SubClass>(expCtx) {}

    void validateArguments(const Expression::ExpressionVector& args) const override {
        uassert(16020,
                str::stream() << "Expression " << this->getOpName() << " takes exactly " << NArgs
                              << " arguments. "
                              << args.size()
                              << " were passed in.",
                args.size() == NArgs);
    }
};

/**
 * Used to make Accumulators available as Expressions, e.g., to make $sum available as an Expression
 * use "REGISTER_EXPRESSION(sum, ExpressionAccumulator<AccumulatorSum>::parse);".
 */
template <typename Accumulator>
class ExpressionFromAccumulator
    : public ExpressionVariadic<ExpressionFromAccumulator<Accumulator>> {
public:
    explicit ExpressionFromAccumulator(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionVariadic<ExpressionFromAccumulator<Accumulator>>(expCtx) {}

    Value evaluate(const Document& root) const final {
        Accumulator accum(this->getExpressionContext());
        const auto n = this->_children.size();
        // If a single array arg is given, loop through it passing each member to the accumulator.
        // If a single, non-array arg is given, pass it directly to the accumulator.
        if (n == 1) {
            Value singleVal = this->_children[0]->evaluate(root);
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
                accum.process(argument->evaluate(root), false);
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
        return Accumulator(this->getExpressionContext()).isAssociative();
    }

    bool isCommutative() const final {
        return Accumulator(this->getExpressionContext()).isCommutative();
    }

    const char* getOpName() const final {
        return Accumulator(this->getExpressionContext()).getOpName();
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};

/**
 * Inherit from this class if your expression takes exactly one numeric argument.
 */
template <typename SubClass>
class ExpressionSingleNumericArg : public ExpressionFixedArity<SubClass, 1> {
public:
    explicit ExpressionSingleNumericArg(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<SubClass, 1>(expCtx) {}

    virtual ~ExpressionSingleNumericArg() = default;

    Value evaluate(const Document& root) const final {
        Value arg = this->_children[0]->evaluate(root);
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
    explicit ExpressionTwoNumericArgs(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<SubClass, 2>(expCtx) {}

    virtual ~ExpressionTwoNumericArgs() = default;

    /**
     * Evaluate performs the type checking necessary to make sure that both arguments are numeric,
     * then calls the evaluateNumericArgs on the two numeric args:
     * 1. If either input is nullish, it returns null.
     * 2. If either input is not numeric, it throws an error.
     * 3. Call evaluateNumericArgs on the two numeric args.
     */
    Value evaluate(const Document& root) const final {
        Value arg1 = this->_children[0]->evaluate(root);
        if (arg1.nullish())
            return Value(BSONNULL);
        uassert(51044,
                str::stream() << this->getOpName() << " only supports numeric types, not "
                              << typeName(arg1.getType()),
                arg1.numeric());
        Value arg2 = this->_children[1]->evaluate(root);
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
    boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root) const final;
    Value serialize(bool explain) const final;

    const char* getOpName() const;

    /**
     * Creates a new ExpressionConstant with value 'value'.
     */
    static boost::intrusive_ptr<ExpressionConstant> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, const Value& value);

    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
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

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const override;

private:
    ExpressionConstant(const boost::intrusive_ptr<ExpressionContext>& expCtx, const Value& value);

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

    Value evaluate(const Document& root) const final {
        auto dateVal = _date->evaluate(root);
        if (dateVal.nullish()) {
            return Value(BSONNULL);
        }
        auto date = dateVal.coerceToDate();

        if (!_timeZone) {
            return evaluateDate(date, TimeZoneDatabase::utcZone());
        }
        auto timeZoneId = _timeZone->evaluate(root);
        if (timeZoneId.nullish()) {
            return Value(BSONNULL);
        }

        uassert(40533,
                str::stream() << _opName
                              << " requires a string for the timezone argument, but was given a "
                              << typeName(timeZoneId.getType())
                              << " ("
                              << timeZoneId.toString()
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
            return ExpressionConstant::create(getExpressionContext(), evaluate(Document{}));
        }
        return this;
    }

    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
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
                                                << argName
                                                << "\"");
                    }
                }
                uassert(40539,
                        str::stream() << "missing 'date' argument to " << opName << ", provided: "
                                      << operatorElem,
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
    explicit DateExpressionAcceptingTimeZone(const boost::intrusive_ptr<ExpressionContext>& expCtx,
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
    explicit ExpressionAbs(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionSingleNumericArg<ExpressionAbs>(expCtx) {}

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};

class ExpressionAdd final : public ExpressionVariadic<ExpressionAdd> {
public:
    explicit ExpressionAdd(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionVariadic<ExpressionAdd>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionAllElementsTrue final : public ExpressionFixedArity<ExpressionAllElementsTrue, 1> {
public:
    explicit ExpressionAllElementsTrue(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionAllElementsTrue, 1>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionAnd final : public ExpressionVariadic<ExpressionAnd> {
public:
    explicit ExpressionAnd(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionVariadic<ExpressionAnd>(expCtx) {}

    boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionAnyElementTrue final : public ExpressionFixedArity<ExpressionAnyElementTrue, 1> {
public:
    explicit ExpressionAnyElementTrue(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionAnyElementTrue, 1>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionArray final : public ExpressionVariadic<ExpressionArray> {
public:
    explicit ExpressionArray(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionVariadic<ExpressionArray>(expCtx) {}

    Value evaluate(const Document& root) const final;
    Value serialize(bool explain) const final;
    boost::intrusive_ptr<Expression> optimize() final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionArrayElemAt final : public ExpressionFixedArity<ExpressionArrayElemAt, 2> {
public:
    explicit ExpressionArrayElemAt(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionArrayElemAt, 2>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};

class ExpressionObjectToArray final : public ExpressionFixedArity<ExpressionObjectToArray, 1> {
public:
    explicit ExpressionObjectToArray(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionObjectToArray, 1>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};

class ExpressionArrayToObject final : public ExpressionFixedArity<ExpressionArrayToObject, 1> {
public:
    explicit ExpressionArrayToObject(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionArrayToObject, 1>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};

class ExpressionCeil final : public ExpressionSingleNumericArg<ExpressionCeil> {
public:
    explicit ExpressionCeil(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionSingleNumericArg<ExpressionCeil>(expCtx) {}

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionCoerceToBool final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root) const final;
    Value serialize(bool explain) const final;

    static boost::intrusive_ptr<ExpressionCoerceToBool> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::intrusive_ptr<Expression> pExpression);

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    ExpressionCoerceToBool(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           boost::intrusive_ptr<Expression> pExpression);

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

    ExpressionCompare(const boost::intrusive_ptr<ExpressionContext>& expCtx, CmpOp cmpOp)
        : ExpressionFixedArity<ExpressionCompare, 2>(expCtx), cmpOp(cmpOp) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    CmpOp getOp() const {
        return cmpOp;
    }

    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement bsonExpr,
        const VariablesParseState& vps,
        CmpOp cmpOp);

    static boost::intrusive_ptr<ExpressionCompare> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        CmpOp cmpOp,
        const boost::intrusive_ptr<Expression>& exprLeft,
        const boost::intrusive_ptr<Expression>& exprRight);

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

private:
    CmpOp cmpOp;
};


class ExpressionConcat final : public ExpressionVariadic<ExpressionConcat> {
public:
    explicit ExpressionConcat(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionVariadic<ExpressionConcat>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionConcatArrays final : public ExpressionVariadic<ExpressionConcatArrays> {
public:
    explicit ExpressionConcatArrays(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionVariadic<ExpressionConcatArrays>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionCond final : public ExpressionFixedArity<ExpressionCond, 3> {
public:
    explicit ExpressionCond(const boost::intrusive_ptr<ExpressionContext>& expCtx) : Base(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vps);

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

private:
    typedef ExpressionFixedArity<ExpressionCond, 3> Base;
};

class ExpressionDateFromString final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document&) const final;

    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vps);

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    ExpressionDateFromString(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             boost::intrusive_ptr<Expression> dateString,
                             boost::intrusive_ptr<Expression> timeZone,
                             boost::intrusive_ptr<Expression> format,
                             boost::intrusive_ptr<Expression> onNull,
                             boost::intrusive_ptr<Expression> onError);

    boost::intrusive_ptr<Expression>& _dateString;
    boost::intrusive_ptr<Expression>& _timeZone;
    boost::intrusive_ptr<Expression>& _format;
    boost::intrusive_ptr<Expression>& _onNull;
    boost::intrusive_ptr<Expression>& _onError;
};

class ExpressionDateFromParts final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root) const final;

    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vps);

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    ExpressionDateFromParts(const boost::intrusive_ptr<ExpressionContext>& expCtx,
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
                                   boost::intrusive_ptr<Expression> field,
                                   StringData fieldName,
                                   long long defaultValue,
                                   long long* returnValue) const;

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
};

class ExpressionDateToParts final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root) const final;

    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vps);

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    /**
     * The iso8601 argument controls whether to output ISO8601 elements or natural calendar.
     */
    ExpressionDateToParts(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          boost::intrusive_ptr<Expression> date,
                          boost::intrusive_ptr<Expression> timeZone,
                          boost::intrusive_ptr<Expression> iso8601);

    boost::optional<int> evaluateIso8601Flag(const Document& root) const;

    boost::intrusive_ptr<Expression>& _date;
    boost::intrusive_ptr<Expression>& _timeZone;
    boost::intrusive_ptr<Expression>& _iso8601;
};

class ExpressionDateToString final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root) const final;

    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vps);

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    ExpressionDateToString(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           boost::intrusive_ptr<Expression> format,
                           boost::intrusive_ptr<Expression> date,
                           boost::intrusive_ptr<Expression> timeZone,
                           boost::intrusive_ptr<Expression> onNull);

    boost::intrusive_ptr<Expression>& _format;
    boost::intrusive_ptr<Expression>& _date;
    boost::intrusive_ptr<Expression>& _timeZone;
    boost::intrusive_ptr<Expression>& _onNull;
};

class ExpressionDayOfMonth final : public DateExpressionAcceptingTimeZone<ExpressionDayOfMonth> {
public:
    explicit ExpressionDayOfMonth(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  boost::intrusive_ptr<Expression> date,
                                  boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionDayOfMonth>(
              expCtx, "$dayOfMonth", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).dayOfMonth);
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionDayOfWeek final : public DateExpressionAcceptingTimeZone<ExpressionDayOfWeek> {
public:
    explicit ExpressionDayOfWeek(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 boost::intrusive_ptr<Expression> date,
                                 boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionDayOfWeek>(
              expCtx, "$dayOfWeek", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dayOfWeek(date));
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionDayOfYear final : public DateExpressionAcceptingTimeZone<ExpressionDayOfYear> {
public:
    explicit ExpressionDayOfYear(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 boost::intrusive_ptr<Expression> date,
                                 boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionDayOfYear>(
              expCtx, "$dayOfYear", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dayOfYear(date));
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionDivide final : public ExpressionFixedArity<ExpressionDivide, 2> {
public:
    explicit ExpressionDivide(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionDivide, 2>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionExp final : public ExpressionSingleNumericArg<ExpressionExp> {
public:
    explicit ExpressionExp(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionSingleNumericArg<ExpressionExp>(expCtx) {}

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionFieldPath final : public Expression {
public:
    bool isRootFieldPath() const {
        return _variable == Variables::kRootId;
    }

    boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root) const final;
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
    static boost::intrusive_ptr<ExpressionFieldPath> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, const std::string& fieldPath);

    /// Like create(), but works with the raw std::string from the user with the "$" prefixes.
    static boost::intrusive_ptr<ExpressionFieldPath> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const std::string& raw,
        const VariablesParseState& vps);

    /**
     * Returns true if this expression logically represents the path 'dottedPath'. For example, if
     * 'dottedPath' is 'a.b' and this FieldPath is '$$CURRENT.a.b', returns true.
     */
    bool representsPath(const std::string& dottedPath) const;

    const FieldPath& getFieldPath() const {
        return _fieldPath;
    }

    ComputedPaths getComputedPaths(const std::string& exprFieldPath,
                                   Variables::Id renamingVar) const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    ExpressionFieldPath(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        const std::string& fieldPath,
                        Variables::Id variable);

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
    Value evaluate(const Document& root) const final;

    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vps);

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    ExpressionFilter(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                     std::string varName,
                     Variables::Id varId,
                     boost::intrusive_ptr<Expression> input,
                     boost::intrusive_ptr<Expression> filter);

    // The name of the variable to set to each element in the array.
    std::string _varName;
    // The id of the variable to set.
    Variables::Id _varId;
    // The array to iterate over.
    boost::intrusive_ptr<Expression>& _input;
    // The expression determining whether each element should be present in the result array.
    boost::intrusive_ptr<Expression>& _filter;
};


class ExpressionFloor final : public ExpressionSingleNumericArg<ExpressionFloor> {
public:
    explicit ExpressionFloor(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionSingleNumericArg<ExpressionFloor>(expCtx) {}

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionHour final : public DateExpressionAcceptingTimeZone<ExpressionHour> {
public:
    explicit ExpressionHour(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            boost::intrusive_ptr<Expression> date,
                            boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionHour>(
              expCtx, "$hour", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).hour);
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionIfNull final : public ExpressionFixedArity<ExpressionIfNull, 2> {
public:
    explicit ExpressionIfNull(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionIfNull, 2>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionIn final : public ExpressionFixedArity<ExpressionIn, 2> {
public:
    explicit ExpressionIn(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionIn, 2>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionIndexOfArray : public ExpressionRangedArity<ExpressionIndexOfArray, 2, 4> {
public:
    explicit ExpressionIndexOfArray(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionRangedArity<ExpressionIndexOfArray, 2, 4>(expCtx) {}


    Value evaluate(const Document& root) const;
    boost::intrusive_ptr<Expression> optimize() final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
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
                                           size_t arrayLength) const;

private:
    class Optimized;
};


class ExpressionIndexOfBytes final : public ExpressionRangedArity<ExpressionIndexOfBytes, 2, 4> {
public:
    explicit ExpressionIndexOfBytes(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionRangedArity<ExpressionIndexOfBytes, 2, 4>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


/**
 * Implements indexOf behavior for strings with UTF-8 encoding.
 */
class ExpressionIndexOfCP final : public ExpressionRangedArity<ExpressionIndexOfCP, 2, 4> {
public:
    explicit ExpressionIndexOfCP(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionRangedArity<ExpressionIndexOfCP, 2, 4>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionLet final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root) const final;

    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vps);

    struct NameAndExpression {
        std::string name;
        boost::intrusive_ptr<Expression>& expression;
    };

    typedef std::map<Variables::Id, NameAndExpression> VariableMap;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    ExpressionLet(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  VariableMap&& vars,
                  std::vector<boost::intrusive_ptr<Expression>> children);

    VariableMap _variables;
    boost::intrusive_ptr<Expression>& _subExpression;
};

class ExpressionLn final : public ExpressionSingleNumericArg<ExpressionLn> {
public:
    explicit ExpressionLn(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionSingleNumericArg<ExpressionLn>(expCtx) {}

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};

class ExpressionLog final : public ExpressionFixedArity<ExpressionLog, 2> {
public:
    explicit ExpressionLog(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionLog, 2>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};

class ExpressionLog10 final : public ExpressionSingleNumericArg<ExpressionLog10> {
public:
    explicit ExpressionLog10(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionSingleNumericArg<ExpressionLog10>(expCtx) {}

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};

class ExpressionMap final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root) const final;

    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vps);

    ComputedPaths getComputedPaths(const std::string& exprFieldPath,
                                   Variables::Id renamingVar) const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    ExpressionMap(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const std::string& varName,              // name of variable to set
        Variables::Id varId,                     // id of variable to set
        boost::intrusive_ptr<Expression> input,  // yields array to iterate
        boost::intrusive_ptr<Expression> each);  // yields results to be added to output array

    std::string _varName;
    Variables::Id _varId;
    boost::intrusive_ptr<Expression>& _input;
    boost::intrusive_ptr<Expression>& _each;
};

class ExpressionMeta final : public Expression {
public:
    Value serialize(bool explain) const final;
    Value evaluate(const Document& root) const final;

    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vps);

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    enum MetaType {
        TEXT_SCORE,
        RAND_VAL,
        SEARCH_SCORE,
    };

    ExpressionMeta(const boost::intrusive_ptr<ExpressionContext>& expCtx, MetaType metaType);

    MetaType _metaType;
};

class ExpressionMillisecond final : public DateExpressionAcceptingTimeZone<ExpressionMillisecond> {
public:
    explicit ExpressionMillisecond(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   boost::intrusive_ptr<Expression> date,
                                   boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionMillisecond>(
              expCtx, "$millisecond", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).millisecond);
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionMinute final : public DateExpressionAcceptingTimeZone<ExpressionMinute> {
public:
    explicit ExpressionMinute(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              boost::intrusive_ptr<Expression> date,
                              boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionMinute>(
              expCtx, "$minute", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).minute);
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionMod final : public ExpressionFixedArity<ExpressionMod, 2> {
public:
    explicit ExpressionMod(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionMod, 2>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionMultiply final : public ExpressionVariadic<ExpressionMultiply> {
public:
    explicit ExpressionMultiply(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionVariadic<ExpressionMultiply>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionMonth final : public DateExpressionAcceptingTimeZone<ExpressionMonth> {
public:
    explicit ExpressionMonth(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             boost::intrusive_ptr<Expression> date,
                             boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionMonth>(
              expCtx, "$month", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).month);
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionNot final : public ExpressionFixedArity<ExpressionNot, 1> {
public:
    explicit ExpressionNot(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionNot, 1>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
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
    Value evaluate(const Document& root) const final;
    Value serialize(bool explain) const final;

    static boost::intrusive_ptr<ExpressionObject> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::vector<boost::intrusive_ptr<Expression>> children,
        std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>&>>&& expressions);

    /**
     * Parses and constructs an ExpressionObject from 'obj'.
     */
    static boost::intrusive_ptr<ExpressionObject> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
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

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    ExpressionObject(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::vector<boost::intrusive_ptr<Expression>> children,
        std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>&>>&& expressions);

    // The mapping from field name to expression within this object. This needs to respect the order
    // in which the fields were specified in the input BSON.
    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>&>> _expressions;
};


class ExpressionOr final : public ExpressionVariadic<ExpressionOr> {
public:
    explicit ExpressionOr(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionVariadic<ExpressionOr>(expCtx) {}

    boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};

class ExpressionPow final : public ExpressionFixedArity<ExpressionPow, 2> {
public:
    explicit ExpressionPow(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionPow, 2>(expCtx) {}

    static boost::intrusive_ptr<Expression> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, Value base, Value exp);

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

private:
    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;
};


class ExpressionRange final : public ExpressionRangedArity<ExpressionRange, 2, 3> {
public:
    explicit ExpressionRange(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionRangedArity<ExpressionRange, 2, 3>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionReduce final : public Expression {
public:
    ExpressionReduce(const boost::intrusive_ptr<ExpressionContext>& expCtx,
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
          _valueVar(valueVar) {}

    Value evaluate(const Document& root) const final;
    boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vps);
    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
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


class ExpressionSecond final : public DateExpressionAcceptingTimeZone<ExpressionSecond> {
public:
    ExpressionSecond(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                     boost::intrusive_ptr<Expression> date,
                     boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionSecond>(
              expCtx, "$second", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).second);
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionSetDifference final : public ExpressionFixedArity<ExpressionSetDifference, 2> {
public:
    explicit ExpressionSetDifference(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionSetDifference, 2>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionSetEquals final : public ExpressionVariadic<ExpressionSetEquals> {
public:
    explicit ExpressionSetEquals(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionVariadic<ExpressionSetEquals>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;
    void validateArguments(const ExpressionVector& args) const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionSetIntersection final : public ExpressionVariadic<ExpressionSetIntersection> {
public:
    explicit ExpressionSetIntersection(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionVariadic<ExpressionSetIntersection>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


// Not final, inherited from for optimizations.
class ExpressionSetIsSubset : public ExpressionFixedArity<ExpressionSetIsSubset, 2> {
public:
    explicit ExpressionSetIsSubset(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionSetIsSubset, 2>(expCtx) {}

    boost::intrusive_ptr<Expression> optimize() override;
    Value evaluate(const Document& root) const override;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

private:
    class Optimized;
};


class ExpressionSetUnion final : public ExpressionVariadic<ExpressionSetUnion> {
public:
    explicit ExpressionSetUnion(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionVariadic<ExpressionSetUnion>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionSize final : public ExpressionFixedArity<ExpressionSize, 1> {
public:
    explicit ExpressionSize(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionSize, 1>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionReverseArray final : public ExpressionFixedArity<ExpressionReverseArray, 1> {
public:
    explicit ExpressionReverseArray(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionReverseArray, 1>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionSlice final : public ExpressionRangedArity<ExpressionSlice, 2, 3> {
public:
    explicit ExpressionSlice(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionRangedArity<ExpressionSlice, 2, 3>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionIsArray final : public ExpressionFixedArity<ExpressionIsArray, 1> {
public:
    explicit ExpressionIsArray(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionIsArray, 1>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};

class ExpressionRound final : public ExpressionRangedArity<ExpressionRound, 1, 2> {
public:
    explicit ExpressionRound(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionRangedArity<ExpressionRound, 1, 2>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};

class ExpressionSplit final : public ExpressionFixedArity<ExpressionSplit, 2> {
public:
    explicit ExpressionSplit(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionSplit, 2>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionSqrt final : public ExpressionSingleNumericArg<ExpressionSqrt> {
public:
    explicit ExpressionSqrt(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionSingleNumericArg<ExpressionSqrt>(expCtx) {}

    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionStrcasecmp final : public ExpressionFixedArity<ExpressionStrcasecmp, 2> {
public:
    explicit ExpressionStrcasecmp(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionStrcasecmp, 2>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionSubstrBytes final : public ExpressionFixedArity<ExpressionSubstrBytes, 3> {
public:
    explicit ExpressionSubstrBytes(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionSubstrBytes, 3>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionSubstrCP final : public ExpressionFixedArity<ExpressionSubstrCP, 3> {
public:
    explicit ExpressionSubstrCP(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionSubstrCP, 3>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionStrLenBytes final : public ExpressionFixedArity<ExpressionStrLenBytes, 1> {
public:
    explicit ExpressionStrLenBytes(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionStrLenBytes, 1>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionStrLenCP final : public ExpressionFixedArity<ExpressionStrLenCP, 1> {
public:
    explicit ExpressionStrLenCP(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionStrLenCP, 1>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionSubtract final : public ExpressionFixedArity<ExpressionSubtract, 2> {
public:
    explicit ExpressionSubtract(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionSubtract, 2>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionSwitch final : public Expression {
public:
    using ExpressionPair =
        std::pair<boost::intrusive_ptr<Expression>&, boost::intrusive_ptr<Expression>&>;

    ExpressionSwitch(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                     std::vector<boost::intrusive_ptr<Expression>> children,
                     std::vector<ExpressionPair> branches)
        : Expression(expCtx, std::move(children)),
          _default(_children.back()),
          _branches(std::move(branches)) {}

    Value evaluate(const Document& root) const final;
    boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vpsIn);
    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
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
    explicit ExpressionToLower(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionToLower, 1>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionToUpper final : public ExpressionFixedArity<ExpressionToUpper, 1> {
public:
    explicit ExpressionToUpper(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionToUpper, 1>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


/**
 * This class is used to implement all three trim expressions: $trim, $ltrim, and $rtrim.
 */
class ExpressionTrim final : public Expression {
private:
    enum class TrimType {
        kBoth,
        kLeft,
        kRight,
    };

public:
    ExpressionTrim(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                   TrimType trimType,
                   StringData name,
                   boost::intrusive_ptr<Expression> input,
                   boost::intrusive_ptr<Expression> charactersToTrim)
        : Expression(expCtx, {std::move(input), std::move(charactersToTrim)}),
          _trimType(trimType),
          _name(name.toString()),
          _input(_children[0]),
          _characters(_children[1]) {}

    Value evaluate(const Document& root) const final;
    boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vpsIn);
    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
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
    explicit ExpressionTrunc(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionRangedArity<ExpressionTrunc, 1, 2>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionType final : public ExpressionFixedArity<ExpressionType, 1> {
public:
    explicit ExpressionType(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionFixedArity<ExpressionType, 1>(expCtx) {}

    Value evaluate(const Document& root) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionWeek final : public DateExpressionAcceptingTimeZone<ExpressionWeek> {
public:
    ExpressionWeek(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                   boost::intrusive_ptr<Expression> date,
                   boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionWeek>(
              expCtx, "$week", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.week(date));
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionIsoWeekYear final : public DateExpressionAcceptingTimeZone<ExpressionIsoWeekYear> {
public:
    explicit ExpressionIsoWeekYear(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   boost::intrusive_ptr<Expression> date,
                                   boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionIsoWeekYear>(
              expCtx, "$isoWeekYear", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.isoYear(date));
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionIsoDayOfWeek final
    : public DateExpressionAcceptingTimeZone<ExpressionIsoDayOfWeek> {
public:
    ExpressionIsoDayOfWeek(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           boost::intrusive_ptr<Expression> date,
                           boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionIsoDayOfWeek>(
              expCtx, "$isoDayOfWeek", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.isoDayOfWeek(date));
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionIsoWeek final : public DateExpressionAcceptingTimeZone<ExpressionIsoWeek> {
public:
    ExpressionIsoWeek(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      boost::intrusive_ptr<Expression> date,
                      boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionIsoWeek>(
              expCtx, "$isoWeek", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.isoWeek(date));
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionYear final : public DateExpressionAcceptingTimeZone<ExpressionYear> {
public:
    ExpressionYear(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                   boost::intrusive_ptr<Expression> date,
                   boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone<ExpressionYear>(
              expCtx, "$year", std::move(date), std::move(timeZone)) {}

    Value evaluateDate(Date_t date, const TimeZone& timeZone) const final {
        return Value(timeZone.dateParts(date).year);
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


class ExpressionZip final : public Expression {
public:
    ExpressionZip(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  bool useLongestLength,
                  std::vector<boost::intrusive_ptr<Expression>> children,
                  std::vector<std::reference_wrapper<boost::intrusive_ptr<Expression>>> inputs,
                  std::vector<std::reference_wrapper<boost::intrusive_ptr<Expression>>> defaults)
        : Expression(expCtx, std::move(children)),
          _useLongestLength(useLongestLength),
          _inputs(std::move(inputs)),
          _defaults(std::move(defaults)) {}

    Value evaluate(const Document& root) const final;
    boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vpsIn);
    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
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
    /**
     * Creates a $convert expression converting from 'input' to the type given by 'toType'. Leaves
     * 'onNull' and 'onError' unspecified.
     */
    static boost::intrusive_ptr<Expression> create(const boost::intrusive_ptr<ExpressionContext>&,
                                                   boost::intrusive_ptr<Expression> input,
                                                   BSONType toType);

    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vpsIn);

    Value evaluate(const Document& root) const final;
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final;

private:
    ExpressionConvert(const boost::intrusive_ptr<ExpressionContext>&,
                      boost::intrusive_ptr<Expression> input,
                      boost::intrusive_ptr<Expression> to,
                      boost::intrusive_ptr<Expression> onError,
                      boost::intrusive_ptr<Expression> onNull);

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
        std::shared_ptr<pcre> pcrePtr;

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
    RegexExecutionState buildInitialState(const Document& root) const;

    /**
     * Checks if there is a match for the given input and pattern that are part of 'executionState'.
     * The method will return a positive number if there is a match and '-1' if there is no match.
     * Throws 'uassert()' for any errors.
     */
    int execute(RegexExecutionState* executionState) const;

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

    Value serialize(bool explain) const;

    const std::string& getOpName() const {
        return _opName;
    }

    ExpressionRegex(const boost::intrusive_ptr<ExpressionContext>& expCtx,
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
    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vpsIn);

    Value evaluate(const Document& root) const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

    using ExpressionRegex::ExpressionRegex;
};

class ExpressionRegexFindAll final : public ExpressionRegex {
public:
    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vpsIn);

    Value evaluate(const Document& root) const final;
    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

    using ExpressionRegex::ExpressionRegex;
};

class ExpressionRegexMatch final : public ExpressionRegex {
public:
    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vpsIn);

    Value evaluate(const Document& root) const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

    using ExpressionRegex::ExpressionRegex;
};
}
