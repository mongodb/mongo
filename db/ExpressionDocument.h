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
    class ExpressionDocument :
        public Expression,
        public boost::enable_shared_from_this<ExpressionDocument>
    {
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

	  @param fieldName the name the evaluated expression will have in the result Document
	  @param pExpression the expression to evaluate obtain this field's Value in the result Document
	*/
	void addField(string fieldName, shared_ptr<Expression> pExpression);

    private:
	ExpressionDocument();

	/* these two vectors are maintained in parallel */
	vector<string> vFieldName;
	vector<shared_ptr<Expression>> vpExpression;
    };
}
