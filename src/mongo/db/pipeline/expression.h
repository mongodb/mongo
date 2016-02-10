/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <map>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BSONArrayBuilder;
class BSONElement;
class BSONObjBuilder;
class DocumentSource;

/**
 * Registers an Parser so it can be called from parseExpression and friends.
 *
 * As an example, if your expression looks like {"$foo": [1,2,3]} you would add this line:
 * REGISTER_EXPRESSION(foo, ExpressionFoo::parse);
 */
#define REGISTER_EXPRESSION(key, parser)                                     \
    MONGO_INITIALIZER(addToExpressionParserMap_##key)(InitializerContext*) { \
        Expression::registerExpression("$" #key, (parser));                  \
        return Status::OK();                                                 \
    }

// TODO: Look into merging with ExpressionContext and possibly ObjectCtx.
/// The state used as input and working space for Expressions.
class Variables {
    MONGO_DISALLOW_COPYING(Variables);

public:
    /**
     * Each unique variable is assigned a unique id of this type
     */
    typedef size_t Id;

    // This is only for expressions that use no variables (even ROOT).
    Variables() : _numVars(0) {}

    explicit Variables(size_t numVars, const Document& root = Document())
        : _root(root), _rest(numVars == 0 ? NULL : new Value[numVars]), _numVars(numVars) {}

    static void uassertValidNameForUserWrite(StringData varName);
    static void uassertValidNameForUserRead(StringData varName);

    static const Id ROOT_ID = Id(-1);

    /**
     * Use this instead of setValue for setting ROOT
     */
    void setRoot(const Document& root) {
        _root = root;
    }
    void clearRoot() {
        _root = Document();
    }
    const Document& getRoot() const {
        return _root;
    }

    void setValue(Id id, const Value& value);
    Value getValue(Id id) const;

    /**
     * returns Document() for non-document values.
     */
    Document getDocument(Id id) const;

private:
    Document _root;
    const std::unique_ptr<Value[]> _rest;
    const size_t _numVars;
};

/**
 * Generates Variables::Ids and keeps track of the number of Ids handed out.
 */
class VariablesIdGenerator {
public:
    VariablesIdGenerator() : _nextId(0) {}

    Variables::Id generateId() {
        return _nextId++;
    }

    /**
     * Returns the number of Ids handed out by this Generator.
     * Return value is intended to be passed to Variables constructor.
     */
    Variables::Id getIdCount() const {
        return _nextId;
    }

private:
    Variables::Id _nextId;
};

/**
 * This class represents the Variables that are defined in an Expression tree.
 *
 * All copies from a given instance share enough information to ensure unique Ids are assigned
 * and to propagate back to the original instance enough information to correctly construct a
 * Variables instance.
 */
class VariablesParseState {
public:
    explicit VariablesParseState(VariablesIdGenerator* idGenerator) : _idGenerator(idGenerator) {}

    /**
     * Assigns a named variable a unique Id. This differs from all other variables, even
     * others with the same name.
     *
     * The special variables ROOT and CURRENT are always implicitly defined with CURRENT
     * equivalent to ROOT. If CURRENT is explicitly defined by a call to this function, it
     * breaks that equivalence.
     *
     * NOTE: Name validation is responsibility of caller.
     */
    Variables::Id defineVariable(StringData name);

    /**
     * Returns the current Id for a variable. uasserts if the variable isn't defined.
     */
    Variables::Id getVariable(StringData name) const;

private:
    StringMap<Variables::Id> _variables;
    VariablesIdGenerator* _idGenerator;
};

class Expression : public IntrusiveCounterUnsigned {
public:
    using Parser =
        stdx::function<boost::intrusive_ptr<Expression>(BSONElement, const VariablesParseState&)>;

    virtual ~Expression(){};

    /*
      Optimize the Expression.

      This provides an opportunity to do constant folding, or to
      collapse nested operators that have the same precedence, such as
      $add, $and, or $or.

      The Expression should be replaced with the return value, which may
      or may not be the same object.  In the case of constant folding,
      a computed expression may be replaced by a constant.

      @returns the optimized Expression
     */
    virtual boost::intrusive_ptr<Expression> optimize() {
        return this;
    }

    /**
     * Add this expression's field dependencies to the set
     *
     * Expressions are trees, so this is often recursive.
     *
     * @param deps Fully qualified paths to depended-on fields are added to this set.
     *             Empty std::string means need full document.
     * @param path path to self if all ancestors are ExpressionObjects.
     *             Top-level ExpressionObject gets pointer to empty vector.
     *             If any other Expression is an ancestor, or in other cases
     *             where {a:1} inclusion objects aren't allowed, they get
     *             NULL.
     */
    virtual void addDependencies(DepsTracker* deps,
                                 std::vector<std::string>* path = NULL) const = 0;

    /** simple expressions are just inclusion exclusion as supported by ExpressionObject */
    virtual bool isSimple() {
        return false;
    }


    /**
     * Serialize the Expression tree recursively.
     * If explain is false, returns a Value parsable by parseOperand().
     */
    virtual Value serialize(bool explain) const = 0;

    /// Evaluate expression with specified inputs and return result. (only used by tests)
    Value evaluate(const Document& root) const {
        Variables vars(0, root);
        return evaluate(&vars);
    }

    /**
     * Evaluate expression with specified inputs and return result.
     *
     * While vars is non-const, if properly constructed, subexpressions modifications to it
     * should not effect outer expressions due to unique variable Ids.
     */
    Value evaluate(Variables* vars) const {
        return evaluateInternal(vars);
    }

    /*
      Utility class for parseObject() below.

      DOCUMENT_OK indicates that it is OK to use a Document in the current
      context.
     */
    class ObjectCtx {
    public:
        explicit ObjectCtx(int options);
        static const int DOCUMENT_OK = 0x0001;
        static const int TOP_LEVEL = 0x0002;
        static const int INCLUSION_OK = 0x0004;

        bool documentOk() const;
        bool topLevel() const;
        bool inclusionOk() const;

    private:
        int options;
    };

    //
    // Diagram of relationship between parse functions when parsing a $op:
    //
    // { someFieldOrArrayIndex: { $op: [ARGS] } }
    //                             ^ parseExpression on inner $op BSONElement
    //                          ^ parseObject on BSONObject
    //             ^ parseOperand on outer BSONElement wrapping the $op Object
    //

    /**
     * Parses a BSON Object that could represent a functional expression or a Document
     * expression.
     */
    static boost::intrusive_ptr<Expression> parseObject(BSONObj obj,
                                                        ObjectCtx* pCtx,
                                                        const VariablesParseState& vps);

    /**
     * Parses a BSONElement which has already been determined to be functional expression.
     *
     * exprElement should be the only element inside the expression object. That is the
     * field name should be the $op for the expression.
     */
    static boost::intrusive_ptr<Expression> parseExpression(BSONElement exprElement,
                                                            const VariablesParseState& vps);


    /**
     * Parses a BSONElement which is an operand in an Expression.
     *
     * This is the most generic parser and can parse ExpressionFieldPath, a literal, or a $op.
     * If it is a $op, exprElement should be the outer element whose value is an Object
     * containing the $op.
     */
    static boost::intrusive_ptr<Expression> parseOperand(BSONElement exprElement,
                                                         const VariablesParseState& vps);

    /*
      Produce a field path std::string with the field prefix removed.

      Throws an error if the field prefix is not present.

      @param prefixedField the prefixed field
      @returns the field path with the prefix removed
     */
    static std::string removeFieldPrefix(const std::string& prefixedField);

    /** Evaluate the subclass Expression using the given Variables as context and return result.
     *
     *  Should only be called by subclasses, but can't be protected because they need to call
     *  this function on each other.
     */
    virtual Value evaluateInternal(Variables* vars) const = 0;

    /**
     * Registers an Parser so it can be called from parseExpression and friends.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_EXPRESSION macro defined in this
     * file.
     */
    static void registerExpression(std::string key, Parser parser);

protected:
    typedef std::vector<boost::intrusive_ptr<Expression>> ExpressionVector;
};


/// Inherit from ExpressionVariadic or ExpressionFixedArity instead of directly from this class.
class ExpressionNary : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() override;
    Value serialize(bool explain) const override;
    void addDependencies(DepsTracker* deps, std::vector<std::string>* path = NULL) const override;

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

    /*
      Get the name of the operator.

      @returns the name of the operator; this std::string belongs to the class
        implementation, and should not be deleted
        and should not
    */
    virtual const char* getOpName() const = 0;

    /// Allow subclasses the opportunity to validate arguments at parse time.
    virtual void validateArguments(const ExpressionVector& args) const {}

    static ExpressionVector parseArguments(BSONElement bsonExpr, const VariablesParseState& vps);

protected:
    ExpressionNary() {}

    ExpressionVector vpOperand;
};

