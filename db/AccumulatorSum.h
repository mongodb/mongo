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

#include "db/Accumulator.h"

namespace mongo
{
    class AccumulatorSum :
        public Accumulator
    {
    public:
	// virtuals from Expression
	virtual shared_ptr<const Value> evaluate(
	    shared_ptr<Document> pDocument) const;

	/*
	  Create a summing accumulator.

	  @returns the created accumulator
	 */
	static shared_ptr<Accumulator> create();

    protected:
	AccumulatorSum();
    };
}
