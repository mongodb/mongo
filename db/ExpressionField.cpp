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
#include "ExpressionField.h"

#include "Document.h"
#include "Field.h"

namespace mongo
{
    ExpressionField::~ExpressionField()
    {
    }

    shared_ptr<ExpressionField> ExpressionField::create(string fieldName)
    {
	shared_ptr<ExpressionField> pExpression(new ExpressionField(fieldName));
	return pExpression;
    }

    ExpressionField::ExpressionField(string theFieldName):
	fieldName(theFieldName)
    {
    }

    shared_ptr<Field> ExpressionField::evaluate(
	shared_ptr<Document> pDocument) const
    {
	shared_ptr<Field> pField(pDocument->getField(fieldName));
	return pField;
    }
}
