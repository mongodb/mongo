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
    class BSONElement;
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
	  Convert the Expression (and any descendant Expressions) into
	  BSON.

	  @params pBuilder the builder to add the expression to
	  @params name the name the expression will be given in an object
	  @params docPrefix whether or not any referenced field names must
	    be preceded by "$document." to disambiguate them from string
	    constants
	 */
	virtual void toBson(
	    BSONObjBuilder *pBuilder, string name, bool docPrefix) const = 0;

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
    };


    class ExpressionNary :
	public Expression,
        public boost::enable_shared_from_this<ExpressionNary> {
    public:
        // virtuals from Expression
	virtual shared_ptr<Expression> optimize();

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

    protected:
        ExpressionNary();

	/*
	  Add the operands to the builder as an array.

	  If there is only one operand (a unary operator), then the operand
	  is added directly, without an array.

	  @params pBuilder the builder to add the operands to
	  @params opName the name of the operator
	  @params docPrefix whether or not to add the "$document." prefix to
	    field paths
	 */
	void operandsToBson(
	    BSONObjBuilder *pBuilder, string opName, bool docPrefix) const;

        vector<shared_ptr<Expression>> vpOperand;
    };


    class ExpressionAdd :
        public ExpressionNary,
        public boost::enable_shared_from_this<ExpressionAdd> {
    public:
        // virtuals from Expression
        virtual ~ExpressionAdd();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual void toBson(
	    BSONObjBuilder *pBuilder, string name, bool docPrefix) const;

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
	virtual void toBson(
	    BSONObjBuilder *pBuilder, string name, bool docPrefix) const;
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
	virtual void toBson(
	    BSONObjBuilder *pBuilder, string name, bool docPrefix) const;

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
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
        virtual void addOperand(shared_ptr<Expression> pExpression);
	virtual void toBson(
	    BSONObjBuilder *pBuilder, string name, bool docPrefix) const;

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
	virtual void toBson(
	    BSONObjBuilder *pBuilder, string name, bool docPrefix) const;

        static shared_ptr<ExpressionConstant> createFromBsonElement(
            BSONElement *pBsonElement);
	static shared_ptr<ExpressionConstant> create(
	    shared_ptr<const Value> pValue);

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
        virtual void addOperand(shared_ptr<Expression> pExpression);
	virtual void toBson(
	    BSONObjBuilder *pBuilder, string name, bool docPrefix) const;

        static shared_ptr<ExpressionNary> create();

    private:
        ExpressionDivide();
    };


    class ExpressionDocument :
        public Expression,
        public boost::enable_shared_from_this<ExpressionDocument> {
    public:
        // virtuals from Expression
        virtual ~ExpressionDocument();
	virtual shared_ptr<Expression> optimize();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual void toBson(
	    BSONObjBuilder *pBuilder, string name, bool docPrefix) const;

        /*
          Create an empty expression.  Until fields are added, this
          will evaluate to an empty document (object).
         */
        static shared_ptr<ExpressionDocument> create();

        /*
          Add a field to the document expression.

          @param fieldName the name the evaluated expression will have in the
                 result Document
          @param pExpression the expression to evaluate obtain this field's
                 Value in the result Document
        */
        void addField(string fieldName, shared_ptr<Expression> pExpression);

    private:
        ExpressionDocument();

        /* these two vectors are maintained in parallel */
        vector<string> vFieldName;
        vector<shared_ptr<Expression>> vpExpression;
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
	virtual void toBson(
	    BSONObjBuilder *pBuilder, string name, bool docPrefix) const;

        static shared_ptr<ExpressionFieldPath> create(string fieldPath);

    private:
        ExpressionFieldPath(string fieldPath);

        vector<string> vFieldPath;
    };


    class ExpressionFieldRange :
	public Expression {
    public:
	// virtuals from expression
        virtual ~ExpressionFieldRange();
	virtual shared_ptr<Expression> optimize();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual void toBson(
	    BSONObjBuilder *pBuilder, string name, bool docPrefix);
	virtual void toMatcherBson(BSONObjBuilder *pBuilder) const;

	static shared_ptr<ExpressionFieldRange> create();

    private:
	ExpressionFieldRange();

	shared_ptr<ExpressionFieldPath> pPath;
	CmpOp lowOp;
	CmpOp highOp;
	shared_ptr<const Value> pLower;
	shared_ptr<const Value> pUpper;
    };


    class ExpressionIfNull :
        public ExpressionNary,
        public boost::enable_shared_from_this<ExpressionIfNull> {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionIfNull();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
        virtual void addOperand(shared_ptr<Expression> pExpression);
	virtual void toBson(
	    BSONObjBuilder *pBuilder, string name, bool docPrefix) const;

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
        virtual void addOperand(shared_ptr<Expression> pExpression);
	virtual void toBson(
	    BSONObjBuilder *pBuilder, string name, bool docPrefix) const;

        static shared_ptr<ExpressionNary> create();

    private:
        ExpressionNot();
    };


    class ExpressionOr :
        public ExpressionNary {
    public:
        // virtuals from Expression
        virtual ~ExpressionOr();
	virtual shared_ptr<Expression> optimize();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
	virtual void toBson(
	    BSONObjBuilder *pBuilder, string name, bool docPrefix) const;
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

};
