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
          Evaluate the expression using the given document as input.

          @return computed value
        */
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const = 0;
    };


    class ExpressionNary :
        public Expression {
    public:
        /*
          Add an operand to the n-ary expression.

          @param pExpression the expression to add
        */
        virtual void addOperand(shared_ptr<Expression> pExpression);

    protected:
        ExpressionNary();

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
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;

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

    class ExpressionCompare :
        public ExpressionNary,
        public boost::enable_shared_from_this<ExpressionCompare> {
    public:
        // virtuals from ExpressionNary
        virtual ~ExpressionCompare();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
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
        /*
          Any changes to these values require adjustment of the lookup
          table in the implementation.
         */
        enum RelOp {
            EQ = 0, // return true for a == b, false otherwise
            NE = 1, // return true for a != b, false otherwise
            GT = 2, // return true for a > b, false otherwise
            GTE = 3, // return true for a >= b, false otherwise
            LT = 4, // return true for a < b, false otherwise
            LTE = 5, // return true for a <= b, false otherwise
            CMP = 6, // return -1, 0, 1 for a < b, a == b, a > b
        };

        ExpressionCompare(RelOp relop);

        RelOp relop;
    };

    class ExpressionConstant :
        public Expression,
        public boost::enable_shared_from_this<ExpressionConstant> {
    public:
        // virtuals from Expression
        virtual ~ExpressionConstant();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;

        static shared_ptr<ExpressionConstant> createFromBsonElement(
            BSONElement *pBsonElement);

    private:
        ExpressionConstant(BSONElement *pBsonElement);

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
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;

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
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;

        static shared_ptr<ExpressionFieldPath> create(string fieldPath);

    private:
        ExpressionFieldPath(string fieldPath);

        vector<string> vFieldPath;
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

        static shared_ptr<ExpressionNary> create();

    private:
        ExpressionNot();
    };


    class ExpressionOr :
        public ExpressionNary,
        public boost::enable_shared_from_this<ExpressionOr> {
    public:
        // virtuals from Expression
        virtual ~ExpressionOr();
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;

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
