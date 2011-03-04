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
#include "Field.h"

#include "Document.h"

namespace mongo
{
    Field::~Field()
    {
    }

    shared_ptr<Field> Field::createFromBsonElement(BSONElement bsonElement)
    {
	shared_ptr<Field> pField(new Field(bsonElement));
	return pField;
    }

    Field::Field(BSONElement bsonElement):
	pFieldName(bsonElement.fieldName()),
	type(bsonElement.type()),
	pDocumentValue(),
	vpField()
    {
	switch(type)
	{
	    case NumberDouble:
		simple.doubleValue = bsonElement.Double();
		break;

	    case String:
		stringValue = bsonElement.String();
		break;

	    case Object:
	    {
		BSONObj document(bsonElement.embeddedObject());
		pDocumentValue = Document::createFromBsonObj(document);
		break;
	    }

	    case Array:
	    {
		vector<BSONElement> vElement(bsonElement.Array());
		const size_t n = vElement.size();

		vpField.reserve(n);

		for(size_t i = 0; i < n; ++i)
		{
		    vpField.push_back(
			Field::createFromBsonElement(vElement[i]));
		}
		break;
	    }

	    case BinData:
		// pBuilder->appendBinData(fieldName, ...);
		assert(false); // unimplemented
		break;

	    case jstOID:
		oidValue = bsonElement.OID();
		break;

	    case Bool:
		simple.boolValue = bsonElement.Bool();
		break;

	    case Date:
		dateValue = bsonElement.Date();
		break;

	    case RegEx:
		stringValue = bsonElement.regex();
		// TODO bsonElement.regexFlags();
		break;

	    case Symbol:
		assert(false); // unimplemented
		break;

	    case CodeWScope:
		assert(false); // unimplemented
		break;

	    case NumberInt:
		simple.intValue = bsonElement.numberInt();
		break;

	    case Timestamp:
		dateValue = bsonElement.timestampTime();
		break;

	    case NumberLong:
		simple.longValue = bsonElement.numberLong();
		break;

		/* these shouldn't happen in this context */
	    case MinKey:
	    case EOO:
	    case Undefined:
	    case jstNULL:
	    case DBRef:
	    case Code:
	    case MaxKey:
		assert(false);
		break;
	}
    }

    const char *Field::getName() const
    {
	return pFieldName;
    }

    BSONType Field::getType() const
    {
	return type;
    }

    double Field::getDouble() const
    {
	assert(getType() == NumberDouble);
	return simple.doubleValue;
    }

    string Field::getString() const
    {
	assert(getType() == String);
	return stringValue;
    }

    shared_ptr<Document> Field::getDocument() const
    {
	assert(getType() == Object);
	return pDocumentValue;
    }

    const vector<shared_ptr<Field>> *Field::getArray() const
    {
	assert(getType() == Array);
	return &vpField;
    }

    OID Field::getOid() const
    {
	assert(getType() == jstOID);
	return oidValue;
    }

    bool Field::getBool() const
    {
	assert(getType() == Bool);
	return simple.boolValue;
    }

    Date_t Field::getDate() const
    {
	assert(getType() == Date);
	return dateValue;
    }

    string Field::getRegex() const
    {
	assert(getType() == RegEx);
	return stringValue;
    }

    string Field::getSymbol() const
    {
	assert(getType() == Symbol);
	return stringValue;
    }

    int Field::getInt() const
    {
	assert(getType() == NumberInt);
	return simple.intValue;
    }

    unsigned long long Field::getTimestamp() const
    {
	assert(getType() == Timestamp);
	return dateValue;
    }

    long long Field::getLong() const
    {
	assert(getType() == NumberLong);
	return simple.longValue;
    }