/// Inherit from ExpressionVariadic or ExpressionFixedArity instead of directly from this class.
template <typename SubClass>
class ExpressionNaryBase : public ExpressionNary {
public:
    static boost::intrusive_ptr<Expression> parse(BSONElement bsonExpr,
                                                  const VariablesParseState& vps) {
        boost::intrusive_ptr<ExpressionNaryBase> expr = new SubClass();
        ExpressionVector args = parseArguments(bsonExpr, vps);
        expr->validateArguments(args);
        expr->vpOperand = args;
        return expr;
    }
};

/// Inherit from this class if your expression takes a variable number of arguments.
template <typename SubClass>
class ExpressionVariadic : public ExpressionNaryBase<SubClass> {};

/**
 * Inherit from this class if your expression can take a range of arguments, e.g. if it has some
 * optional arguments.
 */
template <typename SubClass, int MinArgs, int MaxArgs>
class ExpressionRangedArity : public ExpressionNaryBase<SubClass> {
public:
    void validateArguments(const Expression::ExpressionVector& args) const override {
        uassert(28667,
                mongoutils::str::stream()
                    << "Expression " << this->getOpName() << " takes at least " << MinArgs
                    << " arguments, and at most " << MaxArgs << ", but " << args.size()
                    << " were passed in.",
                MinArgs <= args.size() && args.size() <= MaxArgs);
    }
};

