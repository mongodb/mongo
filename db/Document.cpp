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
#include "Document.h"
#include "Field.h"
#include "FieldIterator.h"

namespace mongo {

    Document::Document():
	fieldPtr()
    {
    }

    Document::Document(BSONObj bsonObj):
	fieldPtr()
    {
	size_t i = 0;
	for(BSONObj::iterator bsonIterator = bsonObj.begin();
	    bsonIterator.more(); ++i)
	{
	    BSONElement bsonElement = bsonIterator.next();
	    shared_ptr<Field> pField(new Field(bsonElement));
	    fieldPtr[i] = pField;
	}
    }

    Document::~Document()
    {
    }

    FieldIterator *Document::createFieldIterator()
    {
	return new FieldIterator(this, &fieldPtr);
    }
}
