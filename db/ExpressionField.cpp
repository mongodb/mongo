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

    shared_ptr<ExpressionField> ExpressionField::create(string fieldPath)
    {
	shared_ptr<ExpressionField> pExpression(new ExpressionField(fieldPath));
	return pExpression;
    }

    ExpressionField::ExpressionField(string theFieldPath):
	fieldPath()
    {
	/*
	  The field path could be using dot notation.
	  Break the field path up by peeling off successive pieces.
	*/
	size_t startpos = 0;
	while(true)
	{
	    /* find the next dot */
	    const size_t dotpos = theFieldPath.find('.', startpos);

	    /* if there are no more dots, use the remainder of the string */
	    if (dotpos == theFieldPath.npos)
	    {
		fieldPath.push_back(theFieldPath.substr(startpos, dotpos));
		break;
	    }
	    
	    /* use the string up to the dot */
	    const size_t length = dotpos - startpos;
	    assert(length); // CW TODO user error: no zero-length field names
	    fieldPath.push_back(
		theFieldPath.substr(startpos, length));

	    /* next time, search starting one spot after that */
	    startpos = dotpos + 1;
	}
    }

    shared_ptr<const Field> ExpressionField::evaluate(
	shared_ptr<Document> pDocument) const
    {
	shared_ptr<const Field> pField;
	const size_t n = fieldPath.size();
	size_t i = 0;
	while(true)
	{
	    pField = pDocument->getField(fieldPath[i]);

	    /* if the field doesn't exist, quit with a null value */
	    if (!pField.get())
		return Field::getNull();

	    /* if we've hit the end of the path, stop */
	    ++i;
	    if (i >= n)
		break;

	    /*
	      We're diving deeper.  If the value was null, return null.
	    */
	    BSONType type = pField->getType();
	    if (type == jstNULL)
		return Field::getNull();
	    if (type != Object)
		assert(false); // CW TODO user error:  must be a document

	    /* extract from the next level down */
	    pDocument = pField->getDocument();
	}

	return pField;
    }
}