/// Inherit from this class if your expression takes a fixed number of arguments.
template <typename SubClass, int NArgs>
class ExpressionFixedArity : public ExpressionNaryBase<SubClass> {
public:
    void validateArguments(const Expression::ExpressionVector& args) const override {
        uassert(16020,
                mongoutils::str::stream() << "Expression " << this->getOpName() << " takes exactly "
                                          << NArgs << " arguments. " << args.size()
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
    Value evaluateInternal(Variables* vars) const final {
        Accumulator accum;
        const size_t n = this->vpOperand.size();
        // If a single array arg is given, loop through it passing each member to the accumulator.
        // If a single, non-array arg is given, pass it directly to the accumulator.
        if (n == 1) {
            Value singleVal = this->vpOperand[0]->evaluateInternal(vars);
            if (singleVal.getType() == Array) {
                for (const Value& val : singleVal.getArray()) {
                    accum.process(val, false);
                }
            } else {
                accum.process(singleVal, false);
            }
        } else {
            // If multiple arguments are given, pass all arguments to the accumulator.
            for (auto&& argument : this->vpOperand) {
                accum.process(argument->evaluateInternal(vars), false);
            }
        }
        return accum.getValue(false);
    }

    bool isAssociative() const final {
        // Return false if a single argument is given to avoid a single array argument being treated
        // as an array instead of as a list of arguments.
        if (this->vpOperand.size() == 1) {
            return false;
        }
        return Accumulator().isAssociative();
    }

    bool isCommutative() const final {
        return Accumulator().isCommutative();
    }

    const char* getOpName() const final {
        return Accumulator().getOpName();
    }
};

/**
 * Inherit from this class if your expression takes exactly one numeric argument.
 */
template <typename SubClass>
class ExpressionSingleNumericArg : public ExpressionFixedArity<SubClass, 1> {
public:
    virtual ~ExpressionSingleNumericArg() {}

    Value evaluateInternal(Variables* vars) const final {
        Value arg = this->vpOperand[0]->evaluateInternal(vars);
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


class ExpressionAbs final : public ExpressionSingleNumericArg<ExpressionAbs> {
    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;
};


class ExpressionAdd final : public ExpressionVariadic<ExpressionAdd> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }
};


class ExpressionAllElementsTrue final : public ExpressionFixedArity<ExpressionAllElementsTrue, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionAnd final : public ExpressionVariadic<ExpressionAnd> {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }
};


class ExpressionAnyElementTrue final : public ExpressionFixedArity<ExpressionAnyElementTrue, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionArray final : public ExpressionVariadic<ExpressionArray> {
public:
    // virtuals from ExpressionNary
    Value evaluateInternal(Variables* vars) const final;
    Value serialize(bool explain) const final;
    const char* getOpName() const final;
};


class ExpressionArrayElemAt final : public ExpressionFixedArity<ExpressionArrayElemAt, 2> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionCeil final : public ExpressionSingleNumericArg<ExpressionCeil> {
public:
    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;
};


class ExpressionCoerceToBool final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    void addDependencies(DepsTracker* deps, std::vector<std::string>* path = NULL) const final;
    Value evaluateInternal(Variables* vars) const final;
    Value serialize(bool explain) const final;

