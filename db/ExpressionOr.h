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
    class ExpressionOr :
        public ExpressionNary,
        public boost::enable_shared_from_this<ExpressionOr>
    {
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
