/**
 * Copyright 2011 (c) 10gen Inc.
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
#include "jsobj.h"

namespace mongo
{
    class Field;
    class FieldIterator;

    class Document :
        boost::noncopyable
    {
    public:
	~Document();

	Document();
	Document(BSONObj bsonObj);

	FieldIterator *createFieldIterator();
	/*
	  Create a new FieldIterator that can be used to examine the
	  Document's fields.
	*/

    private:
	vector<shared_ptr<Field>> fieldPtr;
    };
}
