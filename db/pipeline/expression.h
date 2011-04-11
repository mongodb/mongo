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

namespace mongo {
    class BSONArrayBuilder;
    class BSONElement;
    class BSONObjBuilder;
    class Document;
    class Value;

    class Expression :
            boost::noncopyable {
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
	virtual shared_ptr<Expression> optimize() = 0;

        /*
          Evaluate the Expression using the given document as input.

          @returns the computed value
        */
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const = 0;

	/*
	  Add the Expression (and any descendant Expressions) into a BSON
	  object that is under construction.

	  Unevaluated Expressions always materialize as objects.  Evaluation
	  may produce a scalar or another object, either of which will be
	  substituted inline.

	  @params pBuilder the builder to add the expression to
	  @params fieldName the name the object should be given
	  @params fieldPrefix whether or not any descendant field references
	    should have the field indicator prepended or not
	 */
	virtual void addToBsonObj(
	    BSONObjBuilder *pBuilder, string fieldName,
	    bool fieldPrefix) const = 0;

	/*
	  Add the Expression (and any descendant Expressions) into a BSON
	  array that is under construction.

	  Unevaluated Expressions always materialize as objects.  Evaluation
	  may produce a scalar or another object, either of which will be
	  substituted inline.

	  @params pBuilder the builder to add the expression to
	  @params fieldPrefix whether or not any descendant field references
	    should have the field indicator prepended or not
	 */
	virtual void addToBsonArray(
	    BSONArrayBuilder *pBuilder, bool fieldPrefix) const = 0;

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

	  @params pBuilder the builder to add the expression to.
	 */
	virtual void toMatcherBson(BSONObjBuilder *pBuilder) const;

	/*
	  Utility class for parseObject() below.

	  Only one array can be raveled in a processing pipeline.  If the
	  RAVEL_OK option is used, ravelOk() will return true, and a field
	  can be declared as raveled using ravel(), after which ravelUsed()
	  will return true.  Only specify RAVEL_OK if it is OK to ravel an
	  array in the current context.

	  DOCUMENT_OK indicates that it is OK to use a Document in the current
	  context.
	 */
        class ObjectCtx {
        public:
            ObjectCtx(int options);
            static const int RAVEL_OK = 0x0001;
            static const int DOCUMENT_OK = 0x0002;

            bool ravelOk() const;
            bool ravelUsed() const;
            void ravel(string fieldName);

            bool documentOk() const;

        private:
            int options;
            string raveledField;
        };

	/*
	  Parse a BSONElement Object.  The object could represent a functional
	  expression or a Document expression.

	  @param pBsonElement the element representing the object
	  @param pCtx a MiniCtx representing the options above
	  @returns the parsed Expression
	 */
        static shared_ptr<Expression> parseObject(
            BSONElement *pBsonElement, ObjectCtx *pCtx);

        /*
	  Parse a BSONElement Object which has already been determined to be
	  functional expression.

	  @param pOpName the name of the (prefix) operator
	  @param pBsonElement the BSONElement to parse
	  @returns the parsed Expression
	*/
        static shared_ptr<Expression> parseExpression(
            const char *pOpName, BSONElement *pBsonElement);


	/*
	  Parse a BSONElement which is an operand in an Expression.

	  @param pBsonElement the expected operand's BSONElement
	  @returns the parsed operand, as an Expression
	 */
        static shared_ptr<Expression> parseOperand(BSONElement *pBsonElement);

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
    };


