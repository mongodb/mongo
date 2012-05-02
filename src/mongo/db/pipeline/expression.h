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
 */

#pragma once

#include "pch.h"

#include "db/pipeline/field_path.h"
#include "util/intrusive_counter.h"
#include "util/iterator.h"


namespace mongo {

    class BSONArrayBuilder;
    class BSONElement;
    class BSONObjBuilder;
    class Builder;
    class DependencyTracker;
    class Document;
    class DocumentSource;
    class ExpressionContext;
    class Value;


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
           Add this expression's field dependencies to the dependency tracker.

           Expressions are trees, so this is often recursive.

           @params pTracker the tracker to add the dependencies to
         */
        virtual void addDependencies(
            const intrusive_ptr<DependencyTracker> &pTracker,
            const DocumentSource *pSource) const = 0;

        /*
          Evaluate the Expression using the given document as input.

          @returns the computed value
        */
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const = 0;

        /*
          Add the Expression (and any descendant Expressions) into a BSON
          object that is under construction.

          Unevaluated Expressions always materialize as objects.  Evaluation
          may produce a scalar or another object, either of which will be
          substituted inline.

          @param pBuilder the builder to add the expression to
          @param fieldName the name the object should be given
          @param requireExpression specify true if the value must appear
            as an expression; this is used by DocumentSources like
            $project which distinguish between field inclusion and virtual
            field specification;  See ExpressionConstant.
         */
        virtual void addToBsonObj(
            BSONObjBuilder *pBuilder, string fieldName,
            bool requireExpression) const = 0;

        /*
          Add the Expression (and any descendant Expressions) into a BSON
          array that is under construction.

          Unevaluated Expressions always materialize as objects.  Evaluation
          may produce a scalar or another object, either of which will be
          substituted inline.

          @param pBuilder the builder to add the expression to
         */
        virtual void addToBsonArray(BSONArrayBuilder *pBuilder) const = 0;

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

        /*
          Utility class for parseObject() below.

          Only one array can be unwound in a processing pipeline.  If the
          UNWIND_OK option is used, unwindOk() will return true, and a field
          can be declared as unwound using unwind(), after which unwindUsed()
          will return true.  Only specify UNWIND_OK if it is OK to unwind an
          array in the current context.

          DOCUMENT_OK indicates that it is OK to use a Document in the current
          context.
         */
        class ObjectCtx {
        public:
            ObjectCtx(int options);
            static const int UNWIND_OK = 0x0001;
            static const int DOCUMENT_OK = 0x0002;

            bool unwindOk() const;
            bool unwindUsed() const;
            void unwind(string fieldName);

            bool documentOk() const;

        private:
            int options;
            string unwindField;
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

        static const char unwindName[];

        /*
          Parse a BSONElement Object which has already been determined to be
          functional expression.

          @param pOpName the name of the (prefix) operator
          @param pBsonElement the BSONElement to parse
          @returns the parsed Expression
        */
        static intrusive_ptr<Expression> parseExpression(
            const char *pOpName, BSONElement *pBsonElement);


        /*
          Parse a BSONElement which is an operand in an Expression.

          @param pBsonElement the expected operand's BSONElement
          @returns the parsed operand, as an Expression
         */
        static intrusive_ptr<Expression> parseOperand(
            BSONElement *pBsonElement);

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

    protected:
        typedef vector<intrusive_ptr<Expression> > ExpressionVector;

    };


