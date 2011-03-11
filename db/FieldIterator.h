/**
 * Copyright 2011 10gen Inc.
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
    class Field;

    class FieldIterator :
        boost::noncopyable
    {
    public:
	/*
	  Ask if there are more fields to return.

	  @return true if there are more fields, false otherwise
	*/
	bool more() const;

	/*
	  Move the iterator to point to the next field and return it.

	  @return a pointer to the next Field
	*/
	shared_ptr<const Field> next();

    private:
	friend class Document;
	FieldIterator(shared_ptr<Document> pDocument,
		      const vector<shared_ptr<const Field>> *pVFieldPtr);
	/*
	  @param pDocument points to the document whose fields are being
	      iterated
	  @param pFieldPtr points to the field vector within that document
	*/

	size_t currentField; /* index of current field */
	size_t nField; /* number of fields */

	/*
	  We'll hang on to the original document to ensure we keep the
	  fieldPtr vector alive.
	*/
	shared_ptr<Document> pDocument;
	const vector<shared_ptr<const Field>> *pVFieldPtr;
    };
}
