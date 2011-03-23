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
    class Value;
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
	static shared_ptr<Document> createFromBsonObj(BSONObj *pBsonObj);

	/*
	  Create a new empty Document.

	  @param sizeHint a hint at what the number of fields will be; if
	    known, this can be used to increase memory allocation efficiency
	  @returns shared pointer to the newly created Document
	*/
	static shared_ptr<Document> create(size_t sizeHint = 0);

	/*
	  Clone a document.

	  The new document shares all the fields' values with the original.
	*/
	shared_ptr<Document> clone();

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
	  Get the value of the specified field.

	  @param fieldName the name of the field
	  @return point to the requested field
	*/
	shared_ptr<const Value> getValue(string fieldName);

	/*
	  Add the given field to the Document.

	  BSON documents' fields are ordered; the new Field will be
	  appened to the current list of fields.

	  It is an error to add a field that has the same name as another
	  field.
	*/
	void addField(string fieldName, shared_ptr<const Value> pValue);

	/*
	  Set the given field to be at the specified position in the
	  Document.  This will replace any field that is currently in that
	  position.  The index must be within the current range of field
	  indices.
	*/
	void setField(size_t index,
		      string fieldName, shared_ptr<const Value> pValue);

	/*
	  Compare two documents.

	  BSON document field order is significant, so this just goes through
	  the fields in order.  The comparison is done in roughly the same way
	  as strings are compared, but comparing one field at a time instead
	  of one character at a time.
	*/
	static int compare(const shared_ptr<Document> &rL,
			   const shared_ptr<Document> &rR);

    private:
	friend class FieldIterator;

	Document(size_t sizeHint);
	Document(BSONObj *pBsonObj);

	/* these two vectors parallel each other */
	vector<string> vFieldName;
	vector<shared_ptr<const Value>> vpValue;
    };
}
