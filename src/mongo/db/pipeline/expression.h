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

#include <boost/intrusive_ptr.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/fle_crypto_predicate.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/accumulator_percentile_enum_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/monotonic_expression.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/util/named_enum.h"
#include "mongo/db/update/pattern_cmp.h"
#include "mongo/db/version_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/pcre.h"
#include "mongo/util/safe_num.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mongo {
constexpr size_t kMaxArgumentCountForSwitchAndSetExprForSbe = 100;

class BSONElement;

/**
 * The reasons why an expression is disabled (used for better error messages):
 *
 * testCommandsDisabled: Expression was registered via REGISTER_TEST_EXPRESSION, but
 *                       getTestCommandsEnabled() is false.
 * featureFlagDisabled:  Expression was registered via REGISTER_EXPRESSION_WITH_FEATURE_FLAG, but
 *                       the feature flag was not enabled.
 * otherReasonDisabled:  Expression was registered differently; probably by directly calling
 *                       REGISTER_EXPRESSION_CONDITIONALLY with evaluatedCondition set to false.
 */
enum class ExpressionDisabledReason {
    testCommandsDisabled,
    featureFlagDisabled,
    otherReasonDisabled,
};

/**
 * You can specify a condition, evaluated during startup,
 * that decides whether to register the parser.
 *
 * For example, you could check a feature flag, and register the parser only when it's enabled.
 *
 * Note that the condition is evaluated only once, during a MONGO_INITIALIZER. Don't specify
 * a condition that can change at runtime, such as FCV. (Feature flags are ok, because they
 * cannot be toggled at runtime.)
 *
 * This is the most general REGISTER_EXPRESSION* macro, which all others should delegate to.
 */
#define REGISTER_EXPRESSION_CONDITIONALLY(                                                      \
    key, parser, allowedWithApiStrict, allowedClientType, featureFlag, ...)                     \
    MONGO_INITIALIZER_GENERAL(addToExpressionParserMap_##key,                                   \
                              ("BeginExpressionRegistration"),                                  \
                              ("EndExpressionRegistration"))                                    \
    (InitializerContext*) {                                                                     \
        /* Require 'featureFlag' to be a constexpr. */                                          \
        constexpr FeatureFlag* constFeatureFlag{featureFlag};                                   \
        /* This non-constexpr variable works around a bug in GCC when 'featureFlag' is null. */ \
        FeatureFlag* featureFlagValue{constFeatureFlag};                                        \
        bool evaluatedCondition{__VA_ARGS__};                                                   \
        if (!evaluatedCondition || (featureFlagValue && !featureFlagValue->canBeEnabled())) {   \
            Expression::registerDisabledExpressionName(                                         \
                "$" #key,                                                                       \
                evaluatedCondition ? ExpressionDisabledReason::featureFlagDisabled              \
                                   : (getTestCommandsEnabled()                                  \
                                          ? ExpressionDisabledReason::otherReasonDisabled       \
                                          : ExpressionDisabledReason::testCommandsDisabled));   \
            return;                                                                             \
        }                                                                                       \
        Expression::registerExpression(                                                         \
            "$" #key, (parser), (allowedWithApiStrict), (allowedClientType), (featureFlag));    \
    }

/**
 * Registers a Parser so it can be called from parseExpression and friends.
 *
 * As an example, if your expression looks like {"$foo": [1,2,3]} you would add this line:
 * REGISTER_STABLE_EXPRESSION(foo, ExpressionFoo::parse);
 *
 * An expression registered this way can be used in any featureCompatibilityVersion and will be
 * considered part of the stable API.
 */
#define REGISTER_STABLE_EXPRESSION(key, parser)                      \
    REGISTER_EXPRESSION_CONDITIONALLY(key,                           \
                                      parser,                        \
                                      AllowedWithApiStrict::kAlways, \
                                      AllowedWithClientType::kAny,   \
                                      nullptr, /* featureFlag */     \
                                      true)

/**
 * Registers a Parser so it can be called from parseExpression and friends. Use this version if your
 * expression can only be persisted to a catalog data structure in a feature compatibility version
 * that enables the featureFlag.
 *
 * As an example, if your expression looks like {"$foo": [1,2,3]}, and can only be used in a feature
 * compatibility version that enables featureFlag, you would add this line:
 * REGISTER_EXPRESSION_WITH_FEATURE_FLAG(
 *  foo,
 *  ExpressionFoo::parse,
 *  AllowedWithApiStrict::kNeverInVersion1,
 *  AllowedWithClientType::kAny,
 *  featureFlag);
 *
 * Generally new language features should be excluded from the stable API for a stabilization period
 * to allow for incorporating feedback or fixing accidental semantics bugs.
 *
 * If 'allowedWithApiStrict' is set to 'kSometimes', this expression is expected to register its own
 * parser and enforce the 'sometimes' behavior during that invocation. No extra validation will be
 * done here.
 */
#define REGISTER_EXPRESSION_WITH_FEATURE_FLAG(                         \
    key, parser, allowedWithApiStrict, allowedClientType, featureFlag) \
    REGISTER_EXPRESSION_CONDITIONALLY(                                 \
        key, parser, allowedWithApiStrict, allowedClientType, featureFlag, true)

/**
 * Registers a Parser only if test commands are enabled. Use this if your expression is only used
 * for testing purposes. If the expression requires a FCV gated feature flag, the feature flag
 * should be tied to a permanent FCV (see 'featureFlagBlender'). If the expression does not require
 * a feature flag, a 'nullptr' should be passed.
 */
#define REGISTER_TEST_EXPRESSION(                                      \
    key, parser, allowedWithApiStrict, allowedClientType, featureFlag) \
    REGISTER_EXPRESSION_CONDITIONALLY(key,                             \
                                      parser,                          \
                                      allowedWithApiStrict,            \
                                      allowedClientType,               \
                                      featureFlag,                     \
                                      getTestCommandsEnabled())

class Expression : public RefCountable {
public:
    using Parser = std::function<boost::intrusive_ptr<Expression>(
        ExpressionContext* const, BSONElement, const VariablesParseState&)>;
    using ExpressionVector = std::vector<boost::intrusive_ptr<Expression>>;

    /**
     * Describes the paths that an expression produces when it is part of a projection,
     * or a projection-like clause such as $group _id.
     *
     * Produced by 'Expression::getComputedPaths()'.
     */
    struct ComputedPaths {
        /**
         * Non-rename computed paths.
         *
         * Mutually exclusive with the keys of 'renames', but may overlap with the keys of
         * 'complexRenames'.
         */
        OrderedPathSet paths;

        /**
         * A rename is when the values in a computed path are the same as the values in some
         * other path immediately before the projection. (Plural "values" because, for example,
         * a MatchExpression path can refer to zero or more subparts of a document.)
         *
         * For example in {$project: {a: "$x"}}, 'a' is renamed from 'x'.
         *
         * 'renames' maps from new name to old name: {"a", "x"} in that example.
         */
        StringMap<std::string> renames;

        // Like 'renames' except it also includes expressions which have dotted notation on the
        // right side.
        StringMap<std::string> complexRenames;
    };

    ~Expression() override {}

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
    [[nodiscard]] virtual boost::intrusive_ptr<Expression> optimize() {
        return this;
    }

    /**
     * Serialize the Expression tree recursively.
     *
     * If 'explain' is false, the returned Value must result in the same Expression when parsed by
     * parseOperand().
     */
    virtual Value serialize(const SerializationOptions& options = {}) const = 0;

    /**
     * Evaluate the expression with respect to the Document given by 'root' and the Variables given
     * by 'variables'. It is an error to supply a Variables argument whose built-in variables (like
     * $$NOW) are not set. This method is thread-safe, so long as the 'variables' passed in here is
     * not shared between threads.
     */
    virtual Value evaluate(const Document& root, Variables* variables) const = 0;

    /**
     * Returns information about the paths computed by this expression: see 'ComputedPaths'.
     *
     * This only needs to be overridden by expressions that have renaming semantics, where
     * optimization code could take advantage of knowledge of these renames.
     *
     * The 'exprFieldPath' is the field path at which the result of this expression will be stored.
     * This is used to determine the value of the "new" path created by the rename.
     *
     * The 'renamingVar' is needed for checking whether a field path is a rename. For example, at
     * the top level only field paths that begin with the ROOT variable, as in "$$ROOT.path", are
     * renames. A field path such as "$$var.path" is not a rename.
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
    static void registerExpression(std::string key,
                                   Parser parser,
                                   AllowedWithApiStrict allowedWithApiStrict,
                                   AllowedWithClientType allowedWithClientType,
                                   FeatureFlag* featureFlag);

    /**
     * Register an expression name as disabled to later improve the error message via
     * getErrorMessage. There is no need to call this function directly - it will be called in
     * REGISTER_EXPRESSION_CONDITIONALLY.
     */
    static void registerDisabledExpressionName(std::string key, ExpressionDisabledReason reason);

    /**
     * Return an error message for an unknown expression. Adds a hint in case the expression is
     * switched off by a feature flag or is only available in testing mode.
     */
    static std::string getErrorMessage(StringData key);

    const ExpressionVector& getChildren() const {
        return _children;
    }
    ExpressionVector& getChildren() {
        return _children;
    }

    auto getExpressionContext() const {
        return _expCtx;
    }

    boost::optional<Variables::Id> getBoundaryVariableId() const {
        return _boundaryVariableId;
    }

    bool isMonotonic(const FieldPath& sortedFieldPath) const {
        return getMonotonicState(sortedFieldPath) != monotonic::State::NonMonotonic;
    }

    virtual monotonic::State getMonotonicState(const FieldPath& sortedFieldPath) const {
        return monotonic::State::NonMonotonic;
    }

    /**
     * Helper to determine whether this expression always evaluates to the same value.
     */
    virtual bool selfAndChildrenAreConstant() const {
        return false;
    }

protected:
    Expression(ExpressionContext* const expCtx) : Expression(expCtx, {}) {}

    Expression(ExpressionContext* const expCtx, ExpressionVector&& children)
        : _children(std::move(children)), _expCtx(expCtx) {
        auto varIds = _expCtx->variablesParseState.getDefinedVariableIDs();
        if (!varIds.empty()) {
            _boundaryVariableId = *std::prev(varIds.end());
        }
    }

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
    // Tracks the latest Variable ID which is defined outside of this expression. Useful for
    // dependency analysis to avoid reporting dependencies to local variables defined by this
    // Expression.
    boost::optional<Variables::Id> _boundaryVariableId;
    ExpressionContext* const _expCtx;
};

