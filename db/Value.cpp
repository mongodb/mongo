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
#include "Value.h"

#include "Document.h"

namespace mongo
{
    const Value Value::fieldNull;
    const Value Value::fieldTrue(true);
    const Value Value::fieldFalse(false);
    const Value Value::fieldMinusOne(-1);
    const Value Value::fieldZero(0);
    const Value Value::fieldOne(1);

    Value::Value():
	type(jstNULL),
	oidValue(),
	dateValue(),
	stringValue(),
	pDocumentValue(),
	vpValue()
    {
    }

    Value::Value(bool boolValue):
	type(Bool),
	pDocumentValue(),
	vpValue()
    {
	simple.boolValue = boolValue;
    }

    Value::Value(int intValue):
	type(NumberInt),
	pDocumentValue(),
	vpValue()
    {
	simple.intValue = intValue;
    }

    Value::~Value()
    {
    }

    shared_ptr<const Value> Value::createFromBsonElement(
	BSONElement *pBsonElement)
    {
	shared_ptr<const Value> pValue(new Value(pBsonElement));
	return pValue;
    }

    Value::Value(BSONElement *pBsonElement):
	type(pBsonElement->type()),
	pDocumentValue(),
	vpValue()
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

		vpValue.reserve(n); // save on realloc()ing

		for(size_t i = 0; i < n; ++i)
		{
		    vpValue.push_back(
			Value::createFromBsonElement(&vElement[i]));
		}
		break;
	    }

	    case BinData:
		// pBuilder->appendBinData(fieldName, ...);
		assert(false); // CW TODO unimplemented
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
		assert(false); // CW TODO unimplemented
		break;

	    case CodeWScope:
		assert(false); // CW TODO unimplemented
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
		assert(false); // CW TODO better message
		break;
	}
    }

    shared_ptr<const Value> Value::createInt(int value)
    {
	shared_ptr<const Value> pValue(new Value(value));
	return pValue;
    }

    double Value::getDouble() const
    {
	assert(getType() == NumberDouble);
	return simple.doubleValue;
    }

    string Value::getString() const
    {
	assert(getType() == String);
	return stringValue;
    }

    shared_ptr<Document> Value::getDocument() const
    {
	assert(getType() == Object);
	return pDocumentValue;
    }

    const vector<shared_ptr<const Value>> *Value::getArray() const
    {
	assert(getType() == Array);
	return &vpValue;
    }

    OID Value::getOid() const
    {
	assert(getType() == jstOID);
	return oidValue;
    }

    bool Value::getBool() const
    {
	assert(getType() == Bool);
	return simple.boolValue;
    }

    Date_t Value::getDate() const
    {
	assert(getType() == Date);
	return dateValue;
    }

    string Value::getRegex() const
    {
	assert(getType() == RegEx);
	return stringValue;
    }

    string Value::getSymbol() const
    {
	assert(getType() == Symbol);
	return stringValue;
    }

    int Value::getInt() const
    {
	assert(getType() == NumberInt);
	return simple.intValue;
    }

    unsigned long long Value::getTimestamp() const
    {
	assert(getType() == Timestamp);
	return dateValue;
    }

    long long Value::getLong() const
    {
	assert(getType() == NumberLong);
	return simple.longValue;
    }

    void Value::addToBsonObj(BSONObjBuilder *pBuilder, string fieldName) const
    {
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
	    const size_t n = vpValue.size();
	    BSONArrayBuilder arrayBuilder(n);
	    for(size_t i = 0; i < n; ++i)
	    {
		shared_ptr<const Value> pValue(vpValue[i]);
		pValue->addToBsonArray(&arrayBuilder);
	    }
	    
	    arrayBuilder.done();
	    pBuilder->append(fieldName, arrayBuilder.arr());
	    break;
	}

	case BinData:
	    // pBuilder->appendBinData(fieldName, ...);
	    assert(false); // CW TODO unimplemented
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
	    assert(false); // CW TODO unimplemented
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
	    assert(false); // CW TODO better message
	    break;
	}
    }

    void Value::addToBsonArray(BSONArrayBuilder *pBuilder) const
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
	    const size_t n = vpValue.size();
	    BSONArrayBuilder arrayBuilder(n);
	    for(size_t i = 0; i < n; ++i)
	    {
		shared_ptr<const Value> pValue(vpValue[i]);
		pValue->addToBsonArray(&arrayBuilder);
	    }
	    
	    arrayBuilder.done();
	    pBuilder->append(arrayBuilder.arr());
	    break;
	}

	case BinData:
	    // pBuilder->appendBinData(fieldName, ...);
	    assert(false); // CW TODO unimplemented
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
	    assert(false); // CW TODO unimplemented
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
	    assert(false); // CW TODO better message
	    break;
	}
    }

    bool Value::coerceToBool(shared_ptr<const Value> pValue)
    {
	BSONType type = pValue->getType();
	switch(type)
	{
	case NumberDouble:
	    if (pValue->simple.doubleValue != 0)
		return true;
	    break;

	case String:
	case Object:
	case Array:
	case BinData:
	case jstOID:
	case Date:
	case RegEx:
	case Symbol:
	case Timestamp:
	    return true;

	case Bool:
	    if (pValue->simple.boolValue)
		return true;
	    break;

	case CodeWScope:
	    assert(false); // CW TODO unimplemented
	    break;

	case NumberInt:
	    if (pValue->simple.intValue != 0)
		return true;
	    break;

	case NumberLong:
	    if (pValue->simple.longValue != 0)
		return true;
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
	    assert(false); // CW TODO better message
	    break;
	}

	return false;
    }

    shared_ptr<const Value> Value::coerceToBoolean(
	shared_ptr<const Value> pValue)
    {
	bool result = coerceToBool(pValue);
	if (result)
	    return Value::getTrue();
	return Value::getFalse();
    }
}