    static boost::intrusive_ptr<ExpressionCoerceToBool> create(
        const boost::intrusive_ptr<Expression>& pExpression);

private:
    explicit ExpressionCoerceToBool(const boost::intrusive_ptr<Expression>& pExpression);

    boost::intrusive_ptr<Expression> pExpression;
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

    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    static boost::intrusive_ptr<Expression> parse(BSONElement bsonExpr,
                                                  const VariablesParseState& vps,
                                                  CmpOp cmpOp);

    explicit ExpressionCompare(CmpOp cmpOp);

private:
    CmpOp cmpOp;
};


class ExpressionConcat final : public ExpressionVariadic<ExpressionConcat> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }
};


class ExpressionConcatArrays final : public ExpressionVariadic<ExpressionConcatArrays> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }
};


class ExpressionCond final : public ExpressionFixedArity<ExpressionCond, 3> {
    typedef ExpressionFixedArity<ExpressionCond, 3> Base;

public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    static boost::intrusive_ptr<Expression> parse(BSONElement expr, const VariablesParseState& vps);
};


class ExpressionConstant final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    void addDependencies(DepsTracker* deps, std::vector<std::string>* path = NULL) const final;
    Value evaluateInternal(Variables* vars) const final;
    Value serialize(bool explain) const final;

    const char* getOpName() const;

    static boost::intrusive_ptr<ExpressionConstant> create(const Value& pValue);
    static boost::intrusive_ptr<Expression> parse(BSONElement bsonExpr,
                                                  const VariablesParseState& vps);

    /*
      Get the constant value represented by this Expression.

      @returns the value
     */
    Value getValue() const {
        return pValue;
    }

private:
    explicit ExpressionConstant(const Value& pValue);

    Value pValue;
};

class ExpressionDateToString final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluateInternal(Variables* vars) const final;
    void addDependencies(DepsTracker* deps, std::vector<std::string>* path = NULL) const final;

    static boost::intrusive_ptr<Expression> parse(BSONElement expr, const VariablesParseState& vps);

private:
    ExpressionDateToString(const std::string& format,               // the format string
                           boost::intrusive_ptr<Expression> date);  // the date to format

    // Will uassert on invalid data
    static void validateFormat(const std::string& format);

    // Need raw date as tm doesn't have millisecond resolution.
    // Format must be valid.
    static std::string formatDate(const std::string& format, const tm& tm, const long long date);

    static void insertPadded(StringBuilder& sb, int number, int spaces);

    const std::string _format;
    boost::intrusive_ptr<Expression> _date;
};