    class ExpressionNary :
        public Expression {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<Expression> optimize();
        virtual void addToBsonObj(
            BSONObjBuilder *pBuilder, string fieldName,
            bool requireExpression) const;
        virtual void addToBsonArray(BSONArrayBuilder *pBuilder) const;
        virtual void addDependencies(
            const intrusive_ptr<DependencyTracker> &pTracker,
            const DocumentSource *pSource) const;

        /*
          Add an operand to the n-ary expression.

          @param pExpression the expression to add
        */
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        /*
          Return a factory function that will make Expression nodes of
          the same type as this.  This will be used to create constant
          expressions for constant folding for optimize().  Only return
          a factory function if this operator is both associative and
          commutative.  The default implementation returns NULL; optimize()
          will recognize that and stop.

          Note that ExpressionNary::optimize() promises that if it uses this
          to fold constants, then if optimize() returns an ExpressionNary,
          any remaining constant will be the last one in vpOperand.  Derived
          classes may take advantage of this to do further optimizations in
          their optimize().

          @returns pointer to a factory function or NULL
         */
        virtual intrusive_ptr<ExpressionNary> (*getFactory() const)();

        /*
          Get the name of the operator.

          @returns the name of the operator; this string belongs to the class
            implementation, and should not be deleted
            and should not
        */
        virtual const char *getOpName() const = 0;

    protected:
        ExpressionNary();

        ExpressionVector vpOperand;

        /*
          Add the expression to the builder.

          If there is only one operand (a unary operator), then the operand
          is added directly, without an array.  For more than one operand,
          a named array is created.  In both cases, the result is an object.

          @param pBuilder the (blank) builder to add the expression to
          @param pOpName the name of the operator
         */
        virtual void toBson(BSONObjBuilder *pBuilder,
                            const char *pOpName) const;

        /*
          Checks the current size of vpOperand; if the size equal to or
          greater than maxArgs, fires a user assertion indicating that this
          operator cannot have this many arguments.

          The equal is there because this is intended to be used in
          addOperand() to check for the limit *before* adding the requested
          argument.

          @param maxArgs the maximum number of arguments the operator accepts
        */
        void checkArgLimit(unsigned maxArgs) const;

        /*
          Checks the current size of vpOperand; if the size is not equal to
          reqArgs, fires a user assertion indicating that this must have
          exactly reqArgs arguments.

          This is meant to be used in evaluate(), *before* the evaluation
          takes place.

          @param reqArgs the number of arguments this operator requires
        */
        void checkArgCount(unsigned reqArgs) const;
    };


    class ExpressionAdd :
        public ExpressionNary {
    public:
        // virtuals from Expression
        virtual ~ExpressionAdd();
        virtual intrusive_ptr<Expression> optimize();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;

        // virtuals from ExpressionNary
        virtual intrusive_ptr<ExpressionNary> (*getFactory() const)();

        /*
          Create an expression that finds the sum of n operands.

          @returns addition expression
         */
        static intrusive_ptr<ExpressionNary> create();

    protected:
        // virtuals from ExpressionNary
        virtual void toBson(BSONObjBuilder *pBuilder,
                            const char *pOpName) const;

    private:
        ExpressionAdd();

        /*
          If the operator can be optimized, we save the original here.

          This is necessary because addition must follow its original operand
          ordering strictly if a string is detected, otherwise string
          concatenation may appear to have re-ordered the operands.
         */
        intrusive_ptr<ExpressionAdd> pAdd;
        mutable bool useOriginal;
    };


    class ExpressionAnd :
        public ExpressionNary {
    public:
        // virtuals from Expression
        virtual ~ExpressionAnd();
        virtual intrusive_ptr<Expression> optimize();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void toMatcherBson(BSONObjBuilder *pBuilder) const;

        // virtuals from ExpressionNary
        virtual intrusive_ptr<ExpressionNary> (*getFactory() const)();

        /*
          Create an expression that finds the conjunction of n operands.
          The conjunction uses short-circuit logic; the expressions are
          evaluated in the order they were added to the conjunction, and
          the evaluation stops and returns false on the first operand that
          evaluates to false.

          @returns conjunction expression
         */
        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionAnd();
    };


    class ExpressionCoerceToBool :
        public Expression {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionCoerceToBool();
        virtual intrusive_ptr<Expression> optimize();
        virtual void addDependencies(
            const intrusive_ptr<DependencyTracker> &pTracker,
            const DocumentSource *pSource) const;
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual void addToBsonObj(
            BSONObjBuilder *pBuilder, string fieldName,
            bool requireExpression) const;
        virtual void addToBsonArray(BSONArrayBuilder *pBuilder) const;

        static intrusive_ptr<ExpressionCoerceToBool> create(
            const intrusive_ptr<Expression> &pExpression);

    private:
        ExpressionCoerceToBool(const intrusive_ptr<Expression> &pExpression);

        intrusive_ptr<Expression> pExpression;
    };