    class ExpressionNary :
	public Expression,
        public boost::enable_shared_from_this<ExpressionNary> {
    public:
        // virtuals from Expression
	virtual shared_ptr<Expression> optimize();
	virtual void addToBsonObj(
	    BSONObjBuilder *pBuilder, string fieldName, bool fieldPrefix) const;
	virtual void addToBsonArray(
	    BSONArrayBuilder *pBuilder, bool fieldPrefix) const;

        /*
          Add an operand to the n-ary expression.

          @param pExpression the expression to add
        */
        virtual void addOperand(shared_ptr<Expression> pExpression);

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
	virtual shared_ptr<ExpressionNary> (*getFactory() const)();

	/*
	  Get the name of the operator.

	  @returns the name of the operator; this string belongs to the class
	    implementation, and should not be deleted
	    and should not
	*/
	virtual const char *getName() const = 0;

    protected:
        ExpressionNary();

        vector<shared_ptr<Expression> > vpOperand;

    private:
	/*
	  Add this expression as a field to the object under construction.

	  @params pBuilder the builder for the object under construction
	  @params pOpName the name of the expression's operator
	  @params fieldName the name to give this field in the object under
	    construction
	  @params fieldPrefix whether or not to add the field indicator prefix
	    to descendant field paths
	 */
	void expressionToField(
	    BSONObjBuilder *pBuilder, const char *pOpName,
	    string fieldName, bool fieldPrefix) const;

	/*
	  Add this expression as an element to the array under construction.

	  @params pBuilder the builder for the array under construction
	  @params pOpName the name of the expression's operator
	  @params fieldPrefix whether or not to add the field indicator prefix
	    to descendant field paths
	 */
	void expressionToElement(
	    BSONArrayBuilder *pBuilder, const char *pOpName,
	    bool fieldPrefix) const;

	/*
	  Add the expression to the builder.

	  If there is only one operand (a unary operator), then the operand
	  is added directly, without an array.  For more than one operand,
	  a named array is created.  In both cases, the result is an object.

	  @params pBuilder the (blank) builder to add the expression to
	  @params pOpName the name of the operator
	  @params fieldPrefix whether or not to add the field indicator prefix
	    to field paths
	 */
	void expressionToBson(
	    BSONObjBuilder *pBuilder, const char *pOpName, bool fieldPrefix) const;

    };


    class ExpressionAdd :
        public ExpressionNary,
        public boost::enable_shared_from_this<ExpressionAdd> {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionAdd();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual const char *getName() const;

	// virtuals from ExpressionNary
	virtual shared_ptr<ExpressionNary> (*getFactory() const)();

        /*
          Create an expression that finds the sum of n operands.

          @returns the n-ary expression
         */
        static shared_ptr<ExpressionNary> create();

    private:
        ExpressionAdd();
    };


    class ExpressionAnd :
        public ExpressionNary,
        public boost::enable_shared_from_this<ExpressionAnd> {
    public:
        // virtuals from Expression
        virtual ~ExpressionAnd();
	virtual shared_ptr<Expression> optimize();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual const char *getName() const;
	virtual void toMatcherBson(BSONObjBuilder *pBuilder) const;

	// virtuals from ExpressionNary
	virtual shared_ptr<ExpressionNary> (*getFactory() const)();

        /*
          Create an expression that finds the conjunction of n operands.
          The conjunction uses short-circuit logic; the expressions are
          evaluated in the order they were added to the conjunction, and
          the evaluation stops and returns false on the first operand that
          evaluates to false.

          @returns conjunction expression
         */
        static shared_ptr<ExpressionNary> create();

    private:
        ExpressionAnd();
    };


    class ExpressionCoerceToBool :
	public Expression,
        public boost::enable_shared_from_this<ExpressionCoerceToBool> {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionCoerceToBool();
	virtual shared_ptr<Expression> optimize();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual void addToBsonObj(
	    BSONObjBuilder *pBuilder, string fieldName, bool fieldPrefix) const;
	virtual void addToBsonArray(
	    BSONArrayBuilder *pBuilder, bool fieldPrefix) const;

        static shared_ptr<ExpressionCoerceToBool> create(
	    shared_ptr<Expression> pExpression);

    private:
        ExpressionCoerceToBool(shared_ptr<Expression> pExpression);

	shared_ptr<Expression> pExpression;
    };


    class ExpressionCompare :
        public ExpressionNary,
        public boost::enable_shared_from_this<ExpressionCompare> {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionCompare();
	virtual shared_ptr<Expression> optimize();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual const char *getName() const;
        virtual void addOperand(shared_ptr<Expression> pExpression);

        /*
          Shorthands for creating various comparisons expressions.
          Provide for conformance with the uniform function pointer signature
          required for parsing.

          These create a particular comparision operand, without any
          operands.  Those must be added via ExpressionNary::addOperand().
        */
        static shared_ptr<ExpressionNary> createCmp();
        static shared_ptr<ExpressionNary> createEq();
        static shared_ptr<ExpressionNary> createNe();
        static shared_ptr<ExpressionNary> createGt();
        static shared_ptr<ExpressionNary> createGte();
        static shared_ptr<ExpressionNary> createLt();
        static shared_ptr<ExpressionNary> createLte();

    private:
	friend class ExpressionFieldRange;
        ExpressionCompare(CmpOp cmpOp);

        CmpOp cmpOp;
    };

