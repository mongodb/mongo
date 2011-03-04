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
	bool eof() const;
	/*
	  @return true if there are no more fields, false otherwise
	*/

	bool advance();
	/*
	  Move the iterator to point to the next field.

	  @return true if this results in a valid field
	*/

	shared_ptr<Field> getCurrent() const;
	/*
	  @return a pointer to the current Field
	*/

    private:
	friend class Document;
	FieldIterator(shared_ptr<Document> pDocument,
		      vector<shared_ptr<Field>> *pVFieldPtr);
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
	vector<shared_ptr<Field>> *pVFieldPtr;
    };
}