class ExpressionDayOfMonth final : public ExpressionFixedArity<ExpressionDayOfMonth, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    static inline int extract(const tm& tm) {
        return tm.tm_mday;
    }
};


class ExpressionDayOfWeek final : public ExpressionFixedArity<ExpressionDayOfWeek, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    // MySQL uses 1-7, tm uses 0-6
    static inline int extract(const tm& tm) {
        return tm.tm_wday + 1;
    }
};


class ExpressionDayOfYear final : public ExpressionFixedArity<ExpressionDayOfYear, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    // MySQL uses 1-366, tm uses 0-365
    static inline int extract(const tm& tm) {
        return tm.tm_yday + 1;
    }
};


class ExpressionDivide final : public ExpressionFixedArity<ExpressionDivide, 2> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionExp final : public ExpressionSingleNumericArg<ExpressionExp> {
    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;
};


class ExpressionFieldPath final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    void addDependencies(DepsTracker* deps, std::vector<std::string>* path = NULL) const final;
    Value evaluateInternal(Variables* vars) const final;
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
    static boost::intrusive_ptr<ExpressionFieldPath> create(const std::string& fieldPath);

    /// Like create(), but works with the raw std::string from the user with the "$" prefixes.
    static boost::intrusive_ptr<ExpressionFieldPath> parse(const std::string& raw,
                                                           const VariablesParseState& vps);

    const FieldPath& getFieldPath() const {
        return _fieldPath;
    }