    void Field::addToBsonObj(BSONObjBuilder *pBuilder) const
    {
	StringData fieldName(getName());
	switch(getType())
	{
	case NumberDouble:
	    pBuilder->append(fieldName, getDouble());
	    break;

	case String:
	    pBuilder->append(fieldName, getString());
	    break;

	case Object:
	{
	    shared_ptr<Document> pDocument(getDocument());
	    BSONObjBuilder subBuilder(pBuilder->subobjStart(fieldName));
	    pDocument->toBson(&subBuilder);
	    subBuilder.done();
	    break;
	}

	case Array:
	{
	    const size_t n = vpField.size();
	    BSONArrayBuilder arrayBuilder(n);
	    for(size_t i = 0; i < n; ++i)
	    {
		shared_ptr<Field> pField(vpField[i]);
		pField->addToBsonArray(&arrayBuilder);
	    }
	    
	    arrayBuilder.done();
	    pBuilder->append(fieldName, arrayBuilder.arr());
	    break;
	}

	case BinData:
	    // pBuilder->appendBinData(fieldName, ...);
	    assert(false); // unimplemented
	    break;

	case jstOID:
	    pBuilder->append(fieldName, getOid());
	    break;

	case Bool:
	    pBuilder->append(fieldName, getBool());
	    break;

	case Date:
	    pBuilder->append(fieldName, getDate());
	    break;

	case RegEx:
	    pBuilder->appendRegex(fieldName, getRegex());
	    break;

	case Symbol:
	    pBuilder->appendSymbol(fieldName, getSymbol());
	    break;

	case CodeWScope:
	    assert(false); // unimplemented
	    break;

	case NumberInt:
	    pBuilder->append(fieldName, getInt());
	    break;

	case Timestamp:
	    pBuilder->appendTimestamp(fieldName, getTimestamp());
	    break;

	case NumberLong:
	    pBuilder->append(fieldName, getLong());
	    break;

	    /* these shouldn't happen in this context */
	case MinKey:
	case EOO:
	case Undefined:
	case jstNULL:
	case DBRef:
	case Code:
	case MaxKey:
	    assert(false);
	    break;
	}
    }

    void Field::addToBsonArray(BSONArrayBuilder *pBuilder) const
    {
	switch(getType())
	{
	case NumberDouble:
	    pBuilder->append(getDouble());
	    break;

	case String:
	    pBuilder->append(getString());
	    break;

	case Object:
	{
	    shared_ptr<Document> pDocument(getDocument());
	    BSONObjBuilder subBuilder;
	    pDocument->toBson(&subBuilder);
	    subBuilder.done();
	    pBuilder->append(subBuilder.obj());
	    break;
	}

	case Array:
	{
	    const size_t n = vpField.size();
	    BSONArrayBuilder arrayBuilder(n);
	    for(size_t i = 0; i < n; ++i)
	    {
		shared_ptr<Field> pField(vpField[i]);
		pField->addToBsonArray(&arrayBuilder);
	    }
	    
	    arrayBuilder.done();
	    pBuilder->append(arrayBuilder.arr());
	    break;
	}

	case BinData:
	    // pBuilder->appendBinData(fieldName, ...);
	    assert(false); // unimplemented
	    break;

	case jstOID:
	    pBuilder->append(getOid());
	    break;

	case Bool:
	    pBuilder->append(getBool());
	    break;

	case Date:
	    pBuilder->append(getDate());
	    break;

	case RegEx:
	    pBuilder->append(getRegex());
	    break;

	case Symbol:
	    pBuilder->append(getSymbol());
	    break;

	case CodeWScope:
	    assert(false); // unimplemented
	    break;

	case NumberInt:
	    pBuilder->append(getInt());
	    break;

	case Timestamp:
	    pBuilder->append((long long)getTimestamp());
	    break;

	case NumberLong:
	    pBuilder->append(getLong());
	    break;

	    /* these shouldn't happen in this context */
	case MinKey:
	case EOO:
	case Undefined:
	case jstNULL:
	case DBRef:
	case Code:
	case MaxKey:
	    assert(false);
	    break;
	}
    }
}