/**
 * A constant expression. Repeated calls to evaluate() will always return the same thing.
 */
class ExpressionConstant final : public Expression {
public:
    ExpressionConstant(ExpressionContext* expCtx, const Value& value);

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root, Variables* variables) const final;
    Value serialize(const SerializationOptions& options = {}) const final;

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
     * Returns true if 'expression' is an instance of an ExpressionConstant.
     */
    static bool isConstant(boost::intrusive_ptr<Expression> expression) {
        return dynamic_cast<ExpressionConstant*>(expression.get());
    }

    static Value serializeConstant(const SerializationOptions& opts,
                                   Value val,
                                   bool wrapRepresentativeValue = true);

    bool selfAndChildrenAreConstant() const final {
        return true;
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
    template <typename ExpressionContainer>
    static bool allConstant(const ExpressionContainer& expressions) {
        return std::all_of(expressions.begin(), expressions.end(), [](auto exp) {
            return ExpressionConstant::isConstant(exp);
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
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    monotonic::State getMonotonicState(const FieldPath& sortedFieldPath) const final {
        return monotonic::State::Constant;
    }

    Value _value;
};

/**
 * Inherit from ExpressionVariadic or ExpressionFixedArity instead of directly from this class.
 */
class ExpressionNary : public Expression {
public:
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() override;
    Value serialize(const SerializationOptions& options = {}) const override;

    /*
      Add an operand to the n-ary expression.

      @param pExpression the expression to add
    */
    virtual void addOperand(const boost::intrusive_ptr<Expression>& pExpression);

    enum class Associativity { kFull, kLeft, kNone };

    virtual Associativity getAssociativity() const {
        return Associativity::kNone;
    }

    virtual bool isCommutative() const {
        return false;
    }

    virtual const char* getOpName() const = 0;

    virtual void validateChildren() const {}

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
};

/// Inherit from ExpressionVariadic or ExpressionFixedArity instead of directly from this class.
template <typename SubClass>
class ExpressionNaryBase : public ExpressionNary {
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* const expCtx,
                                                  BSONElement bsonExpr,
                                                  const VariablesParseState& vps) {
        ExpressionVector args = parseArguments(expCtx, bsonExpr, vps);
        auto expr = make_intrusive<SubClass>(expCtx, std::move(args));
        expr->validateChildren();
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

    Value serialize(const SerializationOptions& options = {}) const override {
        // As a special case, we would like to serialize a variadic number of children as
        // "?array<?subtype>" if they are all constant. Check for that here, otherwise default to
        // the normal one-by-one serialization of the children.
        if (options.isSerializingLiteralsAsDebugTypes() &&
            ExpressionConstant::allConstant(this->_children)) {
            // We could evaluate the expression right here and now and end up with just the one
            // constant answer, but this is not an optimization funciton, it is meant to just
            // serialize what we have, so let's preserve the array of constants.
            auto args = [&]() {
                std::vector<Value> values;
                const auto& constants = this->_children;
                values.reserve(constants.size());
                std::transform(constants.begin(),
                               constants.end(),
                               std::back_inserter(values),
                               [](const auto& exp) {
                                   return static_cast<ExpressionConstant*>(exp.get())->getValue();
                               });
                return values;
            }();
            return Value(Document{
                {this->getOpName(), ExpressionConstant::serializeConstant(options, Value(args))}});
        }
        return ExpressionNary::serialize(options);
    }
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

    void validateChildren() const override {
        uassert(28667,
                str::stream() << "Expression " << this->getOpName() << " takes at least " << MinArgs
                              << " arguments, and at most " << MaxArgs << ", but "
                              << this->_children.size() << " were passed in.",
                MinArgs <= this->_children.size() && this->_children.size() <= MaxArgs);
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

    void validateChildren() const override {
        uassert(16020,
                str::stream() << "Expression " << this->getOpName() << " takes exactly " << NArgs
                              << " arguments. " << this->_children.size() << " were passed in.",
                this->_children.size() == NArgs);
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
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    ExpressionFromAccumulator(ExpressionContext* const expCtx,
                              Expression::ExpressionVector&& children)
        : ExpressionVariadic<ExpressionFromAccumulator<AccumulatorState>>(expCtx,
                                                                          std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    Value evaluate(const Document& root, Variables* variables) const final;

    ExpressionNary::Associativity getAssociativity() const final {
        // Return false if a single argument is given to avoid a single array argument being treated
        // as an array instead of as a list of arguments.
        if (this->_children.size() == 1) {
            return ExpressionNary::Associativity::kNone;
        }
        return AccumulatorState(this->getExpressionContext()).getAssociativity();
    }

    bool isCommutative() const final {
        return AccumulatorState(this->getExpressionContext()).isCommutative();
    }

    const char* getOpName() const final {
        return AccumulatorState::kName.data();
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
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    const char* getOpName() const {
        return AccumulatorN::kName.data();
    }

    Value serialize(const SerializationOptions& options = {}) const override {
        MutableDocument md;
        AccumulatorN::serializeHelper(_n, _output, options, md);
        return Value(DOC(getOpName() << md.freeze()));
    }

    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const Expression* getN() const {
        return _n.get();
    }

    const Expression* getOutput() const {
        return _output.get();
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

    ~ExpressionSingleNumericArg() override = default;
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

    ~ExpressionTwoNumericArgs() override = default;
};

/**
 * Inherit from this class if your expression works with date types, and accepts either a single
 * argument which is a date, or an object {date: <date>, timezone: <string>}.
 */
class DateExpressionAcceptingTimeZone : public Expression {
public:
    ~DateExpressionAcceptingTimeZone() override {}

    /**
     * Always serializes to the full {date: <date arg>, timezone: <timezone arg>} format, leaving
     * off the timezone if not specified.
     */
    Value serialize(const SerializationOptions& options = {}) const final {
        auto timezone = _children[_kTimeZone] ? _children[_kTimeZone]->serialize(options) : Value();
        return Value(Document{{_opName,
                               Document{{"date", _children[_kDate]->serialize(options)},
                                        {"timezone", std::move(timezone)}}}});
    }

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;

    const Expression* getDate() const {
        return _children[_kDate].get();
    }
    const Expression* getTimeZone() const {
        return _children[_kTimeZone].get();
    }
    StringData getOpName() const {
        return _opName;
    }
    const boost::optional<TimeZone>& getParsedTimeZone() const {
        return _parsedTimeZone;
    }

protected:
    explicit DateExpressionAcceptingTimeZone(ExpressionContext* const expCtx,
                                             const StringData opName,
                                             boost::intrusive_ptr<Expression> date,
                                             boost::intrusive_ptr<Expression> timeZone)
        : Expression(expCtx, {date, timeZone}), _opName(opName) {}

private:
    // The position of the expression representing the date argument.
    static constexpr size_t _kDate = 0;

    // The position of the expression representing the timezone argument.
    static constexpr size_t _kTimeZone = 1;

    // The name of this expression, e.g. $week or $month.
    StringData _opName;

    // Pre-parsed timezone, if the above expression is a constant.
    boost::optional<TimeZone> _parsedTimeZone;
};

class ExpressionAbs final : public ExpressionSingleNumericArg<ExpressionAbs> {
public:
    explicit ExpressionAbs(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg<ExpressionAbs>(expCtx) {}
    explicit ExpressionAbs(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionSingleNumericArg<ExpressionAbs>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
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
    explicit ExpressionAdd(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionAdd>(expCtx) {}

    ExpressionAdd(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionAdd>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    // ExpressionAdd is left associative because it processes its operands by iterating
    // left-to-right through its _children vector, but the order of operations impacts the result
    // due to integer overflow, floating-point rounding and type promotion.
    Associativity getAssociativity() const final {
        return Associativity::kLeft;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    monotonic::State getMonotonicState(const FieldPath& sortedFieldPath) const final {
        return monotonic::combineExpressions(sortedFieldPath, getChildren());
    }
};


class ExpressionAllElementsTrue final : public ExpressionFixedArity<ExpressionAllElementsTrue, 1> {
public:
    explicit ExpressionAllElementsTrue(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionAllElementsTrue, 1>(expCtx) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }
    ExpressionAllElementsTrue(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionAllElementsTrue, 1>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    Associativity getAssociativity() const final {
        return Associativity::kFull;
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
        : ExpressionVariadic<ExpressionArray>(expCtx) {}

    ExpressionArray(ExpressionContext* const expCtx,
                    std::vector<boost::intrusive_ptr<Expression>>&& children)
        : ExpressionVariadic<ExpressionArray>(expCtx) {
        _children = std::move(children);
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    Value serialize(const SerializationOptions& options = {}) const final;

    static boost::intrusive_ptr<ExpressionArray> create(
        ExpressionContext* const expCtx, std::vector<boost::intrusive_ptr<Expression>>&& children) {
        return make_intrusive<ExpressionArray>(expCtx, std::move(children));
    }

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    bool selfAndChildrenAreConstant() const final;
};


class ExpressionArrayElemAt final : public ExpressionFixedArity<ExpressionArrayElemAt, 2> {
public:
    explicit ExpressionArrayElemAt(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionArrayElemAt, 2>(expCtx) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    ExpressionArrayElemAt(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionArrayElemAt, 2>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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

class ExpressionFirst final : public ExpressionFixedArity<ExpressionFirst, 1> {
public:
    explicit ExpressionFirst(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionFirst, 1>(expCtx) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    ExpressionFirst(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionFirst, 1>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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

class ExpressionLast final : public ExpressionFixedArity<ExpressionLast, 1> {
public:
    explicit ExpressionLast(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionLast, 1>(expCtx) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    ExpressionLast(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionLast, 1>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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

class ExpressionObjectToArray final : public ExpressionFixedArity<ExpressionObjectToArray, 1> {
public:
    explicit ExpressionObjectToArray(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionObjectToArray, 1>(expCtx) {}

    ExpressionObjectToArray(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionObjectToArray, 1>(expCtx, std::move(children)) {}

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
        : ExpressionFixedArity<ExpressionArrayToObject, 1>(expCtx) {}

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

    ExpressionBsonSize(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionBsonSize, 1>(expCtx, std::move(children)) {}

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

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    monotonic::State getMonotonicState(const FieldPath& sortedFieldPath) const final {
        return getChildren()[0]->getMonotonicState(sortedFieldPath);
    }
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

    Associativity getAssociativity() const final {
        return Associativity::kFull;
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

    Associativity getAssociativity() const final {
        return Associativity::kFull;
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

    ExpressionCond(ExpressionContext* const expCtx, ExpressionVector&& children)
        : Base(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;

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

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(const SerializationOptions& options = {}) const final;
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
     * Returns true if this expression has parameter 'format' specified, otherwise false.
     */
    bool isFormatSpecified() const {
        return static_cast<bool>(_children[_kFormat]);
    }

    /**
     * Returns true if this expression has parameter 'timezone' specified, otherwise false.
     */
    bool isTimezoneSpecified() const {
        return static_cast<bool>(_children[_kTimeZone]);
    }

    /**
     * Returns true if this expression has parameter 'onError' specified, otherwise false.
     */
    bool isOnErrorSpecified() const {
        return static_cast<bool>(_children[_kOnError]);
    }

    /**
     * Returns true if this expression has parameter 'onNull' specified, otherwise false.
     */
    bool isOnNullSpecified() const {
        return static_cast<bool>(_children[_kOnNull]);
    }

    const Expression* getDateString() const {
        return _children[_kDateString].get();
    }
    const Expression* getFormat() const {
        return _children[_kFormat].get();
    }
    const Expression* getTimeZone() const {
        return _children[_kTimeZone].get();
    }
    const Expression* getOnNull() const {
        return _children[_kOnNull].get();
    }
    const Expression* getOnError() const {
        return _children[_kOnError].get();
    }
    const boost::optional<TimeZone>& getParsedTimeZone() const {
        return _parsedTimeZone;
    }

private:
    static constexpr size_t _kDateString = 0;
    static constexpr size_t _kTimeZone = 1;
    static constexpr size_t _kFormat = 2;
    static constexpr size_t _kOnNull = 3;
    static constexpr size_t _kOnError = 4;

    // Pre-parsed timezone, if the above expression is a constant.
    boost::optional<TimeZone> _parsedTimeZone;

    template <typename H>
    friend class ExpressionHashVisitor;
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

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(const SerializationOptions& options = {}) const final;
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

    const Expression* getHour() const {
        return _children[_kHour].get();
    }
    const Expression* getMinute() const {
        return _children[_kMinute].get();
    }
    const Expression* getSecond() const {
        return _children[_kSecond].get();
    }
    const Expression* getMillisecond() const {
        return _children[_kMillisecond].get();
    }
    const Expression* getTimeZone() const {
        return _children[_kTimeZone].get();
    }
    const Expression* getYear() const {
        return _children[_kYear].get();
    }
    const Expression* getMonth() const {
        return _children[_kMonth].get();
    }
    const Expression* getDay() const {
        return _children[_kDay].get();
    }
    const Expression* getIsoWeekYear() const {
        return _children[_kIsoWeekYear].get();
    }
    const Expression* getIsoWeek() const {
        return _children[_kIsoWeek].get();
    }
    const Expression* getIsoDayOfWeek() const {
        return _children[_kIsoDayOfWeek].get();
    }
    const boost::optional<TimeZone>& getParsedTimeZone() const {
        return _parsedTimeZone;
    }

private:
    static constexpr size_t _kYear = 0;
    static constexpr size_t _kMonth = 1;
    static constexpr size_t _kDay = 2;
    static constexpr size_t _kHour = 3;
    static constexpr size_t _kMinute = 4;
    static constexpr size_t _kSecond = 5;
    static constexpr size_t _kMillisecond = 6;
    static constexpr size_t _kIsoWeekYear = 7;
    static constexpr size_t _kIsoWeek = 8;
    static constexpr size_t _kIsoDayOfWeek = 9;
    static constexpr size_t _kTimeZone = 10;

    // Pre-parsed timezone, if the above expression is a constant.
    boost::optional<TimeZone> _parsedTimeZone;

    template <typename H>
    friend class ExpressionHashVisitor;
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

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(const SerializationOptions& options = {}) const final;
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

    const Expression* getDate() const {
        return _children[_kDate].get();
    }
    const Expression* getTimeZone() const {
        return _children[_kTimeZone].get();
    }
    const Expression* getIso8601() const {
        return _children[_kIso8601].get();
    }
    const boost::optional<TimeZone>& getParsedTimeZone() const {
        return _parsedTimeZone;
    }

private:
    static constexpr size_t _kDate = 0;
    static constexpr size_t _kTimeZone = 1;
    static constexpr size_t _kIso8601 = 2;

    // Pre-parsed timezone, if the above expression is a constant.
    boost::optional<TimeZone> _parsedTimeZone;

    template <typename H>
    friend class ExpressionHashVisitor;
};

class ExpressionDateToString final : public Expression {
public:
    ExpressionDateToString(ExpressionContext* expCtx,
                           boost::intrusive_ptr<Expression> format,
                           boost::intrusive_ptr<Expression> date,
                           boost::intrusive_ptr<Expression> timeZone,
                           boost::intrusive_ptr<Expression> onNull);
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(const SerializationOptions& options = {}) const final;
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
     * Returns true if this expression has parameter 'format' specified, otherwise false.
     */
    bool isFormatSpecified() const {
        return static_cast<bool>(_children[_kFormat]);
    }

    /**
     * Returns true if this expression has parameter 'timezone' specified, otherwise false.
     */
    bool isTimezoneSpecified() const {
        return static_cast<bool>(_children[_kTimeZone]);
    }

    /**
     * Returns true if this expression has parameter 'onNull' specified, otherwise false.
     */
    bool isOnNullSpecified() const {
        return static_cast<bool>(_children[_kOnNull]);
    }

    const Expression* getFormat() const {
        return _children[_kFormat].get();
    }
    const Expression* getDate() const {
        return _children[_kDate].get();
    }
    const Expression* getTimeZone() const {
        return _children[_kTimeZone].get();
    }
    const Expression* getOnNull() const {
        return _children[_kOnNull].get();
    }
    const boost::optional<TimeZone>& getParsedTimeZone() const {
        return _parsedTimeZone;
    }

private:
    static constexpr size_t _kFormat = 0;
    static constexpr size_t _kDate = 1;
    static constexpr size_t _kTimeZone = 2;
    static constexpr size_t _kOnNull = 3;

    // Pre-parsed timezone, if the above expression is a constant.
    boost::optional<TimeZone> _parsedTimeZone;

    template <typename H>
    friend class ExpressionHashVisitor;
};

class ExpressionDayOfMonth final : public DateExpressionAcceptingTimeZone {
public:
    explicit ExpressionDayOfMonth(ExpressionContext* const expCtx,
                                  boost::intrusive_ptr<Expression> date,
                                  boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone(
              expCtx, "$dayOfMonth", std::move(date), std::move(timeZone)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionDayOfWeek final : public DateExpressionAcceptingTimeZone {
public:
    explicit ExpressionDayOfWeek(ExpressionContext* const expCtx,
                                 boost::intrusive_ptr<Expression> date,
                                 boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone(
              expCtx, "$dayOfWeek", std::move(date), std::move(timeZone)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionDayOfYear final : public DateExpressionAcceptingTimeZone {
public:
    explicit ExpressionDayOfYear(ExpressionContext* const expCtx,
                                 boost::intrusive_ptr<Expression> date,
                                 boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone(
              expCtx, "$dayOfYear", std::move(date), std::move(timeZone)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

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
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(const SerializationOptions& options = {}) const final;
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
        return static_cast<bool>(_children[_kTimeZone]);
    }

    /**
     * Returns true if this expression has parameter 'startOfWeek' specified, otherwise false.
     */
    bool isStartOfWeekSpecified() const {
        return static_cast<bool>(_children[_kStartOfWeek]);
    }

    const Expression* getStartDate() const {
        return _children[_kStartDate].get();
    }
    const Expression* getEndDate() const {
        return _children[_kEndDate].get();
    }
    const Expression* getUnit() const {
        return _children[_kUnit].get();
    }
    const Expression* getTimeZone() const {
        return _children[_kTimeZone].get();
    }
    const Expression* getStartOfWeek() const {
        return _children[_kStartOfWeek].get();
    }
    const boost::optional<TimeUnit>& getParsedUnit() const {
        return _parsedUnit;
    }
    const boost::optional<TimeZone>& getParsedTimeZone() const {
        return _parsedTimeZone;
    }
    const boost::optional<DayOfWeek>& getParsedStartOfWeek() const {
        return _parsedStartOfWeek;
    }

private:
    monotonic::State getMonotonicState(const FieldPath& sortedFieldPath) const final;

    // Starting time instant expression. Accepted types: Date_t, Timestamp, OID.
    static constexpr size_t _kStartDate = 0;

    // Ending time instant expression. Accepted types the same as for '_startDate'.
    static constexpr size_t _kEndDate = 1;

    // Length of time interval to measure the difference. Accepted type: std::string. Accepted
    // values: enumerators from TimeUnit enumeration.
    static constexpr size_t _kUnit = 2;

    // Timezone to use for the difference calculation. Accepted type: std::string. If not specified,
    // UTC is used.
    static constexpr size_t _kTimeZone = 3;

    // First/start day of the week to use for the date difference calculation when time unit is the
    // week. Accepted type: std::string. If not specified, "sunday" is used.
    static constexpr size_t _kStartOfWeek = 4;

    // Pre-parsed time unit, if the above expression is a constant.
    boost::optional<TimeUnit> _parsedUnit;

    // Pre-parsed timezone, if the above expression is a constant.
    boost::optional<TimeZone> _parsedTimeZone;

    // Pre-parsed start of week, if the above expression is a constant.
    boost::optional<DayOfWeek> _parsedStartOfWeek;

    template <typename H>
    friend class ExpressionHashVisitor;
};

class ExpressionDivide final : public ExpressionFixedArity<ExpressionDivide, 2> {
public:
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

    Value evaluate(const Document& root, Variables* variables) const final;
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

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root, Variables* variables) const override;
    Value serialize(const SerializationOptions& options = {}) const final;

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

    /**
     * Checks if any key of 'renameList' map is a prefix of this ExpressionFieldPath's path. It
     * would mean that this ExpressionFieldPath is renameable by 'renameList' if so.
     */
    bool isRenameableByAnyPrefixNameIn(const StringMap<std::string>& renameList) const;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

protected:
    ExpressionFieldPath(ExpressionContext* expCtx,
                        const std::string& fieldPath,
                        Variables::Id variable);


private:
    monotonic::State getMonotonicState(const FieldPath& sortedFieldPath) const final;

    const FieldPath _fieldPath;
    const Variables::Id _variable;
};

class ExpressionFilter final : public Expression {
public:
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(const SerializationOptions& options = {}) const final;
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

    const Expression* getInput() const {
        return _children[_kInput].get();
    }

    const Expression* getCond() const {
        return _children[_kCond].get();
    }

    const boost::optional<size_t>& getLimit() const {
        return _limit;
    }

private:
    // The array to iterate over.
    static constexpr size_t _kInput = 0;
    // The expression determining whether each element should be present in the result array.
    static constexpr size_t _kCond = 1;

    // The name of the variable to set to each element in the array.
    std::string _varName;
    // The id of the variable to set.
    Variables::Id _varId;
    // The optional expression determining how many elements should be present in the result array.
    boost::optional<size_t> _limit;

    template <typename H>
    friend class ExpressionHashVisitor;
};


class ExpressionFloor final : public ExpressionSingleNumericArg<ExpressionFloor> {
public:
    explicit ExpressionFloor(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg<ExpressionFloor>(expCtx) {}
    explicit ExpressionFloor(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionSingleNumericArg<ExpressionFloor>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    monotonic::State getMonotonicState(const FieldPath& sortedFieldPath) const final {
        return getChildren()[0]->getMonotonicState(sortedFieldPath);
    }
};


class ExpressionHour final : public DateExpressionAcceptingTimeZone {
public:
    explicit ExpressionHour(ExpressionContext* const expCtx,
                            boost::intrusive_ptr<Expression> date,
                            boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone(expCtx, "$hour", std::move(date), std::move(timeZone)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

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

    ExpressionIfNull(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionIfNull>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;
    void validateChildren() const final;
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;

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
        : ExpressionFixedArity<ExpressionIn, 2>(expCtx) {}

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
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    ExpressionIndexOfArray(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionRangedArity<ExpressionIndexOfArray, 2, 4>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    Value evaluate(const Document& root, Variables* variables) const override;
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const boost::optional<ValueUnorderedMap<std::vector<int>>>& getParsedIndexMap() const {
        return _parsedIndexMap;
    }

private:
    // Maps the values in the array to the positions at which they occur. We need to remember
    // the positions so that we can verify they are in the appropriate range.
    boost::optional<ValueUnorderedMap<std::vector<int>>> _parsedIndexMap;
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
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(const SerializationOptions& options = {}) const final;
    Value evaluate(const Document& root, Variables* variables) const final;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    struct NameAndExpression {
        std::string name;
        boost::intrusive_ptr<Expression>& expression;
    };

    typedef std::map<Variables::Id, NameAndExpression> VariableMap;

    // Callback function to create the "in" expression with the let variables defined.
    using CreateInExpr = std::function<boost::intrusive_ptr<Expression>(
        ExpressionContext*, const VariablesParseState&)>;

    // Internal API to create a ExpressionLet with the specified variables.
    static boost::intrusive_ptr<Expression> create(
        ExpressionContext* expCtx,
        std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> letVariables,
        const VariablesParseState& vpsIn,
        CreateInExpr createInFunc);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const std::vector<Variables::Id>& getOrderedVariableIds() const {
        return _orderedVariableIds;
    }

    const VariableMap& getVariableMap() const {
        return _variables;
    }

    const Expression* getSubExpression() const {
        return _children[_kSubExpression].get();
    }

private:
    ExpressionLet(ExpressionContext* expCtx,
                  VariableMap&& vars,
                  std::vector<boost::intrusive_ptr<Expression>> children,
                  std::vector<Variables::Id> orderedVariableIds);

    // Index of the last element in the '_children' list.
    const size_t _kSubExpression;

    VariableMap _variables;

    // These ids are ordered to match their corresponding _children expressions.
    std::vector<Variables::Id> _orderedVariableIds;
};

class ExpressionLn final : public ExpressionSingleNumericArg<ExpressionLn> {
public:
    explicit ExpressionLn(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg<ExpressionLn>(expCtx) {}
    ExpressionLn(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionSingleNumericArg<ExpressionLn>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
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
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }
    ExpressionLog(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionLog, 2>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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

    Value evaluate(const Document& root, Variables* variables) const final;
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
                               ServerZerosEncryptionToken zerosToken);

    Value serialize(const SerializationOptions& options = {}) const final;

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const auto& getEncryptedPredicateEvaluator() const {
        return _evaluatorV2;
    }

private:
    EncryptedPredicateEvaluatorV2 _evaluatorV2;
};

class ExpressionInternalFLEBetween final : public Expression {
public:
    ExpressionInternalFLEBetween(ExpressionContext* expCtx,
                                 boost::intrusive_ptr<Expression> field,
                                 std::vector<ServerZerosEncryptionToken> serverTokens);

    Value serialize(const SerializationOptions& options = {}) const final;

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const auto& getEncryptedPredicateEvaluator() const {
        return _evaluatorV2;
    }

private:
    EncryptedPredicateEvaluatorV2 _evaluatorV2;
};

class ExpressionMap final : public Expression {
public:
    ExpressionMap(
        ExpressionContext* expCtx,
        const std::string& varName,              // name of variable to set
        Variables::Id varId,                     // id of variable to set
        boost::intrusive_ptr<Expression> input,  // yields array to iterate
        boost::intrusive_ptr<Expression> each);  // yields results to be added to output array

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(const SerializationOptions& options = {}) const final;
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

    const Expression* getInput() const {
        return _children[_kInput].get();
    }

    const Expression* getEach() const {
        return _children[_kEach].get();
    }

    Variables::Id getVarId() const {
        return _varId;
    }

private:
    static constexpr size_t _kInput = 0;
    static constexpr size_t _kEach = 1;
    std::string _varName;
    Variables::Id _varId;

    template <typename H>
    friend class ExpressionHashVisitor;
};

class ExpressionMeta final : public Expression {
public:
    ExpressionMeta(ExpressionContext* expCtx, DocumentMetadataFields::MetaType metaType);

    Value serialize(const SerializationOptions& options = {}) const final;
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

private:
    // An error message prefix used for parsing errors.
    static constexpr auto kParseErrPrefix = "Unsupported $meta field: ";

    // Used during $meta expression parsing.
    struct ParseMetaTypeResult {
        // The meta enum type.
        DocumentMetadataFields::MetaType metaType;
        // The string name used for the meta type.
        StringData typeName;
        // An optional path, for use cases like $meta: "stream.window".
        boost::optional<StringData> path;
    };

    /**
     * Asserts that if the API version is strict, that the requested metadata field is compatible
     * with it.
     */
    static void _assertMetaFieldCompatibleWithStrictAPI(ExpressionContext* expCtx,
                                                        DocumentMetadataFields::MetaType type);
    /**
     * Asserts that 'featureFlagRankFusionFull' feature flag is enabled, if the
     * requested metadata field requires it.
     */
    static void _assertMetaFieldCompatibleWithHybridScoringFeatureFlag(
        ExpressionContext* expCtx, DocumentMetadataFields::MetaType type);

    /**
     * Asserts that the 'featureFlagStreams' is enabled, depending on the parsed meta type and
     * optional field path specified.
     */
    static void _assertMetaFieldCompatibleWithStreamsFeatureFlag(
        ExpressionContext* expCtx,
        DocumentMetadataFields::MetaType type,
        StringData typeName,
        boost::optional<StringData> optionalPath);

    /**
     * Rewrites { $meta: "stream.path" } as
     * { $let: { in: "$$stream.path", vars: {stream: {$meta: "stream"}} } }
     */
    static boost::intrusive_ptr<Expression> _rewriteAsLet(ExpressionContext* expCtx,
                                                          DocumentMetadataFields::MetaType type,
                                                          StringData typeName,
                                                          StringData path,
                                                          const VariablesParseState& vpsIn);

    /**
     * Helper utility to parse a meta type and optional path from the user supplied typeName.
     */
    static ParseMetaTypeResult _parseMetaType(ExpressionContext* expCtx, StringData typeName);

    DocumentMetadataFields::MetaType _metaType;
};

/**
 * A 'flavor' of {$meta: "sortKey"} which is intended for efficient comparisons internally. This is
 * in contrast to the public facing expression which is meant to return a user-comprehensible sort
 * key, represented as an array. That expression will materialize a new array. Constructing this
 * array is not necessary for just making internal comparisons.
 */
class ExpressionInternalRawSortKey final : public Expression {
public:
    static constexpr StringData kName = "$_internalSortKey"_sd;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext*,
                                                  BSONElement,
                                                  const VariablesParseState&);

    ExpressionInternalRawSortKey(ExpressionContext* expCtx) : Expression(expCtx) {}

    Value serialize(const SerializationOptions& options = {}) const final;
    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionMillisecond final : public DateExpressionAcceptingTimeZone {
public:
    explicit ExpressionMillisecond(ExpressionContext* const expCtx,
                                   boost::intrusive_ptr<Expression> date,
                                   boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone(
              expCtx, "$millisecond", std::move(date), std::move(timeZone)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionMinute final : public DateExpressionAcceptingTimeZone {
public:
    explicit ExpressionMinute(ExpressionContext* const expCtx,
                              boost::intrusive_ptr<Expression> date,
                              boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone(expCtx, "$minute", std::move(date), std::move(timeZone)) {
    }

    Value evaluate(const Document& root, Variables* variables) const final;

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
    explicit ExpressionMultiply(ExpressionContext* const expCtx)
        : ExpressionVariadic<ExpressionMultiply>(expCtx) {}
    ExpressionMultiply(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionMultiply>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    // ExpressionMultiply is left associative because it processes its operands by iterating
    // left-to-right through its _children vector, but the order of operations impacts the result
    // due to integer overflow, floating-point rounding and type promotion.
    Associativity getAssociativity() const final {
        return Associativity::kLeft;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionMonth final : public DateExpressionAcceptingTimeZone {
public:
    explicit ExpressionMonth(ExpressionContext* const expCtx,
                             boost::intrusive_ptr<Expression> date,
                             boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone(expCtx, "$month", std::move(date), std::move(timeZone)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

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
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root, Variables* variables) const final;
    Value serialize(const SerializationOptions& options = {}) const final;

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

    bool selfAndChildrenAreConstant() const final;

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

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    Associativity getAssociativity() const final {
        return Associativity::kFull;
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
        : ExpressionFixedArity<ExpressionPow, 2>(expCtx) {}
    ExpressionPow(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionPow, 2>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    static boost::intrusive_ptr<Expression> create(ExpressionContext* expCtx,
                                                   Value base,
                                                   Value exp);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionRange final : public ExpressionRangedArity<ExpressionRange, 2, 3> {
public:
    explicit ExpressionRange(ExpressionContext* const expCtx)
        : ExpressionRangedArity<ExpressionRange, 2, 3>(expCtx) {}

    ExpressionRange(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionRangedArity<ExpressionRange, 2, 3>(expCtx, std::move(children)) {}

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
          _thisVar(thisVar),
          _valueVar(valueVar) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);
    Value serialize(const SerializationOptions& options = {}) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const Expression* getInput() const {
        return _children[_kInput].get();
    }

    const Expression* getInitial() const {
        return _children[_kInitial].get();
    }

    const Expression* getIn() const {
        return _children[_kIn].get();
    }

    Variables::Id getThisVar() const {
        return _thisVar;
    }

    Variables::Id getValueVar() const {
        return _valueVar;
    }

    size_t getAccumulatedValueDepthCheckInterval() const {
        return _accumulatedValueDepthCheckInterval;
    }

private:
    static constexpr size_t _kInput = 0;
    static constexpr size_t _kInitial = 1;
    static constexpr size_t _kIn = 2;

    Variables::Id _thisVar;
    Variables::Id _valueVar;

    const size_t _accumulatedValueDepthCheckInterval =
        gInternalReduceAccumulatedValueDepthCheckInterval.load();

    template <typename H>
    friend class ExpressionHashVisitor;
};


class ExpressionReplaceBase : public Expression {
public:
    ExpressionReplaceBase(ExpressionContext* const expCtx,
                          boost::intrusive_ptr<Expression> input,
                          boost::intrusive_ptr<Expression> find,
                          boost::intrusive_ptr<Expression> replacement)
        : Expression(expCtx, {std::move(input), std::move(find), std::move(replacement)}) {}

    virtual const char* getOpName() const = 0;
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(const SerializationOptions& options = {}) const final;

    const Expression* getInput() const {
        return _children[_kInput].get();
    }

    const Expression* getFind() const {
        return _children[_kFind].get();
    }

    const Expression* getReplacement() const {
        return _children[_kReplacement].get();
    }

private:
    // These are owned by this->Expression::_children. They are references to intrusive_ptr instead
    // of direct references to Expression because we need to be able to replace each child in
    // optimize() without invalidating the references.
    static constexpr size_t _kInput = 0;
    static constexpr size_t _kFind = 1;
    static constexpr size_t _kReplacement = 2;
};


class ExpressionReplaceOne final : public ExpressionReplaceBase {
public:
    ExpressionReplaceOne(ExpressionContext* const expCtx,
                         boost::intrusive_ptr<Expression> input,
                         boost::intrusive_ptr<Expression> find,
                         boost::intrusive_ptr<Expression> replacement)
        : ExpressionReplaceBase(expCtx, input, find, replacement) {
        // TODO(SERVER-108244): Support $replaceOne with regex in SBE.
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

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

    Value evaluate(const Document& root, Variables* variables) const final;
};

class ExpressionReplaceAll final : public ExpressionReplaceBase {
public:
    ExpressionReplaceAll(ExpressionContext* const expCtx,
                         boost::intrusive_ptr<Expression> input,
                         boost::intrusive_ptr<Expression> find,
                         boost::intrusive_ptr<Expression> replacement)
        : ExpressionReplaceBase(expCtx, input, find, replacement) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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

    Value evaluate(const Document& root, Variables* variables) const final;
};

class ExpressionSecond final : public DateExpressionAcceptingTimeZone {
public:
    ExpressionSecond(ExpressionContext* const expCtx,
                     boost::intrusive_ptr<Expression> date,
                     boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone(expCtx, "$second", std::move(date), std::move(timeZone)) {
    }

    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionSetDifference final : public ExpressionFixedArity<ExpressionSetDifference, 2> {
public:
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
    ExpressionSetEquals(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionSetEquals>(expCtx, std::move(children)) {
        if (_children.size() > kMaxArgumentCountForSwitchAndSetExprForSbe &&
            !feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
            expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
        }
    }

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() override;
    Value evaluate(const Document& root, Variables* variables) const override;
    const char* getOpName() const final;
    void validateChildren() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const boost::optional<std::pair<size_t, ValueFlatUnorderedSet>>& getCachedConstant() const {
        return _cachedConstant;
    }

private:
    // The first element in the pair represent the position on the constant in the '_children'
    // array. The second element is the constant set.
    boost::optional<std::pair<size_t, ValueFlatUnorderedSet>> _cachedConstant;

    template <typename H>
    friend class ExpressionHashVisitor;
};


class ExpressionSetIntersection final : public ExpressionVariadic<ExpressionSetIntersection> {
public:
    ExpressionSetIntersection(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionSetIntersection>(expCtx, std::move(children)) {
        if (_children.size() > kMaxArgumentCountForSwitchAndSetExprForSbe &&
            !feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
            expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
        }
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    Associativity getAssociativity() const final {
        return Associativity::kFull;
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
    ExpressionSetIsSubset(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionSetIsSubset, 2>(expCtx, std::move(children)) {}

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() override;
    Value evaluate(const Document& root, Variables* variables) const override;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const boost::optional<ValueFlatUnorderedSet>& getCachedRhsSet() const {
        return _cachedRhsSet;
    }

private:
    /**
     * This class member handles the case where the RHS set is constant.
     *
     * Since it is constant we can construct the hashset once which makes the runtime performance
     * effectively constant with respect to the size of RHS. Large, constant RHS is expected to be a
     * major use case for $redact and this has been verified to improve performance significantly.
     */
    boost::optional<ValueFlatUnorderedSet> _cachedRhsSet;
};

class ExpressionSetUnion final : public ExpressionVariadic<ExpressionSetUnion> {
public:
    ExpressionSetUnion(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionVariadic<ExpressionSetUnion>(expCtx, std::move(children)) {
        if (_children.size() > kMaxArgumentCountForSwitchAndSetExprForSbe &&
            !feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
            expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
        }
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    Associativity getAssociativity() const final {
        return Associativity::kFull;
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

class ExpressionVectorSimilarity : public Expression {
public:
    ExpressionVectorSimilarity(ExpressionContext* const expCtx,
                               bool score,
                               std::vector<boost::intrusive_ptr<Expression>> children)
        : Expression(expCtx, std::move(children)), _score(score) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    virtual const char* getOpName() const = 0;
    Value evaluate(const Document& root, Variables* variables) const override = 0;

    Value serialize(const SerializationOptions& options = {}) const final;

    bool isScore() const {
        return _score;
    }

protected:
    bool _score;
    static auto _parseInternal(ExpressionContext* expCtx,
                               BSONElement expr,
                               const VariablesParseState& vps,
                               const std::string& similarityName);
};

class ExpressionSimilarityDotProduct final : public ExpressionVectorSimilarity {
public:
    static constexpr auto kName = "$similarityDotProduct"_sd;

    ExpressionSimilarityDotProduct(ExpressionContext* const expCtx,
                                   bool score,
                                   std::vector<boost::intrusive_ptr<Expression>> children)
        : ExpressionVectorSimilarity(expCtx, score, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const override;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const char* getOpName() const override {
        return kName.data();
    }
};

class ExpressionSimilarityCosine final : public ExpressionVectorSimilarity {
public:
    static constexpr auto kName = "$similarityCosine"_sd;

    ExpressionSimilarityCosine(ExpressionContext* const expCtx,
                               bool score,
                               std::vector<boost::intrusive_ptr<Expression>> children)
        : ExpressionVectorSimilarity(expCtx, score, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const override;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const char* getOpName() const override {
        return kName.data();
    }
};

class ExpressionSimilarityEuclidean final : public ExpressionVectorSimilarity {
public:
    static constexpr auto kName = "$similarityEuclidean"_sd;

    ExpressionSimilarityEuclidean(ExpressionContext* const expCtx,
                                  bool score,
                                  std::vector<boost::intrusive_ptr<Expression>> children)
        : ExpressionVectorSimilarity(expCtx, score, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const override;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const char* getOpName() const override {
        return kName.data();
    }
};

class ExpressionSize final : public ExpressionFixedArity<ExpressionSize, 1> {
public:
    explicit ExpressionSize(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionSize, 1>(expCtx) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    ExpressionSize(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionSize, 1>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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

    ExpressionReverseArray(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionReverseArray, 1>(expCtx, std::move(children)) {}

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
        : Expression(expCtx, {std::move(input)}), _sortBy(sortBy) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);
    Value serialize(const SerializationOptions& options = {}) const final;

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

    const Expression* getInput() const {
        return _children[_kInput].get();
    }

    const PatternValueCmp& getSortBy() const {
        return _sortBy;
    }

private:
    static constexpr size_t _kInput = 0;
    PatternValueCmp _sortBy;
};

class ExpressionSlice final : public ExpressionRangedArity<ExpressionSlice, 2, 3> {
public:
    explicit ExpressionSlice(ExpressionContext* const expCtx)
        : ExpressionRangedArity<ExpressionSlice, 2, 3>(expCtx) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }
    ExpressionSlice(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionRangedArity<ExpressionSlice, 2, 3>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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

class ExpressionSigmoid final {
public:
    static boost::intrusive_ptr<Expression> parseExpressionSigmoid(ExpressionContext* expCtx,
                                                                   BSONElement expr,
                                                                   const VariablesParseState& vps);
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
        : ExpressionFixedArity<ExpressionInternalFindAllValuesAtPath, 1>(expCtx) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    explicit ExpressionInternalFindAllValuesAtPath(ExpressionContext* expCtx,
                                                   ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionInternalFindAllValuesAtPath, 1>(expCtx,
                                                                         std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }
    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const override {
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
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() override {
        return this;
    }

    FieldPath getFieldPath() const {
        auto inputConstExpression = dynamic_cast<ExpressionConstant*>(_children[0].get());
        uassert(5511201,
                "Expected const expression as argument to _internalFindAllValuesAtPath",
                inputConstExpression);
        auto constVal = inputConstExpression->getValue();

        uassert(9567004,
                str::stream() << getOpName() << " requires argument to be a string",
                constVal.getType() == BSONType::string);

        return FieldPath(constVal.getString());
    }
};

class ExpressionRound final : public ExpressionRangedArity<ExpressionRound, 1, 2> {
public:
    explicit ExpressionRound(ExpressionContext* const expCtx)
        : ExpressionRangedArity<ExpressionRound, 1, 2>(expCtx) {}
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
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }
    ExpressionSplit(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionSplit, 2>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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

    Value evaluate(const Document& root, Variables* variables) const final;
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
        : ExpressionFixedArity<ExpressionStrcasecmp, 2>(expCtx) {}
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
        : ExpressionFixedArity<ExpressionSubstrBytes, 3>(expCtx) {}
    ExpressionSubstrBytes(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionSubstrBytes, 3>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const override;

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
        : ExpressionFixedArity<ExpressionSubstrCP, 3>(expCtx) {}
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
        : ExpressionFixedArity<ExpressionStrLenBytes, 1>(expCtx) {}

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
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    ExpressionBinarySize(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionBinarySize, 1>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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
        : ExpressionFixedArity<ExpressionStrLenCP, 1>(expCtx) {}
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
    explicit ExpressionSubtract(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionSubtract, 2>(expCtx) {}
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

private:
    monotonic::State getMonotonicState(const FieldPath& sortedFieldPath) const final;
};


class ExpressionSwitch final : public Expression {
public:
    using ExpressionPair =
        std::pair<boost::intrusive_ptr<Expression>&, boost::intrusive_ptr<Expression>&>;

    ExpressionSwitch(ExpressionContext* const expCtx,
                     std::vector<boost::intrusive_ptr<Expression>> children)
        : Expression(expCtx, std::move(children)) {
        uassert(40068, "$switch requires at least one branch", numBranches() >= 1);
        if (_children.size() > kMaxArgumentCountForSwitchAndSetExprForSbe &&
            !feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
            expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
        }
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);
    Value serialize(const SerializationOptions& options = {}) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    /**
     * Returns the number of cases in the switch expression. Each branch is made up of two
     * expressions ('case' and 'then').
     */
    int numBranches() const {
        return _children.size() / 2;
    }

    /**
     * Returns a pair of expression pointers representing the 'case' and 'then' expressions for the
     * i-th branch of the switch.
     */
    std::pair<const Expression*, const Expression*> getBranch(int i) const {
        invariant(i >= 0);
        invariant(i < numBranches());
        return {_children[i * 2].get(), _children[i * 2 + 1].get()};
    }

    /**
     * Returns the 'default' expression, or nullptr if there is no 'default'.
     */
    const Expression* defaultExpr() const {
        return _children.back().get();
    }

private:
    // Helper for 'optimize()'. Deletes the 'case' and 'then' children associated with the i-th
    // branch of the switch.
    void deleteBranch(int i);
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
          _name(std::string{name}) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);
    Value serialize(const SerializationOptions& options = {}) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    /* Returns "trim"/"ltrim"/"rtrim" based on the expression name without the $ sign. */
    std::string getTrimTypeString() const {
        return _name.substr(1);
    }

    bool hasCharactersExpr() const {
        return _children[_kCharacters] != nullptr;
    }

    const Expression* getInput() const {
        return _children[_kInput].get();
    }

    const Expression* getCharacters() const {
        return _children[_kCharacters].get();
    }

    const std::string& getName() const {
        return _name;
    }

    TrimType getTrimType() const {
        return _trimType;
    }

private:
    static constexpr size_t _kInput = 0;
    static constexpr size_t _kCharacters = 1;  // Optional, null if not specified.

    TrimType _trimType;
    std::string _name;  // "$trim", "$ltrim", or "$rtrim".
};


class ExpressionTrunc final : public ExpressionRangedArity<ExpressionTrunc, 1, 2> {
public:
    explicit ExpressionTrunc(ExpressionContext* const expCtx)
        : ExpressionRangedArity<ExpressionTrunc, 1, 2>(expCtx) {}
    ExpressionTrunc(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionRangedArity<ExpressionTrunc, 1, 2>(expCtx, std::move(children)) {}

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
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    ExpressionType(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionType, 1>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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

class ExpressionSubtype final : public ExpressionFixedArity<ExpressionSubtype, 1> {
public:
    explicit ExpressionSubtype(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionSubtype, 1>(expCtx) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    ExpressionSubtype(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionSubtype, 1>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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

class ExpressionIsNumber final : public ExpressionFixedArity<ExpressionIsNumber, 1> {
public:
    explicit ExpressionIsNumber(ExpressionContext* const expCtx)
        : ExpressionFixedArity<ExpressionIsNumber, 1>(expCtx) {}

    ExpressionIsNumber(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionFixedArity<ExpressionIsNumber, 1>(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;
    const char* getOpName() const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionWeek final : public DateExpressionAcceptingTimeZone {
public:
    ExpressionWeek(ExpressionContext* const expCtx,
                   boost::intrusive_ptr<Expression> date,
                   boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone(expCtx, "$week", std::move(date), std::move(timeZone)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionIsoWeekYear final : public DateExpressionAcceptingTimeZone {
public:
    ExpressionIsoWeekYear(ExpressionContext* const expCtx,
                          boost::intrusive_ptr<Expression> date,
                          boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone(
              expCtx, "$isoWeekYear", std::move(date), std::move(timeZone)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionIsoDayOfWeek final : public DateExpressionAcceptingTimeZone {
public:
    ExpressionIsoDayOfWeek(ExpressionContext* const expCtx,
                           boost::intrusive_ptr<Expression> date,
                           boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone(
              expCtx, "$isoDayOfWeek", std::move(date), std::move(timeZone)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionIsoWeek final : public DateExpressionAcceptingTimeZone {
public:
    ExpressionIsoWeek(ExpressionContext* const expCtx,
                      boost::intrusive_ptr<Expression> date,
                      boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone(
              expCtx, "$isoWeek", std::move(date), std::move(timeZone)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


class ExpressionYear final : public DateExpressionAcceptingTimeZone {
public:
    ExpressionYear(ExpressionContext* const expCtx,
                   boost::intrusive_ptr<Expression> date,
                   boost::intrusive_ptr<Expression> timeZone = nullptr)
        : DateExpressionAcceptingTimeZone(expCtx, "$year", std::move(date), std::move(timeZone)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

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
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    Value evaluate(const Document& root, Variables* variables) const final;
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);
    Value serialize(const SerializationOptions& options = {}) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    bool getUseLongestLength() const {
        return _useLongestLength;
    }

    const std::vector<std::reference_wrapper<boost::intrusive_ptr<Expression>>>& getInputs() const {
        return _inputs;
    }

    const std::vector<std::reference_wrapper<boost::intrusive_ptr<Expression>>>& getDefaults()
        const {
        return _defaults;
    }

private:
    bool _useLongestLength;
    std::vector<std::reference_wrapper<boost::intrusive_ptr<Expression>>> _inputs;
    std::vector<std::reference_wrapper<boost::intrusive_ptr<Expression>>> _defaults;
};

enum class ConversionBase {
    kBinary = 2,
    kOctal = 8,
    kDecimal = 10,
    kHexadecimal = 16,
};

static bool isValidConversionBase(int base) {
    switch (base) {
        case (stdx::to_underlying(ConversionBase::kBinary)):
        case (stdx::to_underlying(ConversionBase::kOctal)):
        case (stdx::to_underlying(ConversionBase::kDecimal)):
        case (stdx::to_underlying(ConversionBase::kHexadecimal)):
            return true;
        default:
            return false;
    }
}

// This enum is not compatible with the QUERY_UTIL_NAMED_ENUM_DEFINE util since the "auto" type
// conflicts with the C++ keyword "auto". Instead, we manually define the enum and the
// toStringData function below.
enum class BinDataFormat {
    kAuto,
    kBase64,
    kBase64Url,
    kHex,
    kUtf8,
    kUuid,
};

static StringData toStringData(BinDataFormat type) {
    switch (type) {
        case BinDataFormat::kAuto:
            return "auto"_sd;
        case BinDataFormat::kBase64:
            return "base64"_sd;
        case BinDataFormat::kBase64Url:
            return "base64url"_sd;
        case BinDataFormat::kHex:
            return "hex"_sd;
        case BinDataFormat::kUtf8:
            return "utf8"_sd;
        case BinDataFormat::kUuid:
            return "uuid"_sd;
        default:
            MONGO_UNREACHABLE_TASSERT(4341123);
    }
}

/**
 * Used in $convert when converting between BinData and numeric types. Represents the endianness in
 * which we interpret or write the BinData.
 */
#define CONVERT_BYTE_ORDER_TYPE(F) \
    F(little)                      \
    F(big)
QUERY_UTIL_NAMED_ENUM_DEFINE(ConvertByteOrderType, CONVERT_BYTE_ORDER_TYPE);
#undef CONVERT_BYTE_ORDER_TYPE

class ExpressionConvert final : public Expression {
public:
    struct ConvertTargetTypeInfo {
        BSONType type;
        Value subtype;

        static const Value defaultSubtypeVal;
        static boost::optional<ConvertTargetTypeInfo> parse(Value value);
    };

    ExpressionConvert(ExpressionContext* expCtx,
                      boost::intrusive_ptr<Expression> input,
                      boost::intrusive_ptr<Expression> to,
                      boost::intrusive_ptr<Expression> base,
                      boost::intrusive_ptr<Expression> format,
                      boost::intrusive_ptr<Expression> onError,
                      boost::intrusive_ptr<Expression> onNull,
                      boost::intrusive_ptr<Expression> byteOrder,
                      bool allowBinDataConvert,
                      bool allowBinDataConvertNumeric);
    /**
     * Creates a $convert expression converting from 'input' to the type given by 'toType'. Leaves
     * 'onNull' and 'onError' unspecified.
     */
    static boost::intrusive_ptr<Expression> create(
        ExpressionContext*,
        boost::intrusive_ptr<Expression> input,
        BSONType toType,
        boost::optional<BinDataFormat> format = boost::none,
        boost::optional<BinDataType> toSubtype = boost::none,
        boost::optional<ConvertByteOrderType> byteOrder = boost::none);

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);

    Value evaluate(const Document& root, Variables* variables) const final;
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(const SerializationOptions& options = {}) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    static bool checkBinDataConvertAllowed();
    static bool checkBinDataConvertNumericAllowed(const ExpressionContext* expCtx);

    const Expression* getInput() const {
        return _children[_kInput].get();
    }
    const Expression* getTo() const {
        return _children[_kTo].get();
    }
    const Expression* getBase() const {
        return _children[_kBase].get();
    }
    const Expression* getFormat() const {
        return _children[_kFormat].get();
    }
    const Expression* getOnError() const {
        return _children[_kOnError].get();
    }
    const Expression* getOnNull() const {
        return _children[_kOnNull].get();
    }
    const Expression* getByteOrder() const {
        return _children[_kByteOrder].get();
    }
    bool getAllowBinDataConvert() const {
        return _allowBinDataConvert;
    }
    bool getAllowBinDataConvertNumeric() const {
        return _allowBinDataConvertNumeric;
    }

private:
    static BSONType computeTargetType(Value typeName);

    // Support for BinData $convert is FCV gated. These feature flags are checked once during
    // parsing to avoid having to acquire FCV snapshot for every document during evaluation.
    const bool _allowBinDataConvert;
    const bool _allowBinDataConvertNumeric;

    static constexpr size_t _kInput = 0;
    static constexpr size_t _kTo = 1;
    static constexpr size_t _kBase = 2;
    static constexpr size_t _kFormat = 3;
    static constexpr size_t _kOnError = 4;
    static constexpr size_t _kOnNull = 5;
    static constexpr size_t _kByteOrder = 6;
};

class ExpressionRegex : public Expression {
public:
    /**
     * Struct to hold precompiled regex, validated pattern & options strings and number of captures
     * when the expression has constant values for their 'regex' and 'options' fields
     */
    struct PrecompiledRegex {
        boost::optional<std::string> pattern;
        boost::optional<std::string> options;
        std::shared_ptr<pcre::Regex> pcrePtr;
        int numCaptures = 0;
    };

    /**
     * Optimizes '$regex*' expressions. If the expression has constant 'regex' and 'options' fields,
     * then it can be optimized. Stores the optimized regex in '_precompiledRegex'
     * so that it can be reused during expression evaluation.
     */
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() override;

    bool hasConstantRegex() const {
        return _precompiledRegex.has_value();
    }

    bool hasOptions() const {
        return (_children[_kOptions].get() != nullptr);
    }

    /**
     * If pattern or options are not constants, returns boost::none. Otherwise, return value
     * contains regex pattern and options if they are not null.
     */
    boost::optional<std::pair<boost::optional<std::string>, std::string>>
    getConstantPatternAndOptions() const;

    Value serialize(const SerializationOptions& options = {}) const override;

    const std::string& getOpName() const {
        return _opName;
    }

    const Expression* getInput() const {
        return _children[_kInput].get();
    }

    const Expression* getRegex() const {
        return _children[_kRegex].get();
    }

    const Expression* getOptions() const {
        return _children[_kOptions].get();
    }

    const auto& getPreCompiledRegex() const {
        return _precompiledRegex;
    }

    ExpressionRegex(ExpressionContext* const expCtx,
                    boost::intrusive_ptr<Expression> input,
                    boost::intrusive_ptr<Expression> regex,
                    boost::intrusive_ptr<Expression> options,
                    const StringData opName)
        : Expression(expCtx, {std::move(input), std::move(regex), std::move(options)}),
          _opName(opName) {}

private:
    /**
     * Expressions which, when evaluated for a given document, produce the the regex pattern, the
     * regex option flags, and the input text to which the regex should be applied.
     */
    static constexpr size_t _kInput = 0;
    static constexpr size_t _kRegex = 1;
    static constexpr size_t _kOptions = 2;

    /**
     * This variable will be set when the $regex* expressions have constant values for their 'regex'
     * and 'options' fields, allowing us to pre-compile the regex and re-use it across the
     * Expression's lifetime.
     */
    boost::optional<PrecompiledRegex> _precompiledRegex;

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
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement exprElement,
                                                  const VariablesParseState& vps);

    Value serialize(const SerializationOptions& options = {}) const final;

    Value evaluate(const Document& root, Variables* variables) const final;

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;

    const char* getOpName() const;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    explicit ExpressionRandom(ExpressionContext* expCtx);
};

/**
 * Returns current wall clock time.
 */
class ExpressionCurrentDate final : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement exprElement,
                                                  const VariablesParseState& vps);

    Value serialize(const SerializationOptions& options = {}) const final;

    Value evaluate(const Document& root, Variables* variables) const final;

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;

    const char* getOpName() const;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    explicit ExpressionCurrentDate(ExpressionContext* expCtx);
};

class ExpressionToHashedIndexKey : public Expression {
public:
    ExpressionToHashedIndexKey(ExpressionContext* const expCtx,
                               boost::intrusive_ptr<Expression> inputExpression)
        : Expression(expCtx, {inputExpression}) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    Value evaluate(const Document& root, Variables* variables) const override;
    Value serialize(const SerializationOptions& options = {}) const final;
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
          _opName(opName) {}

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(const SerializationOptions& options = {}) const final;

    const Expression* getStartDate() const {
        return _children[_kStartDate].get();
    }
    const Expression* getUnit() const {
        return _children[_kUnit].get();
    }
    const Expression* getAmount() const {
        return _children[_kAmount].get();
    }
    const Expression* getTimeZone() const {
        return _children[_kTimeZone].get();
    }
    const boost::optional<TimeUnit>& getParsedUnit() const {
        return _parsedUnit;
    }
    const boost::optional<TimeZone>& getParsedTimeZone() const {
        return _parsedTimeZone;
    }
    StringData getOpName() const {
        return _opName;
    }

protected:
    monotonic::State getMonotonicState(const FieldPath& sortedFieldPath) const final;
    virtual monotonic::State combineMonotonicStateOfArguments(
        monotonic::State startDataMonotonicState, monotonic::State amountMonotonicState) const = 0;

private:
    // The expression representing the startDate argument.
    static constexpr size_t _kStartDate = 0;

    // Unit of time: year, quarter, week, etc.
    static constexpr size_t _kUnit = 1;

    // Amount of units to be added or subtracted.
    static constexpr size_t _kAmount = 2;

    // The expression representing the timezone argument.
    static constexpr size_t _kTimeZone = 3;

    // Pre-parsed time unit, if the above expression is a constant.
    boost::optional<TimeUnit> _parsedUnit;

    // Pre-parsed timezone, if the above expression is a constant.
    boost::optional<TimeZone> _parsedTimeZone;

    // The name of this expression, e.g. $dateAdd or $dateSubtract.
    StringData _opName;
};

class ExpressionDateAdd final : public ExpressionDateArithmetics {
public:
    using ExpressionDateArithmetics::ExpressionDateArithmetics;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);
    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    monotonic::State combineMonotonicStateOfArguments(
        monotonic::State startDataMonotonicState,
        monotonic::State amountMonotonicState) const final;
};

class ExpressionDateSubtract final : public ExpressionDateArithmetics {
public:
    using ExpressionDateArithmetics::ExpressionDateArithmetics;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);
    Value evaluate(const Document& root, Variables* variables) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    monotonic::State combineMonotonicStateOfArguments(
        monotonic::State startDataMonotonicState,
        monotonic::State amountMonotonicState) const final;
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
 * This visitor is used to visit only ExpressionFieldPath nodes in an expression tree and call 'fn'
 * on them.
 *
 * Usage example:
 * bool isFoo = false;
 * FieldPathVisitor visitor([&](const ExpressionFieldPath* expr) {
 *     isFoo = isFoo || expr->isFoo();
 * });
 */
template <typename F>
struct FieldPathVisitor : public SelectiveConstExpressionVisitorBase {
    // To avoid overloaded-virtual warnings.
    using SelectiveConstExpressionVisitorBase::visit;

    explicit FieldPathVisitor(const F& fn) : _fn(fn) {}

    void visit(const ExpressionFieldPath* expr) final {
        _fn(expr);
    }

    F _fn;
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
    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(const SerializationOptions& options = {}) const final;
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
        return static_cast<bool>(_children[_kTimeZone]);
    }

    /**
     * Returns true if this expression has parameter 'startOfWeek' specified, otherwise false.
     */
    bool isStartOfWeekSpecified() const {
        return static_cast<bool>(_children[_kStartOfWeek]);
    }

    /**
     * Returns true if this expression has parameter 'binSize' specified, otherwise false.
     */
    bool isBinSizeSpecified() const {
        return static_cast<bool>(_children[_kBinSize]);
    }

    boost::optional<TimeUnit> getOptimizedUnit() const {
        if (_parsedUnit) {
            return _parsedUnit;
        }
        return boost::none;
    }

    boost::optional<long long> getOptimizedBinSize() const {
        if (_parsedBinSize) {
            return _parsedBinSize;
        }
        return boost::none;
    }

    boost::optional<TimeZone> getOptimizedTimeZone() const {
        if (_parsedTimeZone) {
            return _parsedTimeZone;
        }
        return boost::none;
    }

    const Expression* getDate() const {
        return _children[_kDate].get();
    }
    const Expression* getUnit() const {
        return _children[_kUnit].get();
    }
    const Expression* getBinSize() const {
        return _children[_kBinSize].get();
    }
    const Expression* getTimeZone() const {
        return _children[_kTimeZone].get();
    }
    const Expression* getStartOfWeek() const {
        return _children[_kStartOfWeek].get();
    }
    const boost::optional<TimeZone>& getParsedTimeZone() const {
        return _parsedTimeZone;
    }
    const boost::optional<TimeUnit>& getParsedUnit() const {
        return _parsedUnit;
    }
    const boost::optional<long long>& getParsedBinSize() const {
        return _parsedBinSize;
    }
    const boost::optional<DayOfWeek>& getParsedStartOfWeek() const {
        return _parsedStartOfWeek;
    }

private:
    monotonic::State getMonotonicState(const FieldPath& sortedFieldPath) const final;

    // Expression that evaluates to a date to truncate. Accepted BSON types: Date, bsonTimestamp,
    // jstOID.
    static constexpr size_t _kDate = 0;

    // Time units used to describe the size of bins. Accepted BSON type: String. Accepted values:
    // enumerators from TimeUnit enumeration.
    static constexpr size_t _kUnit = 1;

    // Size of bins in time units '_unit'. Accepted BSON types: NumberInt, NumberLong, NumberDouble,
    // NumberDecimal. Accepted are only values that can be coerced to a 64-bit integer without loss.
    // If not specified, 1 is used.
    static constexpr size_t _kBinSize = 2;

    // Timezone to use for the truncation operation. Accepted BSON type: String. If not specified,
    // UTC is used.
    static constexpr size_t _kTimeZone = 3;

    // First/start day of the week to use for date truncation when the time unit is the week.
    // Accepted BSON type: String. If not specified, "sunday" is used.
    static constexpr size_t _kStartOfWeek = 4;

    // Pre-parsed timezone, if the above expression is a constant.
    boost::optional<TimeZone> _parsedTimeZone;

    // Pre-parsed time unit, if the above expression is a constant.
    boost::optional<TimeUnit> _parsedUnit;

    // Pre-parsed bin size, if the above expression is a constant.
    boost::optional<long long> _parsedBinSize;

    // Pre-parsed start of week, if the above expression is a constant.
    boost::optional<DayOfWeek> _parsedStartOfWeek;

    template <typename H>
    friend class ExpressionHashVisitor;
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
        : Expression(expCtx, {std::move(field), std::move(input)}) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    Value serialize(const SerializationOptions& options = {}) const final;

    Value evaluate(const Document& root, Variables* variables) const final;

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const Expression* getField() const {
        return _children[_kField].get();
    }

    const Expression* getInput() const {
        return _children[_kInput].get();
    }

    static constexpr auto kExpressionName = "$getField"_sd;

private:
    static constexpr size_t _kField = 0;
    static constexpr size_t _kInput = 1;
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
          _fieldName(getValidFieldName(_children[_kField])) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    Value serialize(const SerializationOptions& options = {}) const final;

    Value evaluate(const Document& root, Variables* variables) const final;

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const std::string& getFieldName() const {
        return _fieldName;
    }

    const Expression* getField() const {
        return _children[_kField].get();
    }

    const Expression* getInput() const {
        return _children[_kInput].get();
    }

    const Expression* getValue() const {
        return _children[_kValue].get();
    }

    static constexpr auto kExpressionName = "$setField"_sd;

private:
    /**
     * Ensures 'fieldExpr' is a constant string representing a valid field name and returns it as a
     * string. If 'fieldExpr' is not valid, this function will throw a 'uassert()'.
     */
    std::string getValidFieldName(boost::intrusive_ptr<Expression> fieldExpr);

    // This is pre-validated by the constructor.
    const std::string _fieldName;

    static constexpr size_t _kField = 0;
    static constexpr size_t _kInput = 1;
    static constexpr size_t _kValue = 2;
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

template <typename SubClass>
class ExpressionBitwise : public ExpressionVariadic<SubClass> {
public:
    explicit ExpressionBitwise(ExpressionContext* const expCtx)
        : ExpressionVariadic<SubClass>(expCtx) {}

    ExpressionBitwise(ExpressionContext* const expCtx, Expression::ExpressionVector&& children)
        : ExpressionVariadic<SubClass>(expCtx, std::move(children)) {}

    ExpressionNary::Associativity getAssociativity() const final {
        return ExpressionNary::Associativity::kFull;
    }

    bool isCommutative() const final {
        return true;
    }

private:
    virtual SafeNum getIdentity() const = 0;
};

class ExpressionBitAnd final : public ExpressionBitwise<ExpressionBitAnd> {
public:
    SafeNum getIdentity() const final {
        return -1;  // In two's complement, this is all 1's.
    }

    const char* getOpName() const final {
        return "$bitAnd";
    }

    Value evaluate(const Document& root, Variables* variables) const final;

    explicit ExpressionBitAnd(ExpressionContext* const expCtx)
        : ExpressionBitwise<ExpressionBitAnd>(expCtx) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    ExpressionBitAnd(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionBitwise<ExpressionBitAnd>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionBitOr final : public ExpressionBitwise<ExpressionBitOr> {
public:
    SafeNum getIdentity() const final {
        return 0;
    }

    const char* getOpName() const final {
        return "$bitOr";
    }

    Value evaluate(const Document& root, Variables* variables) const final;

    explicit ExpressionBitOr(ExpressionContext* const expCtx)
        : ExpressionBitwise<ExpressionBitOr>(expCtx) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    ExpressionBitOr(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionBitwise<ExpressionBitOr>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }
    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionBitXor final : public ExpressionBitwise<ExpressionBitXor> {
public:
    SafeNum getIdentity() const final {
        return 0;
    }

    const char* getOpName() const final {
        return "$bitXor";
    }

    Value evaluate(const Document& root, Variables* variables) const final;

    explicit ExpressionBitXor(ExpressionContext* const expCtx)
        : ExpressionBitwise<ExpressionBitXor>(expCtx) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    ExpressionBitXor(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionBitwise<ExpressionBitXor>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionBitNot final : public ExpressionSingleNumericArg<ExpressionBitNot> {
public:
    explicit ExpressionBitNot(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg<ExpressionBitNot>(expCtx) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }
    explicit ExpressionBitNot(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionSingleNumericArg<ExpressionBitNot>(expCtx, std::move(children)) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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

/**
 * The expression '$_internalKeyStringValue' is used to generate the key string binary of any
 * document value ('input' field) under an optionally different non-default collation ('collation'
 * field). The generated key string binary purposefully doesn't contain the type bits information,
 * so that the generated binary has the same ordering as the index.
 *
 * The expression specification is a follows:
 * {
 *     $_internalKeyStringValue: {
 *         input: <expression>,
 *         collation: <collation spec>
 *     }
 * }
 *
 * Examples:
 * Case 1: The 'input' field is an integer.
 * Input1:
 * {
 *     $_internalKeyStringValue: {
 *         input: 1
 *     }
 * }
 * Output1: BinData(0, "KwIE")
 *
 * Case 2: The 'input' field is an integer of the same numeric value as above but different type.
 * Input2:
 * {
 *     $_internalKeyStringValue: {
 *         input: 1.0
 *     }
 * }
 * Output2: BinData(0, "KwIE")
 *
 * Case 3: The 'input' field is a string. The 'collation' field is a non-default collation spec.
 * Input3:
 * {
 *     $_internalIndexKey: {
 *         input: "aAa",
 *         collation: {locale: "en", strength: 1}
 *     }
 * }
 * Output3: BinData(0, "PCkpKQAE")
 */
class ExpressionInternalKeyStringValue final : public Expression {
public:
    ExpressionInternalKeyStringValue(ExpressionContext* expCtx,
                                     boost::intrusive_ptr<Expression> input,
                                     boost::intrusive_ptr<Expression> collation)
        : Expression(expCtx, {input, collation}) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    Value serialize(const SerializationOptions& options = {}) const final;

    Value evaluate(const Document& root, Variables* variables) const final;

    const char* getOpName() const {
        return "$_internalKeyStringValue";
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const Expression* getInput() const {
        return _children[_kInput].get();
    }

    const Expression* getCollation() const {
        return _children[_kCollation].get();
    }

private:
    static constexpr size_t _kInput = 0;
    static constexpr size_t _kCollation = 1;
};

/**
 * Returns a UUID.
 */
class ExpressionCreateUUID final : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement exprElement,
                                                  const VariablesParseState& vps);

    Value serialize(const SerializationOptions& options = {}) const final;

    Value evaluate(const Document& root, Variables* variables) const final;

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;

    const char* getOpName() const;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    explicit ExpressionCreateUUID(ExpressionContext* expCtx);
};


class ExpressionCreateObjectId : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement exprElement,
                                                  const VariablesParseState& vps);

    Value serialize(const SerializationOptions& options = {}) const final;

    Value evaluate(const Document& root, Variables* variables) const final;

    [[nodiscard]] boost::intrusive_ptr<Expression> optimize() final;

    const char* getOpName() const;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

private:
    explicit ExpressionCreateObjectId(ExpressionContext* expCtx);
};


/**
 * ExpressionEncTextSearch is the base class for all encrypted text search expressions. The first
 * operand (input) must be a field path expression, and the second operand (text) a constant
 * literal expression. Note, specializations of this class can only be evaluated when their text
 * operand is a BinData payload.
 * The constant literal (text) can only be one of the following:
 * 1) A string literal: In this case, the expression can't be evaluated, and is likely being used in
 *    the context of query analysis to parse and analyze the query.
 * 2) A BinData payload: In this case, the expression can be evaluated by the server.
 */
class ExpressionEncTextSearch : public Expression {
public:
    const auto& getEncryptedPredicateEvaluator() const {
        return _evaluatorV2;
    }

    // If we didn't have an encrypted payload at instantiation time, then we can't be evaluated.
    // We'll use this for guardrails around code we know shouldn't ever be called.
    bool canBeEvaluated() const;

    const ExpressionFieldPath& getInput() const;
    const ExpressionConstant& getText() const;

protected:
    ExpressionEncTextSearch(ExpressionContext* expCtx,
                            boost::intrusive_ptr<Expression> input,
                            boost::intrusive_ptr<Expression> text);

    static constexpr size_t _kInput = 0;
    static constexpr size_t _kTextOperand = 1;

private:
    EncryptedPredicateEvaluatorV2 _evaluatorV2;
};

class ExpressionEncStrStartsWith final : public ExpressionEncTextSearch {
public:
    ExpressionEncStrStartsWith(ExpressionContext* expCtx,
                               boost::intrusive_ptr<Expression> input,
                               boost::intrusive_ptr<Expression> prefix);

    Value evaluate(const Document& root, Variables* variables) const final;
    Value serialize(const SerializationOptions& options = {}) const final;
    const char* getOpName() const;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement bsonExpr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionEncStrEndsWith final : public ExpressionEncTextSearch {
public:
    ExpressionEncStrEndsWith(ExpressionContext* expCtx,
                             boost::intrusive_ptr<Expression> input,
                             boost::intrusive_ptr<Expression> suffix);

    Value evaluate(const Document& root, Variables* variables) const final;
    Value serialize(const SerializationOptions& options = {}) const final;
    const char* getOpName() const;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement bsonExpr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionEncStrContains final : public ExpressionEncTextSearch {
public:
    ExpressionEncStrContains(ExpressionContext* expCtx,
                             boost::intrusive_ptr<Expression> input,
                             boost::intrusive_ptr<Expression> substring);

    Value evaluate(const Document& root, Variables* variables) const final;
    Value serialize(const SerializationOptions& options = {}) const final;
    const char* getOpName() const;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement bsonExpr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionEncStrNormalizedEq final : public ExpressionEncTextSearch {
public:
    ExpressionEncStrNormalizedEq(ExpressionContext* expCtx,
                                 boost::intrusive_ptr<Expression> input,
                                 boost::intrusive_ptr<Expression> substring);

    Value evaluate(const Document& root, Variables* variables) const final;
    Value serialize(const SerializationOptions& options = {}) const final;
    const char* getOpName() const;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement bsonExpr,
                                                  const VariablesParseState& vps);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

static boost::intrusive_ptr<Expression> parseParenthesisExprObj(ExpressionContext* expCtx,
                                                                BSONElement expr,
                                                                const VariablesParseState& vpsIn);

}  // namespace mongo
