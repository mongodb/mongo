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

    class ExpressionField :
        public Expression,
        public boost::enable_shared_from_this<ExpressionField>
    {
    public:
	// virtuals from Expression
	virtual ~ExpressionField();
	virtual shared_ptr<Field> evaluate(
	    shared_ptr<Document> pDocument) const;

	static shared_ptr<ExpressionField> create(string fieldName);

    private:
	ExpressionField(string fieldName);

	string fieldName;
    };
}
