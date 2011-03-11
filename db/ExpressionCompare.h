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

#include "Expression.h"

namespace mongo
{
    class Document;
    class Field;

    class ExpressionCompare :
        public Expression,
        public boost::enable_shared_from_this<ExpressionCompare>
    {
    public:
	// virtuals from Expression
	virtual ~ExpressionCompare();
	virtual shared_ptr<const Field> evaluate(
	    shared_ptr<Document> pDocument) const;

	/*
	  Any changes to these values require adjustment of the lookup
	  table in the implementation.
	 */
	enum RelOp
	{
	    EQ = 0, // return true for a == b, false otherwise
	    NE = 1, // return true for a != b, false otherwise
	    GT = 2, // return true for a > b, false otherwise
	    GTE = 3, // return true for a >= b, false otherwise
	    LT = 4, // return true for a < b, false otherwise
	    LTE = 5, // return true for a <= b, false otherwise
	    CMP = 6, // return -1, 0, 1 for a < b, a == b, a > b
	};

	/*
	  Create an expression that compares two operands.

	  @param operand a vector of two operands to be compared
	  @param relOp indicates the desired result; see enum RelOp
	  @returns comparison result
	 */
	static shared_ptr<ExpressionCompare> create(
	    vector<shared_ptr<Expression>> operand, RelOp relOp);

	/*
	  Shorthands for creating various comparisons expressions.
	  Provide for conformance with the uniform function pointer signature
	  required for parsing.
	*/
	static shared_ptr<ExpressionCompare> createCmp(
	    vector<shared_ptr<Expression>> operand);
	static shared_ptr<ExpressionCompare> createEq(
	    vector<shared_ptr<Expression>> operand);
	static shared_ptr<ExpressionCompare> createNe(
	    vector<shared_ptr<Expression>> operand);
	static shared_ptr<ExpressionCompare> createGt(
	    vector<shared_ptr<Expression>> operand);
	static shared_ptr<ExpressionCompare> createGte(
	    vector<shared_ptr<Expression>> operand);
	static shared_ptr<ExpressionCompare> createLt(
	    vector<shared_ptr<Expression>> operand);
	static shared_ptr<ExpressionCompare> createLte(
	    vector<shared_ptr<Expression>> operand);


    private:
	ExpressionCompare(vector<shared_ptr<Expression>> operand, RelOp relop);

	RelOp relop;
	vector<shared_ptr<Expression>> operand;
    };
}
