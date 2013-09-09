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

#include "mongo/pch.h"

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    class BSONArrayBuilder;
    class BSONElement;
    class BSONObjBuilder;
    class DocumentSource;

    // TODO: Look into merging with ExpressionContext and possibly ObjectCtx.
    /// The state used as input to Expressions
    class Variables {
    public:
        Variables() {}

        explicit Variables(const Document& rootAndCurrent)
            : root(rootAndCurrent)
            , current(rootAndCurrent)
        {}

        Variables(const Document& root, const Value& current, const Document& rest = Document())
            : root(root)
            , current(current)
            , rest(rest)
        {}

        static void uassertValidNameForUserWrite(StringData varName);
        static void uassertValidNameForUserRead(StringData varName);

        Value root;
        Value current;
        Document rest;
    };

    class Expression :
        public IntrusiveCounterUnsigned {
    public:
        virtual ~Expression() {};

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
        virtual intrusive_ptr<Expression> optimize() = 0;

        /**
           Add this expression's field dependencies to the set

           Expressions are trees, so this is often recursive.

           @param deps output parameter
           @param path path to self if all ancestors are ExpressionObjects.
                       Top-level ExpressionObject gets pointer to empty vector.
                       If any other Expression is an ancestor, or in other cases
                       where {a:1} inclusion objects aren't allowed, they get
                       NULL.
         */
        virtual void addDependencies(set<string>& deps, vector<string>* path=NULL) const = 0;

        /** simple expressions are just inclusion exclusion as supported by ExpressionObject */
        virtual bool isSimple() { return false; }


        /**
         * Serialize the Expression tree (recursively) and results in a Value
         * parsable by parseOperand().
         */
        virtual Value serialize() const = 0;

        /*
          Convert the expression into a BSONObj that corresponds to the
          db.collection.find() predicate language.  This is intended for
          use by DocumentSourceFilter.

          This is more limited than the full expression language supported
          by all available expressions in a DocumentSource processing
          pipeline, and will fail with an assertion if an attempt is made
          to go outside the bounds of the recognized patterns, which don't
          include full computed expressions.  There are other methods available
          on DocumentSourceFilter which can be used to analyze a filter
          predicate and break it up into appropriate expressions which can
          be translated within these constraints.  As a result, the default
          implementation is to fail with an assertion; only a subset of
          operators will be able to fulfill this request.

          @param pBuilder the builder to add the expression to.
         */
        virtual void toMatcherBson(BSONObjBuilder *pBuilder) const;

        /// Evaluate expression with specified inputs and return result.
        Value evaluate(const Document& root) const { return evaluate(Variables(root)); }
        Value evaluate(const Document& root, const Value& current) const {
            return evaluate(Variables(root, current));
        }
        Value evaluate(const Variables& vars) const { return evaluateInternal(vars); }

        /*
          Utility class for parseObject() below.

          DOCUMENT_OK indicates that it is OK to use a Document in the current
          context.
         */
        class ObjectCtx {
        public:
            ObjectCtx(int options);
            static const int DOCUMENT_OK = 0x0001;
            static const int TOP_LEVEL = 0x0002;
            static const int INCLUSION_OK = 0x0004;

            bool documentOk() const;
            bool topLevel() const;
            bool inclusionOk() const;

        private:
            int options;
        };

        /*
          Parse a BSONElement Object.  The object could represent a functional
          expression or a Document expression.

          @param pBsonElement the element representing the object
          @param pCtx a MiniCtx representing the options above
          @returns the parsed Expression
         */
        static intrusive_ptr<Expression> parseObject(
            BSONElement *pBsonElement, ObjectCtx *pCtx);

        /*
          Parse a BSONElement Object which has already been determined to be
          functional expression.
        */
        static intrusive_ptr<Expression> parseExpression(BSONElement exprElement);


        /*
          Parse a BSONElement which is an operand in an Expression.

          @param exprElement the expected operand's BSONElement
          @returns the parsed operand, as an Expression
         */
        static intrusive_ptr<Expression> parseOperand(BSONElement exprElement);

        /*
          Produce a field path string with the field prefix removed.

          Throws an error if the field prefix is not present.

          @param prefixedField the prefixed field
          @returns the field path with the prefix removed
         */
        static string removeFieldPrefix(const string &prefixedField);

        /*
          Enumeration of comparison operators.  These are shared between a
          few expression implementations, so they are factored out here.

          Any changes to these values require adjustment of the lookup
          table in the implementation.
        */
        enum CmpOp {
            EQ = 0, // return true for a == b, false otherwise
            NE = 1, // return true for a != b, false otherwise
            GT = 2, // return true for a > b, false otherwise
            GTE = 3, // return true for a >= b, false otherwise
            LT = 4, // return true for a < b, false otherwise
            LTE = 5, // return true for a <= b, false otherwise
            CMP = 6, // return -1, 0, 1 for a < b, a == b, a > b
        };

        static int signum(int i);

        /** Evaluate the subclass Expression using the given Variables as context and return result.
         *
         *  Should only be called by subclasses, but can't be protected because they need to call
         *  this function on each other.
         */
        virtual Value evaluateInternal(const Variables& vars) const = 0;

    protected:
        typedef vector<intrusive_ptr<Expression> > ExpressionVector;
    };


    /// Inherit from ExpressionVariadic or ExpressionFixedArity instead of directly from this class.
    class ExpressionNary :
        public Expression {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<Expression> optimize();
        virtual Value serialize() const;
        virtual void addDependencies(set<string>& deps, vector<string>* path=NULL) const;

        /*
          Add an operand to the n-ary expression.

          @param pExpression the expression to add
        */
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        // TODO split this into two functions
        virtual bool isAssociativeAndCommutative() const { return false; }

        /*
          Get the name of the operator.

          @returns the name of the operator; this string belongs to the class
            implementation, and should not be deleted
            and should not
        */
        virtual const char *getOpName() const = 0;

        /// Allow subclasses the opportunity to validate arguments at parse time.
        virtual void validateArguments(const ExpressionVector& args) const {}

        static ExpressionVector parseArguments(BSONElement bsonExpr);

    protected:
        ExpressionNary() {}

        ExpressionVector vpOperand;
    };

    /// Inherit from ExpressionVariadic or ExpressionFixedArity instead of directly from this class.
    template <typename SubClass>
    class ExpressionNaryBase : public ExpressionNary {
    public:
        static intrusive_ptr<Expression> parse(BSONElement bsonExpr) {
            intrusive_ptr<ExpressionNaryBase> expr = new SubClass();
            ExpressionVector args = parseArguments(bsonExpr);
            expr->validateArguments(args);
            expr->vpOperand = args;
            return expr;
        }
    };

    /// Inherit from this class if your expression takes a variable number of arguments.
    template <typename SubClass>
    class ExpressionVariadic : public ExpressionNaryBase<SubClass> {
    };

    /// Inherit from this class if your expression takes a fixed number of arguments.
    template <typename SubClass, int NArgs>
    class ExpressionFixedArity : public ExpressionNaryBase<SubClass> {
    public:
        virtual void validateArguments(const Expression::ExpressionVector& args) const {
            uassert(16020, mongoutils::str::stream()
                        << "Expression " << this->getOpName() << " takes exactly " << NArgs
                        << " arguments. " << args.size() << " were passed in.",
                    args.size() == NArgs);
                    
        }
    };


    class ExpressionAdd : public ExpressionVariadic<ExpressionAdd> {
    public:
        // virtuals from Expression
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
        virtual bool isAssociativeAndCommutative() const { return true; }
    };


    class ExpressionAllElementsTrue : public ExpressionFixedArity<ExpressionAllElementsTrue, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionAnd : public ExpressionVariadic<ExpressionAnd> {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<Expression> optimize();
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
        virtual void toMatcherBson(BSONObjBuilder *pBuilder) const;
        virtual bool isAssociativeAndCommutative() const { return true; }
    };


    class ExpressionAnyElementTrue : public ExpressionFixedArity<ExpressionAnyElementTrue, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionCoerceToBool : public Expression {
    public:
        // virtuals from ExpressionNary
        virtual intrusive_ptr<Expression> optimize();
        virtual void addDependencies(set<string>& deps, vector<string>* path=NULL) const;
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual Value serialize() const;

        static intrusive_ptr<ExpressionCoerceToBool> create(
            const intrusive_ptr<Expression> &pExpression);


    private:
        ExpressionCoerceToBool(const intrusive_ptr<Expression> &pExpression);

        intrusive_ptr<Expression> pExpression;
    };


    class ExpressionCompare : public ExpressionFixedArity<ExpressionCompare, 2> {
    public:
        // virtuals from ExpressionNary
        virtual intrusive_ptr<Expression> optimize();
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;

        static intrusive_ptr<Expression> parse(BSONElement bsonExpr);

        friend class ExpressionFieldRange;
        ExpressionCompare(CmpOp cmpOp);

    private:
        CmpOp cmpOp;
    };


    class ExpressionConcat : public ExpressionVariadic<ExpressionConcat> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionCond : public ExpressionFixedArity<ExpressionCond, 3> {
        typedef ExpressionFixedArity<ExpressionCond, 3> Base;
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;

        static intrusive_ptr<Expression> parse(BSONElement expr);
    };


    class ExpressionConstant : public Expression {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<Expression> optimize();
        virtual void addDependencies(set<string>& deps, vector<string>* path=NULL) const;
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
        virtual Value serialize() const;

        static intrusive_ptr<Expression> parse(BSONElement bsonExpr);
        static intrusive_ptr<ExpressionConstant> create(const Value& pValue);

        /*
          Get the constant value represented by this Expression.

          @returns the value
         */
        Value getValue() const;

    private:
        ExpressionConstant(const Value& pValue);

        Value pValue;
    };


    class ExpressionDayOfMonth : public ExpressionFixedArity<ExpressionDayOfMonth, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionDayOfWeek : public ExpressionFixedArity<ExpressionDayOfWeek, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionDayOfYear : public ExpressionFixedArity<ExpressionDayOfYear, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionDivide : public ExpressionFixedArity<ExpressionDivide, 2> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionFieldPath : public Expression {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<Expression> optimize();
        virtual void addDependencies(set<string>& deps, vector<string>* path=NULL) const;
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual Value serialize() const;

        /*
          Create a field path expression using old semantics (rooted off of CURRENT).

          // NOTE: this method is deprecated
          // TODO remove this method in favor of parse()

          Evaluation will extract the value associated with the given field
          path from the source document.

          @param fieldPath the field path string, without any leading document
            indicator
          @returns the newly created field path expression
         */
        static intrusive_ptr<ExpressionFieldPath> create(const string& fieldPath);

        /// Like create(), but works with the raw string from the user with the "$" prefixes.
        static intrusive_ptr<ExpressionFieldPath> parse(const string& raw);

        const FieldPath& getFieldPath() const { return _fieldPath; }

    private:
        ExpressionFieldPath(const string &fieldPath);

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

        // A cache of string comparison of _fieldPath.getFieldName(0)
        enum BaseVar {
            CURRENT,
            ROOT,
            OTHER,
        };

        const FieldPath _fieldPath;
        const BaseVar _baseVar;
    };


    class ExpressionFieldRange : public Expression {
    public:
        // virtuals from expression
        virtual intrusive_ptr<Expression> optimize();
        virtual void addDependencies(set<string>& deps, vector<string>* path=NULL) const;
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual Value serialize() const;
        virtual void toMatcherBson(BSONObjBuilder *pBuilder) const;

        /*
          Create a field range expression.

          Field ranges are meant to match up with classic Matcher semantics,
          and therefore are conjunctions.  For example, these appear in
          mongo shell predicates in one of these forms:
          { a : C } -> (a == C) // degenerate "point" range
          { a : { $lt : C } } -> (a < C) // open range
          { a : { $gt : C1, $lte : C2 } } -> ((a > C1) && (a <= C2)) // closed

          When initially created, a field range only includes one end of
          the range.  Additional points may be added via intersect().

          Note that NE and CMP are not supported.

          @param pFieldPath the field path for extracting the field value
          @param cmpOp the comparison operator
          @param pValue the value to compare against
          @returns the newly created field range expression
         */
        static intrusive_ptr<ExpressionFieldRange> create(
            const intrusive_ptr<ExpressionFieldPath> &pFieldPath,
            CmpOp cmpOp, const Value& pValue);

        /*
          Add an intersecting range.

          This can be done any number of times after creation.  The
          range is internally optimized for each new addition.  If the new
          intersection extends or reduces the values within the range, the
          internal representation is adjusted to reflect that.

          Note that NE and CMP are not supported.

          @param cmpOp the comparison operator
          @param pValue the value to compare against
         */
        void intersect(CmpOp cmpOp, const Value& pValue);

    private:
        ExpressionFieldRange(const intrusive_ptr<ExpressionFieldPath> &pFieldPath,
                             CmpOp cmpOp,
                             const Value& pValue);

        intrusive_ptr<ExpressionFieldPath> pFieldPath;

        class Range {
        public:
            Range(CmpOp cmpOp, const Value& pValue);
            Range(const Range &rRange);

            Range *intersect(const Range *pRange) const;
            bool contains(const Value& pValue) const;

            Range(const Value& pBottom, bool bottomOpen,
                  const Value& pTop, bool topOpen);

            bool bottomOpen;
            bool topOpen;
            Value pBottom;
            Value pTop;
        };

        scoped_ptr<Range> pRange;
    };


    class ExpressionHour : public ExpressionFixedArity<ExpressionHour, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionIfNull : public ExpressionFixedArity<ExpressionIfNull, 2> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionLet : public Expression {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<Expression> optimize();
        virtual Value serialize() const;
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual void addDependencies(set<string>& deps, vector<string>* path=NULL) const;

        static intrusive_ptr<Expression> parse(BSONElement expr);

        typedef map<string, intrusive_ptr<Expression> > VariableMap;

    private:
        ExpressionLet(const VariableMap& vars, intrusive_ptr<Expression> subExpression);

        VariableMap _variables;
        intrusive_ptr<Expression> _subExpression;
    };

    class ExpressionMap : public Expression {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<Expression> optimize();
        virtual Value serialize() const;
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual void addDependencies(set<string>& deps, vector<string>* path=NULL) const;

        static intrusive_ptr<Expression> parse(BSONElement expr);

    private:
        ExpressionMap(const string& varName, // name of variable to set
                      intrusive_ptr<Expression> input, // yields array to iterate
                      intrusive_ptr<Expression> each); // yields results to be added to output array

        string _varName;
        intrusive_ptr<Expression> _input;
        intrusive_ptr<Expression> _each;
    };

    class ExpressionMillisecond : public ExpressionFixedArity<ExpressionMillisecond, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char* getOpName() const;
    };


    class ExpressionMinute : public ExpressionFixedArity<ExpressionMinute, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionMod : public ExpressionFixedArity<ExpressionMod, 2> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };
    

    class ExpressionMultiply : public ExpressionVariadic<ExpressionMultiply> {
    public:
        // virtuals from Expression
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
        virtual bool isAssociativeAndCommutative() const { return true; }
    };


    class ExpressionMonth : public ExpressionFixedArity<ExpressionMonth, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionNot : public ExpressionFixedArity<ExpressionNot, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionObject : public Expression {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<Expression> optimize();
        virtual bool isSimple();
        virtual void addDependencies(set<string>& deps, vector<string>* path=NULL) const;
        /** Only evaluates non inclusion expressions.  For inclusions, use addToDocument(). */
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual Value serialize() const;

        /// like evaluate(), but return a Document instead of a Value-wrapped Document.
        Document evaluateDocument(const Variables& vars) const;

        /** Evaluates with inclusions and adds results to passed in Mutable document
         *
         *  @param output the MutableDocument to add the evaluated expressions to
         *  @param currentDoc the input Document for this level (for inclusions)
         *  @param vars the variables for use in subexpressions
         */
        void addToDocument(MutableDocument& ouput,
                           const Document& currentDoc,
                           const Variables& vars
                          ) const;

        // estimated number of fields that will be output
        size_t getSizeHint() const;

        /** Create an empty expression.
         *  Until fields are added, this will evaluate to an empty document.
         */
        static intrusive_ptr<ExpressionObject> create();

        /// Like create but uses special handling of _id for root object of $project.
        static intrusive_ptr<ExpressionObject> createRoot();

        /*
          Add a field to the document expression.

          @param fieldPath the path the evaluated expression will have in the
                 result Document
          @param pExpression the expression to evaluate obtain this field's
                 Value in the result Document
        */
        void addField(const FieldPath &fieldPath,
                      const intrusive_ptr<Expression> &pExpression);

        /*
          Add a field path to the set of those to be included.

          Note that including a nested field implies including everything on
          the path leading down to it.

          @param fieldPath the name of the field to be included
        */
        void includePath(const string &fieldPath);

        /*
          Get a count of the added fields.

          @returns how many fields have been added
         */
        size_t getFieldCount() const;

        /*
          Specialized BSON conversion that allows for writing out a
          $project specification.  This creates a standalone object, which must
          be added to a containing object with a name

          @param pBuilder where to write the object to
          @param requireExpression see Expression::addToBsonObj
         */
        void documentToBson(BSONObjBuilder *pBuilder,
                            bool requireExpression) const;

        /*
          Visitor abstraction used by emitPaths().  Each path is recorded by
          calling path().
         */
        class PathSink {
        public:
            virtual ~PathSink() {};

            /**
               Record a path.

               @param path the dotted path string
               @param include if true, the path is included; if false, the path
                 is excluded
             */
            virtual void path(const string &path, bool include) = 0;
        };

        void excludeId(bool b) { _excludeId = b; }

    private:
        ExpressionObject(bool atRoot);

        // Mapping from fieldname to the Expression that generates its value.
        // NULL expression means inclusion from source document.
        typedef map<string, intrusive_ptr<Expression> > FieldMap;
        FieldMap _expressions;

        // this is used to maintain order for generated fields not in the source document
        vector<string> _order;

        bool _excludeId;
        bool _atRoot;
    };


    class ExpressionOr : public ExpressionVariadic<ExpressionOr> {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<Expression> optimize();
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
        virtual void toMatcherBson(BSONObjBuilder *pBuilder) const;
        virtual bool isAssociativeAndCommutative() const { return true; }
    };


    class ExpressionSecond : public ExpressionFixedArity<ExpressionSecond, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionSetDifference : public ExpressionFixedArity<ExpressionSetDifference, 2> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionSetEquals : public ExpressionVariadic<ExpressionSetEquals> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
        virtual void validateArguments(const ExpressionVector& args) const;
    };


    class ExpressionSetIntersection : public ExpressionVariadic<ExpressionSetIntersection> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
        virtual bool isAssociativeAndCommutative() const { return true; }
    };


    class ExpressionSetIsSubset : public ExpressionFixedArity<ExpressionSetIsSubset, 2> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionSetUnion : public ExpressionVariadic<ExpressionSetUnion> {
    public:
        // virtuals from ExpressionNary
        // virtual intrusive_ptr<Expression> optimize();
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
        virtual bool isAssociativeAndCommutative() const { return true; }
    };


    class ExpressionSize : public ExpressionFixedArity<ExpressionSize, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionStrcasecmp : public ExpressionFixedArity<ExpressionStrcasecmp, 2> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionSubstr : public ExpressionFixedArity<ExpressionSubstr, 3> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionSubtract : public ExpressionFixedArity<ExpressionSubtract, 2> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionToLower : public ExpressionFixedArity<ExpressionToLower, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionToUpper : public ExpressionFixedArity<ExpressionToUpper, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionWeek : public ExpressionFixedArity<ExpressionWeek, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };


    class ExpressionYear : public ExpressionFixedArity<ExpressionYear, 1> {
    public:
        // virtuals from ExpressionNary
        virtual Value evaluateInternal(const Variables& vars) const;
        virtual const char *getOpName() const;
    };
}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline int Expression::signum(int i) {
        if (i < 0)
            return -1;
        if (i > 0)
            return 1;
        return 0;
    }

    inline Value ExpressionConstant::getValue() const {
        return pValue;
    }

    inline size_t ExpressionObject::getFieldCount() const {
        return _expressions.size();
    }
}
