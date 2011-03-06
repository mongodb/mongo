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
        public boost::enable_shared_from_this<Document>,
        boost::noncopyable
    {
    public:
	~Document();

	/*
	  Create a new Document from the given BSONObj.

	  Document field values may be pointed to in the BSONObj, so it
	  must live at least as long as the resulting Document.

	  @returns shared pointer to the newly created Document
	*/
	static shared_ptr<Document> createFromBsonObj(BSONObj bsonObj);

	/*
	  Create a new empty Document.

	  @returns shared pointer to the newly created Document
	*/
	static shared_ptr<Document> create();

	/*
	  Add this document to the BSONObj under construction with the
	  given BSONObjBuilder.
	*/
	void toBson(BSONObjBuilder *pBsonObjBuilder);

	/*
	  Create a new FieldIterator that can be used to examine the
	  Document's fields.
	*/
	FieldIterator *createFieldIterator();

	/*
	  Get a pointer to the requested field.

	  @param fieldName the name of the field
	  @return point to the requested field
	*/
	shared_ptr<Field> getField(string fieldName);

	/*
	  Add the given field to the Document.

	  BSON documents' fields are ordered; the new Field will be
	  appened to the current list of fields.

	  It is an error to add a Field that has the same name as another
	  field.
	*/
	void addField(shared_ptr<Field> pField);

    private:
	Document();
	Document(BSONObj bsonObj);

	vector<shared_ptr<Field>> fieldPtr;
    };
}
