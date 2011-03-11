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

namespace mongo
{
    shared_ptr<Document> Document::createFromBsonObj(BSONObj *pBsonObj)
    {
	shared_ptr<Document> pDocument(new Document(pBsonObj));
	return pDocument;
    }

    Document::Document(BSONObj *pBsonObj):
	fieldPtr()
    {
	BSONObjIterator bsonIterator(pBsonObj->begin());
	while(bsonIterator.more())
	{
	    BSONElement bsonElement(bsonIterator.next());
	    shared_ptr<const Field> pField(
		Field::createFromBsonElement(&bsonElement));
	    fieldPtr.push_back(pField);
	}
    }

    void Document::toBson(BSONObjBuilder *pBuilder)
    {
	auto_ptr<FieldIterator> pFieldIterator(createFieldIterator());

	while(pFieldIterator->more())
	{
	    shared_ptr<const Field> pField(pFieldIterator->next());
	    pField->addToBsonObj(pBuilder);
	}
    }

    shared_ptr<Document> Document::create()
    {
        shared_ptr<Document> pDocument(new Document());
	return pDocument;
    }

    Document::Document():
	fieldPtr()
    {
    }

    shared_ptr<Document> Document::clone(shared_ptr<Document> pDocument)
    {
	shared_ptr<Document> pNew(Document::create());

	const size_t nField = pDocument->fieldPtr.size();
	for(size_t iField = 0; iField < nField; ++iField)
	    pNew->fieldPtr.push_back(pDocument->fieldPtr[iField]);

	return pNew;
    }

    Document::~Document()
    {
    }

    FieldIterator *Document::createFieldIterator()
    {
	return new FieldIterator(shared_from_this(), &fieldPtr);
    }

    shared_ptr<const Field> Document::getField(string fieldName)
    {
	/*
	  For now, assume the number of fields is small enough that iteration
	  is ok.  Later, if this gets large, we can create a map into the
	  vector for these lookups.

	  Note that because of the schema-less nature of this data, we always
	  have to look, and can't assume that the requested field is always
	  in a particular place as we would with a statically compilable
	  reference.
	*/
	const size_t n = fieldPtr.size();
	for(size_t i = 0; i < n; ++i)
	{
	    shared_ptr<const Field> pField(fieldPtr[i]);
	    const char *pFieldName = pField->getName();
	    if (fieldName.compare(pFieldName) == 0)
		return(pField);
	}

	return(shared_ptr<const Field>());
    }

    void Document::addField(shared_ptr<const Field> pField)
    {
        fieldPtr.push_back(pField);
    }

    void Document::setField(size_t index, shared_ptr<const Field> pField)
    {
	fieldPtr[index] = pField;
    }
}
