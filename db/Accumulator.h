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

#include "db/ExpressionNary.h"

namespace mongo
{
    class Accumulator :
        public ExpressionNary
    {
    public:
	// virtuals from ExpressionNary
	virtual void addOperand(shared_ptr<Expression> pExpression);

	/*
	  Get the accumulated value.

	  @returns the accumulated value
	 */
	virtual shared_ptr<const Value> getValue() const;

    protected:
	Accumulator(shared_ptr<const Value> pStartValue);

	/*
	  Expression evaluation should be const, but accumulators have
	  this value, which mutates as a side-effect of the evaluation.
	*/
	mutable shared_ptr<const Value> pValue;
    };
}