    class ExpressionCompare :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionCompare();
        virtual intrusive_ptr<Expression> optimize();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        /*
          Shorthands for creating various comparisons expressions.
          Provide for conformance with the uniform function pointer signature
          required for parsing.

          These create a particular comparision operand, without any
          operands.  Those must be added via ExpressionNary::addOperand().
        */
        static intrusive_ptr<ExpressionNary> createCmp();
        static intrusive_ptr<ExpressionNary> createEq();
        static intrusive_ptr<ExpressionNary> createNe();
        static intrusive_ptr<ExpressionNary> createGt();
        static intrusive_ptr<ExpressionNary> createGte();
        static intrusive_ptr<ExpressionNary> createLt();
        static intrusive_ptr<ExpressionNary> createLte();

    private:
        friend class ExpressionFieldRange;
        ExpressionCompare(CmpOp cmpOp);

        CmpOp cmpOp;
    };


    class ExpressionCond :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionCond();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionCond();
    };


    class ExpressionConstant :
        public Expression {
    public:
        // virtuals from Expression
        virtual ~ExpressionConstant();
        virtual intrusive_ptr<Expression> optimize();
        virtual void addDependencies(
            const intrusive_ptr<DependencyTracker> &pTracker,
            const DocumentSource *pSource) const;
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addToBsonObj(
            BSONObjBuilder *pBuilder, string fieldName,
            bool requireExpression) const;
        virtual void addToBsonArray(BSONArrayBuilder *pBuilder) const;

        static intrusive_ptr<ExpressionConstant> createFromBsonElement(
            BSONElement *pBsonElement);
        static intrusive_ptr<ExpressionConstant> create(
            const intrusive_ptr<const Value> &pValue);

        /*
          Get the constant value represented by this Expression.

          @returns the value
         */
        intrusive_ptr<const Value> getValue() const;

    private:
        ExpressionConstant(BSONElement *pBsonElement);
        ExpressionConstant(const intrusive_ptr<const Value> &pValue);

        intrusive_ptr<const Value> pValue;
    };


    class ExpressionDayOfMonth :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionDayOfMonth();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionDayOfMonth();
    };


    class ExpressionDayOfWeek :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionDayOfWeek();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionDayOfWeek();
    };


    class ExpressionDayOfYear :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionDayOfYear();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionDayOfYear();
    };


    class ExpressionDivide :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionDivide();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionDivide();
    };


    class ExpressionFieldPath :
        public Expression {
    public:
        // virtuals from Expression
        virtual ~ExpressionFieldPath();
        virtual intrusive_ptr<Expression> optimize();
        virtual void addDependencies(
            const intrusive_ptr<DependencyTracker> &pTracker,
            const DocumentSource *pSource) const;
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual void addToBsonObj(
            BSONObjBuilder *pBuilder, string fieldName,
            bool requireExpression) const;
        virtual void addToBsonArray(BSONArrayBuilder *pBuilder) const;

        /*
          Create a field path expression.

          Evaluation will extract the value associated with the given field
          path from the source document.

          @param fieldPath the field path string, without any leading document
            indicator
          @returns the newly created field path expression
         */
        static intrusive_ptr<ExpressionFieldPath> create(
            const string &fieldPath);

        /*
          Return a string representation of the field path.

          @param fieldPrefix whether or not to include the document field
            indicator prefix
          @returns the dot-delimited field path
         */
        string getFieldPath(bool fieldPrefix) const;

        /*
          Write a string representation of the field path to a stream.

          @param the stream to write to
          @param fieldPrefix whether or not to include the document field
            indicator prefix
         */
        void writeFieldPath(ostream &outStream, bool fieldPrefix) const;

    private:
        ExpressionFieldPath(const string &fieldPath);

        /*
          Internal implementation of evaluate(), used recursively.

          The internal implementation doesn't just use a loop because of
          the possibility that we need to skip over an array.  If the path
          is "a.b.c", and a is an array, then we fan out from there, and
          traverse "b.c" for each element of a:[...].  This requires that
          a be an array of objects in order to navigate more deeply.

          @param index current path field index to extract
          @param pathLength maximum number of fields on field path
          @param pDocument current document traversed to (not the top-level one)
          @returns the field found; could be an array
         */
        intrusive_ptr<const Value> evaluatePath(
            size_t index, const size_t pathLength, 
            intrusive_ptr<Document> pDocument) const;

        FieldPath fieldPath;
    };


    class ExpressionFieldRange :
        public Expression {
    public:
        // virtuals from expression
        virtual ~ExpressionFieldRange();
        virtual intrusive_ptr<Expression> optimize();
        virtual void addDependencies(
            const intrusive_ptr<DependencyTracker> &pTracker,
            const DocumentSource *pSource) const;
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual void addToBsonObj(
            BSONObjBuilder *pBuilder, string fieldName,
            bool requireExpression) const;
        virtual void addToBsonArray(BSONArrayBuilder *pBuilder) const;
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
            CmpOp cmpOp, const intrusive_ptr<const Value> &pValue);

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
        void intersect(CmpOp cmpOp, const intrusive_ptr<const Value> &pValue);

    private:
        ExpressionFieldRange(const intrusive_ptr<ExpressionFieldPath> &pFieldPath,
                             CmpOp cmpOp,
                             const intrusive_ptr<const Value> &pValue);

        intrusive_ptr<ExpressionFieldPath> pFieldPath;

        class Range {
        public:
            Range(CmpOp cmpOp, const intrusive_ptr<const Value> &pValue);
            Range(const Range &rRange);

            Range *intersect(const Range *pRange) const;
            bool contains(const intrusive_ptr<const Value> &pValue) const;

            Range(const intrusive_ptr<const Value> &pBottom, bool bottomOpen,
                  const intrusive_ptr<const Value> &pTop, bool topOpen);

            bool bottomOpen;
            bool topOpen;
            intrusive_ptr<const Value> pBottom;
            intrusive_ptr<const Value> pTop;
        };

        scoped_ptr<Range> pRange;

        /*
          Add to a generic Builder.

          The methods to append items to an object and an array differ by
          their inclusion of a field name.  For more complicated objects,
          it makes sense to abstract that out and use a generic builder that
          always looks the same, and then implement addToBsonObj() and
          addToBsonArray() by using the common method.
         */
        void addToBson(Builder *pBuilder) const;
    };


    class ExpressionHour :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionHour();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionHour();
    };


    class ExpressionIfNull :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionIfNull();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionIfNull();
    };


    class ExpressionIsoDate :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionIsoDate();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionIsoDate();

        static const char argYear[];
        static const char argMonth[];
        static const char argDayOfMonth[];
        static const char argHour[];
        static const char argMinute[];
        static const char argSecond[];

        static const unsigned flagYear;
        static const unsigned flagMonth;
        static const unsigned flagDayOfMonth;
        static const unsigned flagHour;
        static const unsigned flagMinute;
        static const unsigned flagSecond;
        unsigned flag;

        /**
           Get a named long argument out of the given document.

           @param pArgs the evaluated document with the named arguments in it
           @param pName the name of the argument
           @param defaultValue the value to return if the argument isn't found
           @returns the value if found, otherwise zero
           @throws uassert for non-whole numbers or non-numbers
         */
        int getIntArg(
            const intrusive_ptr<Document> &pArgs,
            const char *pName, int defaultValue) const;

        /**
           Check that the named argument fits in an integer.

           @params pName the name of the argument
           @params value the long value of the argument
           @returns the integer value
           @throws uassert if the value is out of range
         */
        int checkIntRange(const char *pName, long long value) const;
    };


    class ExpressionMinute :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionMinute();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionMinute();
    };


    class ExpressionMod :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionMod();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionMod();
    };
    

    class ExpressionMultiply :
        public ExpressionNary {
    public:
        // virtuals from Expression
        virtual ~ExpressionMultiply();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;

        // virtuals from ExpressionNary
        virtual intrusive_ptr<ExpressionNary> (*getFactory() const)();

        /*
          Create an expression that finds the product of n operands.

          @returns multiplication expression
         */
        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionMultiply();
    };


    class ExpressionMonth :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionMonth();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionMonth();
    };


    class ExpressionNoOp :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionNoOp();
        virtual intrusive_ptr<Expression> optimize();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionNoOp();
    };


    class ExpressionNot :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionNot();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionNot();
    };


    class ExpressionObject :
        public Expression {
    public:
        // virtuals from Expression
        virtual ~ExpressionObject();
        virtual intrusive_ptr<Expression> optimize();
        virtual void addDependencies(
            const intrusive_ptr<DependencyTracker> &pTracker,
            const DocumentSource *pSource) const;
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual void addToBsonObj(
            BSONObjBuilder *pBuilder, string fieldName,
            bool requireExpression) const;
        virtual void addToBsonArray(BSONArrayBuilder *pBuilder) const;

        /*
          evaluate(), but return a Document instead of a Value-wrapped
          Document.

          @param pDocument the input Document
          @returns the result document
         */
        intrusive_ptr<Document> evaluateDocument(
            const intrusive_ptr<Document> &pDocument) const;

        /*
          evaluate(), but add the evaluated fields to a given document
          instead of creating a new one.

          @param pResult the Document to add the evaluated expressions to
          @param pDocument the input Document
          @param excludeId for exclusions, exclude the _id, if present
         */
        void addToDocument(const intrusive_ptr<Document> &pResult,
                           const intrusive_ptr<Document> &pDocument,
            bool excludeId = false) const;

        /*
          Estimate the number of fields that will result from evaluating
          this over pDocument.  Does not include _id.  This is an estimate
          (really an upper bound) because we can't account for undefined
          fields without actually doing the evaluation.  But this is still
          useful as an argument to Document::create(), if you plan to use
          addToDocument().

          @param pDocument the input document
          @returns estimated number of fields that will result
         */
        size_t getSizeHint(const intrusive_ptr<Document> &pDocument) const;

        /*
          Create an empty expression.  Until fields are added, this
          will evaluate to an empty document (object).
         */
        static intrusive_ptr<ExpressionObject> create();

        /*
          Add a field to the document expression.

          @param fieldPath the path the evaluated expression will have in the
                 result Document
          @param pExpression the expression to evaluate obtain this field's
                 Value in the result Document
        */
        void addField(const string &fieldPath,
                      const intrusive_ptr<Expression> &pExpression);

        /*
          Add a field path to the set of those to be included.

          Note that including a nested field implies including everything on
          the path leading down to it.

          @param fieldPath the name of the field to be included
        */
        void includePath(const string &fieldPath);

        /*
          Add a field path to the set of those to be excluded.

          Note that excluding a nested field implies including everything on
          the path leading down to it (because you're stating you want to see
          all the other fields that aren't being excluded).

          @param fieldName the name of the field to be excluded
         */
        void excludePath(const string &fieldPath);

        /**
           Get an iterator that can be used to iterate over all the result
           field names in this ExpressionObject.

           @returns the (intrusive_ptr'ed) iterator
         */
        Iterator<string> *getFieldIterator() const;

        /*
          Return the expression for a field.

          @param fieldName the field name for the expression to return
          @returns the expression used to compute the field, if it is present,
            otherwise NULL.
        */
        intrusive_ptr<Expression> getField(const string &fieldName) const;

        /*
          Get a count of the added fields.

          @returns how many fields have been added
         */
        size_t getFieldCount() const;

        /*
          Get a count of the exclusions.

          @returns how many fields have been excluded.
        */
        size_t getExclusionCount() const;

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

        /**
          Emit the field paths that have been included or excluded.  "Included"
          includes paths that are referenced in expressions for computed
          fields.

          @param pSink where to write the paths to
          @param pvPath pointer to a vector of strings describing the path on
            descent; the top-level call should pass an empty vector
         */
        void emitPaths(PathSink *pPathSink) const;

    private:
        ExpressionObject();

        void includePath(
            const FieldPath *pPath, size_t pathi, size_t pathn,
            bool excludeLast);

        bool excludePaths;
        set<string> path;

        /* these two vectors are maintained in parallel */
        vector<string> vFieldName;
        vector<intrusive_ptr<Expression> > vpExpression;


        /*
          Utility function used by documentToBson().  Emits inclusion
          and exclusion paths by recursively walking down the nested
          ExpressionObject trees these have created.

          @param pSink where to write the paths to
          @param pvPath pointer to a vector of strings describing the path on
            descent; the top-level call should pass an empty vector
         */
        void emitPaths(PathSink *pPathSink, vector<string> *pvPath) const;

        /*
          Utility object for collecting emitPaths() results in a BSON
          object.
         */
        class BuilderPathSink :
            public PathSink {
        public:
            // virtuals from PathSink
            virtual void path(const string &path, bool include);

            /*
              Create a PathSink that writes paths to a BSONObjBuilder,
              to create an object in the form of { path:is_included,...}

              This object uses a builder pointer that won't guarantee the
              lifetime of the builder, so make sure it outlasts the use of
              this for an emitPaths() call.

              @param pBuilder to the builder to write paths to
             */
            BuilderPathSink(BSONObjBuilder *pBuilder);

        private:
            BSONObjBuilder *pBuilder;
        };

        /* utility class used by emitPaths() */
        class PathPusher :
            boost::noncopyable {
        public:
            PathPusher(vector<string> *pvPath, const string &s);
            ~PathPusher();

        private:
            vector<string> *pvPath;
        };
    };


    class ExpressionOr :
        public ExpressionNary {
    public:
        // virtuals from Expression
        virtual ~ExpressionOr();
        virtual intrusive_ptr<Expression> optimize();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void toMatcherBson(BSONObjBuilder *pBuilder) const;

        // virtuals from ExpressionNary
        virtual intrusive_ptr<ExpressionNary> (*getFactory() const)();

        /*
          Create an expression that finds the conjunction of n operands.
          The conjunction uses short-circuit logic; the expressions are
          evaluated in the order they were added to the conjunction, and
          the evaluation stops and returns false on the first operand that
          evaluates to false.

          @returns conjunction expression
         */
        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionOr();
    };


    class ExpressionSecond :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionSecond();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionSecond();
    };


    class ExpressionStrcasecmp :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionStrcasecmp();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionStrcasecmp();
    };


    class ExpressionSubstr :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionSubstr();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionSubstr();
    };


    class ExpressionSubtract :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionSubtract();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionSubtract();
    };


    class ExpressionToLower :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionToLower();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionToLower();
    };


    class ExpressionToUpper :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionToUpper();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionToUpper();
    };


    class ExpressionWeek :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionWeek();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionWeek();
    };


    class ExpressionYear :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionYear();
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);

        static intrusive_ptr<ExpressionNary> create();

    private:
        ExpressionYear();
    };
}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline bool Expression::ObjectCtx::unwindOk() const {
        return ((options & UNWIND_OK) != 0);
    }

    inline bool Expression::ObjectCtx::unwindUsed() const {
        return (unwindField.size() != 0);
    }

    inline int Expression::signum(int i) {
        if (i < 0)
            return -1;
        if (i > 0)
            return 1;
        return 0;
    }

    inline intrusive_ptr<const Value> ExpressionConstant::getValue() const {
        return pValue;
    }

    inline string ExpressionFieldPath::getFieldPath(bool fieldPrefix) const {
        return fieldPath.getPath(fieldPrefix);
    }

    inline void ExpressionFieldPath::writeFieldPath(
        ostream &outStream, bool fieldPrefix) const {
        return fieldPath.writePath(outStream, fieldPrefix);
    }

    inline size_t ExpressionObject::getFieldCount() const {
        return vFieldName.size();
    }

    inline ExpressionObject::BuilderPathSink::BuilderPathSink(
        BSONObjBuilder *pB):
        pBuilder(pB) {
    }

    inline ExpressionObject::PathPusher::PathPusher(
        vector<string> *pTheVPath, const string &s):
        pvPath(pTheVPath) {
        pvPath->push_back(s);
    }

    inline ExpressionObject::PathPusher::~PathPusher() {
        pvPath->pop_back();
    }

}
