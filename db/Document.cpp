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
#include "FieldIterator.h"
#include "Value.h"

namespace mongo
{
    shared_ptr<Document> Document::createFromBsonObj(BSONObj *pBsonObj)
    {
	shared_ptr<Document> pDocument(new Document(pBsonObj));
	return pDocument;
    }

    Document::Document(BSONObj *pBsonObj):
	vFieldName(),
	vpValue()
    {
	BSONObjIterator bsonIterator(pBsonObj->begin());
	while(bsonIterator.more())
	{
	    BSONElement bsonElement(bsonIterator.next());
	    string fieldName(bsonElement.fieldName());
	    shared_ptr<const Value> pValue(
		Value::createFromBsonElement(&bsonElement));

	    vFieldName.push_back(fieldName);
	    vpValue.push_back(pValue);
	}
    }

    void Document::toBson(BSONObjBuilder *pBuilder)
    {
	const size_t n = vFieldName.size();
	for(size_t i = 0; i < n; ++i)
	    vpValue[i]->addToBsonObj(pBuilder, vFieldName[i]);
    }

    shared_ptr<Document> Document::create()
    {
        shared_ptr<Document> pDocument(new Document());
	return pDocument;
    }

    Document::Document():
	vFieldName(),
	vpValue()
    {
    }

    shared_ptr<Document> Document::clone()
    {
	shared_ptr<Document> pNew(Document::create());

	const size_t n = vFieldName.size();
	pNew->vFieldName.reserve(n);
	pNew->vpValue.reserve(n);
	for(size_t i = 0; i < n; ++i)
	    pNew->addField(vFieldName[i], vpValue[i]);

	return pNew;
    }

    Document::~Document()
    {
    }

    FieldIterator *Document::createFieldIterator()
    {
	return new FieldIterator(shared_from_this());
    }

    shared_ptr<const Value> Document::getValue(string fieldName)
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
	const size_t n = vFieldName.size();
	for(size_t i = 0; i < n; ++i)
	{
	    if (strcmp(vFieldName[i].c_str(), fieldName.c_str()) == 0)
		return vpValue[i];
	}

	return(shared_ptr<const Value>());
    }

    void Document::addField(string fieldName, shared_ptr<const Value> pValue)
    {
	vFieldName.push_back(fieldName);
	vpValue.push_back(pValue);
    }

    void Document::setField(size_t index,
			    string fieldName, shared_ptr<const Value> pValue)
    {
	vFieldName[index] = fieldName;
	vpValue[index] = pValue;
    }
}