private:
    ExpressionFieldPath(const std::string& fieldPath, Variables::Id variable);

    /*
      Internal implementation of evaluateInternal(), used recursively.

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
    Value evaluateInternal(Variables* vars) const final;
    void addDependencies(DepsTracker* deps, std::vector<std::string>* path = NULL) const final;

    static boost::intrusive_ptr<Expression> parse(BSONElement expr, const VariablesParseState& vps);

private:
    ExpressionFilter(std::string varName,
                     Variables::Id varId,
                     boost::intrusive_ptr<Expression> input,
                     boost::intrusive_ptr<Expression> filter);

    // The name of the variable to set to each element in the array.
    std::string _varName;
    // The id of the variable to set.
    Variables::Id _varId;
    // The array to iterate over.
    boost::intrusive_ptr<Expression> _input;
    // The expression determining whether each element should be present in the result array.
    boost::intrusive_ptr<Expression> _filter;
};


class ExpressionFloor final : public ExpressionSingleNumericArg<ExpressionFloor> {
public:
    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;
};


class ExpressionHour final : public ExpressionFixedArity<ExpressionHour, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    static inline int extract(const tm& tm) {
        return tm.tm_hour;
    }
};


class ExpressionIfNull final : public ExpressionFixedArity<ExpressionIfNull, 2> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionLet final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluateInternal(Variables* vars) const final;
    void addDependencies(DepsTracker* deps, std::vector<std::string>* path = NULL) const final;

    static boost::intrusive_ptr<Expression> parse(BSONElement expr, const VariablesParseState& vps);

    struct NameAndExpression {
        NameAndExpression() {}
        NameAndExpression(std::string name, boost::intrusive_ptr<Expression> expression)
            : name(name), expression(expression) {}

        std::string name;
        boost::intrusive_ptr<Expression> expression;
    };

    typedef std::map<Variables::Id, NameAndExpression> VariableMap;

private:
    ExpressionLet(const VariableMap& vars, boost::intrusive_ptr<Expression> subExpression);

    VariableMap _variables;
    boost::intrusive_ptr<Expression> _subExpression;
};

class ExpressionLn final : public ExpressionSingleNumericArg<ExpressionLn> {
    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;
};

class ExpressionLog final : public ExpressionFixedArity<ExpressionLog, 2> {
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};

class ExpressionLog10 final : public ExpressionSingleNumericArg<ExpressionLog10> {
    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;
};

class ExpressionMap final : public Expression {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value serialize(bool explain) const final;
    Value evaluateInternal(Variables* vars) const final;
    void addDependencies(DepsTracker* deps, std::vector<std::string>* path = NULL) const final;

    static boost::intrusive_ptr<Expression> parse(BSONElement expr, const VariablesParseState& vps);

private:
    ExpressionMap(
        const std::string& varName,              // name of variable to set
        Variables::Id varId,                     // id of variable to set
        boost::intrusive_ptr<Expression> input,  // yields array to iterate
        boost::intrusive_ptr<Expression> each);  // yields results to be added to output array

    std::string _varName;
    Variables::Id _varId;
    boost::intrusive_ptr<Expression> _input;
    boost::intrusive_ptr<Expression> _each;
};

class ExpressionMeta final : public Expression {
public:
    Value serialize(bool explain) const final;
    Value evaluateInternal(Variables* vars) const final;
    void addDependencies(DepsTracker* deps, std::vector<std::string>* path = NULL) const final;

    static boost::intrusive_ptr<Expression> parse(BSONElement expr, const VariablesParseState& vps);

private:
    enum MetaType {
        TEXT_SCORE,
        RAND_VAL,
    };

    ExpressionMeta(MetaType metaType);

    MetaType _metaType;
};

class ExpressionMillisecond final : public ExpressionFixedArity<ExpressionMillisecond, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    static int extract(const long long date);
};


class ExpressionMinute final : public ExpressionFixedArity<ExpressionMinute, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    static int extract(const tm& tm) {
        return tm.tm_min;
    }
};


class ExpressionMod final : public ExpressionFixedArity<ExpressionMod, 2> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionMultiply final : public ExpressionVariadic<ExpressionMultiply> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }
};


class ExpressionMonth final : public ExpressionFixedArity<ExpressionMonth, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    // MySQL uses 1-12, tm uses 0-11
    static inline int extract(const tm& tm) {
        return tm.tm_mon + 1;
    }
};


class ExpressionNot final : public ExpressionFixedArity<ExpressionNot, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionObject final : public Expression {
public:
    using FieldMap = std::map<std::string, boost::intrusive_ptr<Expression>>;
    boost::intrusive_ptr<Expression> optimize() final;
    bool isSimple() final;
    void addDependencies(DepsTracker* deps, std::vector<std::string>* path = NULL) const final;
    /** Only evaluates non inclusion expressions.  For inclusions, use addToDocument(). */
    Value evaluateInternal(Variables* vars) const final;
    Value serialize(bool explain) const final;

    /// like evaluate(), but return a Document instead of a Value-wrapped Document.
    Document evaluateDocument(Variables* vars) const;

    /** Evaluates with inclusions and adds results to passed in Mutable document
     *
     *  @param output the MutableDocument to add the evaluated expressions to
     *  @param currentDoc the input Document for this level (for inclusions)
     *  @param vars the variables for use in subexpressions
     */
    void addToDocument(MutableDocument& ouput, const Document& currentDoc, Variables* vars) const;

    // estimated number of fields that will be output
    size_t getSizeHint() const;

    /** Create an empty expression.
     *  Until fields are added, this will evaluate to an empty document.
     */
    static boost::intrusive_ptr<ExpressionObject> create();

    /// Like create but uses special handling of _id for root object of $project.
    static boost::intrusive_ptr<ExpressionObject> createRoot();

    /*
      Add a field to the document expression.

      @param fieldPath the path the evaluated expression will have in the
             result Document
      @param pExpression the expression to evaluate obtain this field's
             Value in the result Document
    */
    void addField(const FieldPath& fieldPath, const boost::intrusive_ptr<Expression>& pExpression);

    /*
      Add a field path to the set of those to be included.

      Note that including a nested field implies including everything on
      the path leading down to it.

      @param fieldPath the name of the field to be included
    */
    void includePath(const std::string& fieldPath);

    /*
      Get a count of the added fields.

      @returns how many fields have been added
     */
    size_t getFieldCount() const {
        return _expressions.size();
    };

    /*
      Specialized BSON conversion that allows for writing out a
      $project specification.  This creates a standalone object, which must
      be added to a containing object with a name

      @param pBuilder where to write the object to
      @param requireExpression see Expression::addToBsonObj
     */
    void documentToBson(BSONObjBuilder* pBuilder, bool requireExpression) const;

    /*
      Visitor abstraction used by emitPaths().  Each path is recorded by
      calling path().
     */
    class PathSink {
    public:
        virtual ~PathSink(){};

        /**
           Record a path.

           @param path the dotted path string
           @param include if true, the path is included; if false, the path
             is excluded
         */
        virtual void path(const std::string& path, bool include) = 0;
    };

    void excludeId(bool b) {
        _excludeId = b;
    }

    const FieldMap& getChildExpressions() const {
        return _expressions;
    }

