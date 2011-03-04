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
    class ExpressionField :
        public Expression,
        boost::enable_shared_from_this<ExpressionField>,
        boost::noncopyable
    {
    public:
	// virtuals from Expression
	virtual ~ExpressionField();
	virtual void setDocument(shared_ptr<Document>);
	virtual shared_ptr<Expression> evaluate();

	static shared_ptr<Expression> create(string fieldName);

    private:
	ExpressionField(string theFieldName);

	string fieldName;
	shared_ptr<Document> pDocument;
    };
}
