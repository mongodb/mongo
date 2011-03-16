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
#include "ExpressionConstant.h"

#include "Value.h"

namespace mongo
{
    ExpressionConstant::~ExpressionConstant()
    {
    }

    shared_ptr<ExpressionConstant> ExpressionConstant::createFromBsonElement(
	BSONElement *pBsonElement)
    {
	shared_ptr<ExpressionConstant> pEC(
	    new ExpressionConstant(pBsonElement));
	return pEC;
    }

    ExpressionConstant::ExpressionConstant(BSONElement *pBsonElement):
	pValue(Value::createFromBsonElement(pBsonElement))
    {
    }

    shared_ptr<const Value> ExpressionConstant::evaluate(
	shared_ptr<Document> pDocument) const
    {
	return pValue;
    }
}
