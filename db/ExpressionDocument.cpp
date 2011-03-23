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

#include "pch.h"
#include "ExpressionDocument.h"

#include "Document.h"
#include "Value.h"

namespace mongo
{
    ExpressionDocument::~ExpressionDocument()
    {
    }

    shared_ptr<ExpressionDocument> ExpressionDocument::create()
    {
	shared_ptr<ExpressionDocument> pExpression(new ExpressionDocument());
	return pExpression;
    }

    ExpressionDocument::ExpressionDocument():
	vFieldName(),
	vpExpression()
    {
    }

    shared_ptr<const Value> ExpressionDocument::evaluate(
	shared_ptr<Document> pDocument) const
    {
	const size_t n = vFieldName.size();
	shared_ptr<Document> pResult(Document::create(n));
	for(size_t i = 0; i < n; ++i)
	{
	    pResult->addField(vFieldName[i],
			      vpExpression[i]->evaluate(pDocument));
	}

	shared_ptr<const Value> pValue(Value::createDocument(pResult));
	return pValue;
    }
    
    void ExpressionDocument::addField(string fieldName,
				      shared_ptr<Expression> pExpression)
    {
	vFieldName.push_back(fieldName);
	vpExpression.push_back(pExpression);
    }
}
