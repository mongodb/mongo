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
    const Field Field::fieldNull("Field::fieldNull");
    const Field Field::fieldTrue("Field::fieldTrue", true);
    const Field Field::fieldFalse("Field::fieldFalse", false);
    const Field Field::fieldMinusOne("Field::fieldMinusOne", -1);
    const Field Field::fieldZero("Field::fieldZero", 0);
    const Field Field::fieldOne("Field::fieldOne", 1);

    Field::Field(string theFieldName):
	fieldName(theFieldName),
	type(jstNULL),
	oidValue(),
	dateValue(),
	stringValue(),
	pDocumentValue(),
	vpField()
    {
    }

    Field::Field(string theFieldName, bool boolValue):
	fieldName(theFieldName),
	type(Bool),
	pDocumentValue(),
	vpField()
    {
	simple.boolValue = boolValue;
    }

    Field::Field(string theFieldName, int intValue):
	fieldName(theFieldName),
	type(NumberInt),
	pDocumentValue(),
	vpField()
    {
	simple.intValue = intValue;
    }

    Field::~Field()
    {
    }

    shared_ptr<const Field> Field::createFromBsonElement(
	BSONElement *pBsonElement)
    {
	shared_ptr<const Field> pField(new Field(pBsonElement));
	return pField;
    }

    Field::Field(BSONElement *pBsonElement):
	fieldName(pBsonElement->fieldName()),
	type(pBsonElement->type()),
	pDocumentValue(),
	vpField()
    {
	switch(type)
	{
	    case NumberDouble:
		simple.doubleValue = pBsonElement->Double();
		break;

	    case String:
		stringValue = pBsonElement->String();
		break;

	    case Object:
	    {
		BSONObj document(pBsonElement->embeddedObject());
		pDocumentValue = Document::createFromBsonObj(&document);
		break;
	    }

	    case Array:
	    {
		vector<BSONElement> vElement(pBsonElement->Array());
		const size_t n = vElement.size();

		vpField.reserve(n); // save on realloc()ing

		for(size_t i = 0; i < n; ++i)
		{
		    vpField.push_back(
			Field::createFromBsonElement(&vElement[i]));
		}
		break;
	    }

	    case BinData:
		// pBuilder->appendBinData(fieldName, ...);
		assert(false); // unimplemented
		break;

	    case jstOID:
		oidValue = pBsonElement->OID();
		break;

	    case Bool:
		simple.boolValue = pBsonElement->Bool();
		break;

	    case Date:
		dateValue = pBsonElement->Date();
		break;

	    case RegEx:
		stringValue = pBsonElement->regex();
		// TODO pBsonElement->regexFlags();
		break;

	    case Symbol:
		assert(false); // unimplemented
		break;

	    case CodeWScope:
		assert(false); // unimplemented
		break;

	    case NumberInt:
		simple.intValue = pBsonElement->numberInt();
		break;

	    case Timestamp:
		dateValue = pBsonElement->timestampTime();
		break;

	    case NumberLong:
		simple.longValue = pBsonElement->numberLong();
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

    shared_ptr<const Field> Field::createRename(
	string newName, shared_ptr<const Field> pSourceField)
    {
	shared_ptr<const Field> pField(new Field(newName, pSourceField));
	return pField;
    }

    shared_ptr<const Field> Field::createInt(string fieldName, int value)
    {
	shared_ptr<const Field> pField(new Field(fieldName, value));
	return pField;
    }

    Field::Field(string newName, shared_ptr<const Field> pSourceField):
	fieldName(newName),
	type(pSourceField->getType()),
	pDocumentValue(),
	vpField()
    {
	/* assign the simple value, whatever it is */
	switch(type)
	{
	    case NumberDouble:
		simple.doubleValue = pSourceField->simple.doubleValue;
		break;

	    case String:
	    case RegEx:
		stringValue = pSourceField->stringValue;
		break;

	    case Object:
		pDocumentValue = pSourceField->pDocumentValue;
		break;

	    case Array:
		vpField = pSourceField->vpField;
		break;

	    case BinData:
		assert(false); // unimplemented
		break;

	    case jstOID:
		oidValue = pSourceField->oidValue;
		break;

	    case Bool:
		simple.boolValue = pSourceField->simple.boolValue;
		break;

	    case Date:
	    case Timestamp:
		dateValue = pSourceField->dateValue;
		break;

	    case Symbol:
		assert(false); // unimplemented
		break;

	    case CodeWScope:
		assert(false); // unimplemented
		break;

	    case NumberInt:
		simple.intValue = pSourceField->simple.intValue;
		break;

	    case NumberLong:
		simple.longValue = pSourceField->simple.longValue;
		break;

	    case jstNULL:
		/* nothing to do */
		break;
	    
		/* these shouldn't happen in this context */
	    case MinKey:
	    case EOO:
	    case Undefined:
	    case DBRef:
	    case Code:
	    case MaxKey:
		assert(false);
		break;
	}
    }

    const char *Field::getName() const
    {
	return fieldName.c_str();
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

    const vector<shared_ptr<const Field>> *Field::getArray() const
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
		shared_ptr<const Field> pField(vpField[i]);
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

	case jstNULL:
	    pBuilder->appendNull(fieldName);
	    break;

	    /* these shouldn't happen in this context */
	case MinKey:
	case EOO:
	case Undefined:
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
		shared_ptr<const Field> pField(vpField[i]);
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
