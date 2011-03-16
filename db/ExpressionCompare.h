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

#include "ExpressionNary.h"

namespace mongo
{
    class ExpressionCompare :
        public ExpressionNary,
        public boost::enable_shared_from_this<ExpressionCompare>
    {
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

	ExpressionCompare(RelOp relop);

	RelOp relop;
    };
}