    class ExpressionConstant :
        public Expression,
        public boost::enable_shared_from_this<ExpressionConstant> {
    public:
        // virtuals from Expression
        virtual ~ExpressionConstant();
	virtual shared_ptr<Expression> optimize();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual const char *getName() const;
	virtual void addToBsonObj(
	    BSONObjBuilder *pBuilder, string fieldName, bool fieldPrefix) const;
	virtual void addToBsonArray(
	    BSONArrayBuilder *pBuilder, bool fieldPrefix) const;

        static shared_ptr<ExpressionConstant> createFromBsonElement(
            BSONElement *pBsonElement);
	static shared_ptr<ExpressionConstant> create(
	    shared_ptr<const Value> pValue);

	/*
	  Get the constant value represented by this Expression.

	  @returns the value
	 */
	shared_ptr<const Value> getValue() const;

    private:
        ExpressionConstant(BSONElement *pBsonElement);
	ExpressionConstant(shared_ptr<const Value> pValue);

        shared_ptr<const Value> pValue;
    };


    class ExpressionDivide :
        public ExpressionNary,
        public boost::enable_shared_from_this<ExpressionDivide> {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionDivide();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual const char *getName() const;
        virtual void addOperand(shared_ptr<Expression> pExpression);

        static shared_ptr<ExpressionNary> create();

    private:
        ExpressionDivide();
    };


    class ExpressionFieldPath :
        public Expression,
        public boost::enable_shared_from_this<ExpressionFieldPath> {
    public:
        // virtuals from Expression
        virtual ~ExpressionFieldPath();
	virtual shared_ptr<Expression> optimize();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual void addToBsonObj(
	    BSONObjBuilder *pBuilder, string fieldName, bool fieldPrefix) const;
	virtual void addToBsonArray(
	    BSONArrayBuilder *pBuilder, bool fieldPrefix) const;

	/*
	  Create a field path expression.

	  Evaluation will extract the value associated with the given field
	  path from the source document.

	  @param fieldPath the field path string, without any leading document
	    indicator
	  @returns the newly created field path expression
	 */
        static shared_ptr<ExpressionFieldPath> create(string fieldPath);

	/*
	  Return a string representation of the field path.

	  @param fieldPrefix whether or not to include the document field
	    indicator prefix
	  @returns the dot-delimited field path
	 */
	string getFieldPath(bool fieldPrefix) const;

    private:
        ExpressionFieldPath(string fieldPath);

        vector<string> vField;
    };


    class ExpressionFieldRange :
	public Expression,
	public boost::enable_shared_from_this<ExpressionFieldRange> {
    public:
	// virtuals from expression
        virtual ~ExpressionFieldRange();
	virtual shared_ptr<Expression> optimize();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual void addToBsonObj(
	    BSONObjBuilder *pBuilder, string fieldName, bool fieldPrefix) const;
	virtual void addToBsonArray(
	    BSONArrayBuilder *pBuilder, bool fieldPrefix) const;
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
	static shared_ptr<ExpressionFieldRange> create(
	    shared_ptr<ExpressionFieldPath> pFieldPath,
	    CmpOp cmpOp, shared_ptr<const Value> pValue);

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
	void intersect(CmpOp cmpOp, shared_ptr<const Value> pValue);

    private:
	ExpressionFieldRange(shared_ptr<ExpressionFieldPath> pFieldPath,
			     CmpOp cmpOp, shared_ptr<const Value> pValue);

	shared_ptr<ExpressionFieldPath> pFieldPath;

	class Range {
	public:
	    Range(CmpOp cmpOp, shared_ptr<const Value> pValue);
	    Range(const Range &rRange);

	    Range *intersect(const Range *pRange) const;
	    bool contains(shared_ptr<const Value> pValue) const;

	    Range(shared_ptr<const Value> pBottom, bool bottomOpen,
		  shared_ptr<const Value> pTop, bool topOpen);

	    bool bottomOpen;
	    bool topOpen;
	    shared_ptr<const Value> pBottom;
	    shared_ptr<const Value> pTop;
	};

	scoped_ptr<Range> pRange;

	/*
	  Here are some helpers for the implementations of addToBsonObj() and
	  addToBsonArray().  These two methods are really the same, except
	  for the final addition of the construction object, which must be
	  given a name for addToBsonObj(), but not for addToBsonArray().

	  Therefore, we create a wrapper builder abstraction which will always
	  take a name, but will drop it on the floor for the array case.
	  Both of these are then implemented in terms of addToBson(), and call
	  it with the appropriate builder wrapper.

	  The builder wrapper is kept as minimal as possible to meet the needs
	  of addToBson().
	 */
	class Builder {
	public:
	    virtual void append(bool b) = 0;
	    virtual void append(BSONObjBuilder *pDone) = 0;
	};

	class BuilderObj :
	    public Builder {
	public:
	    virtual void append(bool b);
	    virtual void append(BSONObjBuilder *pDone);

	    BuilderObj(BSONObjBuilder *pBuilder, string fieldName);

	private:
	    BSONObjBuilder *pBuilder;
	    string fieldName;
	};

	class BuilderArray :
	    public Builder {
	public:
	    virtual void append(bool b);
	    virtual void append(BSONObjBuilder *pDone);

	    BuilderArray(BSONArrayBuilder *pBuilder);

	private:
	    BSONArrayBuilder *pBuilder;
	};

	void addToBson(Builder *pBuilder, bool fieldPrefix) const;
    };