private:
    explicit ExpressionObject(bool atRoot);

    // Mapping from fieldname to the Expression that generates its value.
    // NULL expression means inclusion from source document.
    FieldMap _expressions;

    // this is used to maintain order for generated fields not in the source document
    std::vector<std::string> _order;

    bool _excludeId;
    bool _atRoot;
};


class ExpressionOr final : public ExpressionVariadic<ExpressionOr> {
public:
    boost::intrusive_ptr<Expression> optimize() final;
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }
};

class ExpressionPow final : public ExpressionFixedArity<ExpressionPow, 2> {
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionRange final : public ExpressionRangedArity<ExpressionRange, 2, 3> {
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionSecond final : public ExpressionFixedArity<ExpressionSecond, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    static inline int extract(const tm& tm) {
        return tm.tm_sec;
    }
};


class ExpressionSetDifference final : public ExpressionFixedArity<ExpressionSetDifference, 2> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionSetEquals final : public ExpressionVariadic<ExpressionSetEquals> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
    void validateArguments(const ExpressionVector& args) const final;
};


class ExpressionSetIntersection final : public ExpressionVariadic<ExpressionSetIntersection> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }
};


// Not final, inherited from for optimizations.
class ExpressionSetIsSubset : public ExpressionFixedArity<ExpressionSetIsSubset, 2> {
public:
    boost::intrusive_ptr<Expression> optimize() override;
    Value evaluateInternal(Variables* vars) const override;
    const char* getOpName() const final;

private:
    class Optimized;
};


class ExpressionSetUnion final : public ExpressionVariadic<ExpressionSetUnion> {
public:
    // intrusive_ptr<Expression> optimize() final;
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }
};


class ExpressionSize final : public ExpressionFixedArity<ExpressionSize, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionReverseArray final : public ExpressionFixedArity<ExpressionReverseArray, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionSlice final : public ExpressionRangedArity<ExpressionSlice, 2, 3> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionIsArray final : public ExpressionFixedArity<ExpressionIsArray, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionSqrt final : public ExpressionSingleNumericArg<ExpressionSqrt> {
    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;
};


class ExpressionStrcasecmp final : public ExpressionFixedArity<ExpressionStrcasecmp, 2> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionSubstr final : public ExpressionFixedArity<ExpressionSubstr, 3> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionSubtract final : public ExpressionFixedArity<ExpressionSubtract, 2> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionToLower final : public ExpressionFixedArity<ExpressionToLower, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionToUpper final : public ExpressionFixedArity<ExpressionToUpper, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionTrunc final : public ExpressionSingleNumericArg<ExpressionTrunc> {
public:
    Value evaluateNumericArg(const Value& numericArg) const final;
    const char* getOpName() const final;
};


class ExpressionType final : public ExpressionFixedArity<ExpressionType, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;
};


class ExpressionWeek final : public ExpressionFixedArity<ExpressionWeek, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    static int extract(const tm& tm);
};


class ExpressionYear final : public ExpressionFixedArity<ExpressionYear, 1> {
public:
    Value evaluateInternal(Variables* vars) const final;
    const char* getOpName() const final;

    // tm_year is years since 1990
    static int extract(const tm& tm) {
        return tm.tm_year + 1900;
    }
};
}
