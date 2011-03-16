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

namespace mongo
{
    class Document;
    class Value;

    class Expression :
        boost::noncopyable
    {
    public:
	virtual ~Expression() {};

	/*
	  Evaluate the expression using the given document as input.

	  @return computed value
	*/
	virtual shared_ptr<const Value> evaluate(
	    shared_ptr<Document> pDocument) const = 0;
    };
}