    class ExpressionIfNull :
        public ExpressionNary,
        public boost::enable_shared_from_this<ExpressionIfNull> {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionIfNull();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual const char *getName() const;
        virtual void addOperand(shared_ptr<Expression> pExpression);

        static shared_ptr<ExpressionNary> create();

    private:
        ExpressionIfNull();
    };


    class ExpressionNot :
        public ExpressionNary,
        public boost::enable_shared_from_this<ExpressionNot> {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionNot();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual const char *getName() const;
        virtual void addOperand(shared_ptr<Expression> pExpression);

        static shared_ptr<ExpressionNary> create();

    private:
        ExpressionNot();
    };


    class ExpressionObject :
        public Expression,
        public boost::enable_shared_from_this<ExpressionObject> {
    public:
        // virtuals from Expression
        virtual ~ExpressionObject();
	virtual shared_ptr<Expression> optimize();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual void addToBsonObj(
	    BSONObjBuilder *pBuilder, string fieldName, bool fieldPrefix) const;
	virtual void addToBsonArray(
	    BSONArrayBuilder *pBuilder, bool fieldPrefix) const;

        /*
          Create an empty expression.  Until fields are added, this
          will evaluate to an empty document (object).
         */
        static shared_ptr<ExpressionObject> create();

        /*
          Add a field to the document expression.

          @param fieldName the name the evaluated expression will have in the
                 result Document
          @param pExpression the expression to evaluate obtain this field's
                 Value in the result Document
        */
        void addField(string fieldName, shared_ptr<Expression> pExpression);

    private:
        ExpressionObject();

	void documentToBson(BSONObjBuilder *pBuilder, bool fieldPrefix) const;

        /* these two vectors are maintained in parallel */
        vector<string> vFieldName;
        vector<shared_ptr<Expression> > vpExpression;
    };


    class ExpressionOr :
        public ExpressionNary {
    public:
        // virtuals from Expression
        virtual ~ExpressionOr();
	virtual shared_ptr<Expression> optimize();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual const char *getName() const;
	virtual void toMatcherBson(BSONObjBuilder *pBuilder) const;

	// virtuals from ExpressionNary
	virtual shared_ptr<ExpressionNary> (*getFactory() const)();

        /*
          Create an expression that finds the conjunction of n operands.
          The conjunction uses short-circuit logic; the expressions are
          evaluated in the order they were added to the conjunction, and
          the evaluation stops and returns false on the first operand that
          evaluates to false.

          @returns conjunction expression
         */
        static shared_ptr<ExpressionNary> create();

    private:
        ExpressionOr();
    };
}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline bool Expression::ObjectCtx::ravelOk() const {
        return ((options & RAVEL_OK) != 0);
    }

    inline bool Expression::ObjectCtx::ravelUsed() const {
        return (raveledField.size() != 0);
    }

    inline int Expression::signum(int i) {
	if (i < 0)
	    return -1;
	if (i > 0)
	    return 1;
	return 0;
    }

    inline shared_ptr<const Value> ExpressionConstant::getValue() const {
	return pValue;
    }
};
