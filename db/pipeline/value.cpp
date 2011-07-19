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
#include "db/pipeline/value.h"

#include <boost/functional/hash.hpp>
#include "db/jsobj.h"
#include "db/pipeline/builder.h"
#include "db/pipeline/document.h"

namespace mongo {
    const Value Value::fieldUndefined(Undefined);
    const Value Value::fieldNull;
    const Value Value::fieldTrue(true);
    const Value Value::fieldFalse(false);
    const Value Value::fieldMinusOne(-1);
    const Value Value::fieldZero(0);
    const Value Value::fieldOne(1);

    Value::~Value() {
    }

    Value::Value():
        type(jstNULL),
        oidValue(),
        dateValue(),
        stringValue(),
        pDocumentValue(),
        vpValue() {
    }

    Value::Value(BSONType theType):
        type(theType),
        oidValue(),
        dateValue(),
        stringValue(),
        pDocumentValue(),
        vpValue() {
	switch(type) {
	case Undefined:
	case jstNULL:
	case Object: // empty
	case Array: // empty
	    break;

	case NumberDouble:
	    simple.doubleValue = 0;
	    break;

	case Bool:
	    simple.boolValue = false;
	    break;

	case NumberInt:
	    simple.intValue = 0;
	    break;

	case Timestamp:
	    simple.timestampValue = 0;
	    break;

	case NumberLong:
	    simple.longValue = 0;
	    break;

	default:
	    // nothing else is allowed
	    assert(false && type);
	}
    }

    Value::Value(bool boolValue):
        type(Bool),
        pDocumentValue(),
        vpValue() {
        simple.boolValue = boolValue;
    }

    shared_ptr<const Value> Value::createFromBsonElement(
        BSONElement *pBsonElement) {
        shared_ptr<const Value> pValue(new Value(pBsonElement));
        return pValue;
    }

    Value::Value(BSONElement *pBsonElement):
        type(pBsonElement->type()),
        pDocumentValue(),
        vpValue() {
        switch(type) {
        case NumberDouble:
            simple.doubleValue = pBsonElement->Double();
            break;

        case String:
            stringValue = pBsonElement->String();
            break;

        case Object: {
            BSONObj document(pBsonElement->embeddedObject());
            pDocumentValue = Document::createFromBsonObj(&document);
            break;
        }

        case Array: {
            vector<BSONElement> vElement(pBsonElement->Array());
            const size_t n = vElement.size();

            vpValue.reserve(n); // save on realloc()ing

            for(size_t i = 0; i < n; ++i) {
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

        case jstNULL:
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

    Value::Value(int intValue):
        type(NumberInt),
        pDocumentValue(),
        vpValue() {
        simple.intValue = intValue;
    }

    shared_ptr<const Value> Value::createInt(int value) {
        shared_ptr<const Value> pValue(new Value(value));
        return pValue;
    }

    Value::Value(long long longValue):
        type(NumberLong),
        pDocumentValue(),
        vpValue() {
        simple.longValue = longValue;
    }

    shared_ptr<const Value> Value::createLong(long long value) {
        shared_ptr<const Value> pValue(new Value(value));
        return pValue;
    }

    Value::Value(double value):
        type(NumberDouble),
        pDocumentValue(),
        vpValue() {
        simple.doubleValue = value;
    }

    shared_ptr<const Value> Value::createDouble(double value) {
        shared_ptr<const Value> pValue(new Value(value));
        return pValue;
    }

    Value::Value(const Date_t &value):
        type(Date),
        pDocumentValue(),
        vpValue() {
        dateValue = value;
    }

    shared_ptr<const Value> Value::createDate(const Date_t &value) {
        shared_ptr<const Value> pValue(new Value(value));
        return pValue;
    }

    Value::Value(const string &value):
        type(String),
        pDocumentValue(),
        vpValue() {
        stringValue = value;
    }

    shared_ptr<const Value> Value::createString(const string &value) {
        shared_ptr<const Value> pValue(new Value(value));
        return pValue;
    }

    Value::Value(const shared_ptr<Document> &pDocument):
        type(Object),
        pDocumentValue(pDocument),
        vpValue() {
    }

    shared_ptr<const Value> Value::createDocument(
        const shared_ptr<Document> &pDocument) {
        shared_ptr<const Value> pValue(new Value(pDocument));
        return pValue;
    }

    Value::Value(const vector<shared_ptr<const Value> > &thevpValue):
        type(Array),
        pDocumentValue(),
        vpValue(thevpValue) {
    }

    shared_ptr<const Value> Value::createArray(
        const vector<shared_ptr<const Value> > &vpValue) {
        shared_ptr<const Value> pValue(new Value(vpValue));
        return pValue;
    }

    double Value::getDouble() const {
        BSONType type = getType();
        if (type == NumberInt)
            return simple.intValue;
        if (type == NumberLong)
            return simple.longValue;

        assert(type == NumberDouble);
        return simple.doubleValue;
    }

    string Value::getString() const {
        assert(getType() == String);
        return stringValue;
    }

    shared_ptr<Document> Value::getDocument() const {
        assert(getType() == Object);
        return pDocumentValue;
    }

    ValueIterator::~ValueIterator() {
    }

    Value::vi::~vi() {
    }

    bool Value::vi::more() const {
        return (nextIndex < size);
    }

    shared_ptr<const Value> Value::vi::next() {
        assert(more());
        return (*pvpValue)[nextIndex++];
    }

    Value::vi::vi(const shared_ptr<const Value> &pValue,
                  const vector<shared_ptr<const Value> > *thepvpValue):
        size(thepvpValue->size()),
        nextIndex(0),
        pvpValue(thepvpValue) {
    }

    shared_ptr<ValueIterator> Value::getArray() const {
        assert(getType() == Array);
        shared_ptr<ValueIterator> pVI(new vi(shared_from_this(), &vpValue));
        return pVI;
    }

    OID Value::getOid() const {
        assert(getType() == jstOID);
        return oidValue;
    }

    bool Value::getBool() const {
        assert(getType() == Bool);
        return simple.boolValue;
    }

    Date_t Value::getDate() const {
        assert(getType() == Date);
        return dateValue;
    }

    string Value::getRegex() const {
        assert(getType() == RegEx);
        return stringValue;
    }

    string Value::getSymbol() const {
        assert(getType() == Symbol);
        return stringValue;
    }

    int Value::getInt() const {
        assert(getType() == NumberInt);
        return simple.intValue;
    }

    unsigned long long Value::getTimestamp() const {
        assert(getType() == Timestamp);
        return dateValue;
    }

    long long Value::getLong() const {
        BSONType type = getType();
        if (type == NumberInt)
            return simple.intValue;

        assert(type == NumberLong);
        return simple.longValue;
    }

    void Value::addToBson(Builder *pBuilder) const {
        switch(getType()) {
        case NumberDouble:
            pBuilder->append(getDouble());
            break;

        case String:
            pBuilder->append(getString());
            break;

        case Object: {
            shared_ptr<Document> pDocument(getDocument());
            BSONObjBuilder subBuilder;
            pDocument->toBson(&subBuilder);
            subBuilder.done();
            pBuilder->append(&subBuilder);
            break;
        }

        case Array: {
            const size_t n = vpValue.size();
            BSONArrayBuilder arrayBuilder(n);
            for(size_t i = 0; i < n; ++i) {
                vpValue[i]->addToBsonArray(&arrayBuilder);
            }

            pBuilder->append(&arrayBuilder);
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

	case jstNULL:
	    pBuilder->append();
	    break;

            /* these shouldn't appear in this context */
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

    void Value::addToBsonObj(BSONObjBuilder *pBuilder, string fieldName) const {
	BuilderObj objBuilder(pBuilder, fieldName);
	addToBson(&objBuilder);
    }

    void Value::addToBsonArray(BSONArrayBuilder *pBuilder) const {
	BuilderArray arrBuilder(pBuilder);
	addToBson(&arrBuilder);
    }

    bool Value::coerceToBool() const {
        BSONType type = getType();
        switch(type) {
        case NumberDouble:
            if (simple.doubleValue != 0)
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
            if (simple.boolValue)
                return true;
            break;

        case CodeWScope:
            assert(false); // CW TODO unimplemented
            break;

        case NumberInt:
            if (simple.intValue != 0)
                return true;
            break;

        case NumberLong:
            if (simple.longValue != 0)
                return true;
            break;

        case jstNULL:
        case Undefined:
            /* nothing to do */
            break;

            /* these shouldn't happen in this context */
        case MinKey:
        case EOO:
        case DBRef:
        case Code:
        case MaxKey:
            assert(false); // CW TODO better message
            break;
        }

        return false;
    }

    shared_ptr<const Value> Value::coerceToBoolean() const {
        bool result = coerceToBool();

        /* always normalize to the singletons */
        if (result)
            return Value::getTrue();
        return Value::getFalse();
    }

    int Value::coerceToInt() const {
        switch(type) {
        case NumberDouble:
            return (int)simple.doubleValue;

        case NumberInt:
            return simple.intValue;

        case NumberLong:
            return (int)simple.longValue;

        case String:
            assert(false); // CW TODO try to convert w/atod()
            return (int)0;

	case jstNULL:
	case Undefined:
	    break;

        default:
            assert(false); // CW TODO no conversion available
        } // switch(type)

        return (int)0;
    }

    long long Value::coerceToLong() const {
        switch(type) {
        case NumberDouble:
            return (long long)simple.doubleValue;

        case NumberInt:
            return simple.intValue;

        case NumberLong:
            return simple.longValue;

        case String:
            assert(false); // CW TODO try to convert w/atod()
            return (long long)0;

	case jstNULL:
	case Undefined:
	    break;

        default:
            assert(false); // CW TODO no conversion available
        } // switch(type)

        return (long long)0;
    }

    double Value::coerceToDouble() const {
        switch(type) {
        case NumberDouble:
            return simple.doubleValue;

        case NumberInt:
            return (double)simple.intValue;

        case NumberLong:
            return (double)simple.longValue;

        case String:
            assert(false); // CW TODO try to convert w/atod()
            return (double)0;

	case jstNULL:
	case Undefined:
	    break;

        default:
            assert(false); // CW TODO no conversion available
        } // switch(type)

        return (double)0;
    }

    Date_t Value::coerceToDate() const {
        switch(type) {

        case Date:
            return dateValue; 

	case jstNULL:
	case Undefined:
	    break;

        default:
            assert(false); // CW TODO no conversion available
        } // switch(type)

            assert(false); // CW TODO no conversion available
        return jstNULL; 
    }

    string Value::coerceToString() const {
        stringstream ss;
        switch(type) {
        case NumberDouble:
            ss << simple.doubleValue;
            return ss.str();

        case NumberInt:
            ss << simple.intValue;
            return ss.str();

        case NumberLong:
            ss << simple.longValue;
            return ss.str();

        case String:
            return stringValue;

        case Date:
            return dateValue.toString();

	case jstNULL:
	case Undefined:
	    break;

        default:
            assert(false); // CW TODO no conversion available
        } // switch(type)

        return "";
    }

    int Value::compare(const shared_ptr<const Value> &rL,
                       const shared_ptr<const Value> &rR) {
        BSONType lType = rL->getType();
	BSONType rType = rR->getType();

	/*
	  Special handling for Undefined and NULL values; these are types,
	  so it's easier to handle them here before we go below to handle
	  values of the same types.  This allows us to compare Undefined and
	  NULL values with everything else.  As coded now:
	  (*) Undefined is less than everything except itself (which is equal)
	  (*) NULL is less than everything except Undefined and itself
	 */
	if (lType == Undefined) {
	    if (rType == Undefined)
		return 0;

	    /* if rType is anything else, the left value is less */
	    return -1;
	}
	
	if (lType == jstNULL) {
	    if (rType == Undefined)
		return 1;
	    if (rType == jstNULL)
		return 0;

	    return -1;
	}

	if ((rType == Undefined) || (rType == jstNULL)) {
	    /*
	      We know the left value isn't Undefined, because of the above.
	      Count a NULL value as greater than an undefined one.
	    */
	    return 1;
	}

        // CW TODO for now, only compare like values
        assert(lType == rType);

        switch(lType) {
        case NumberDouble:
            if (rL->simple.doubleValue < rR->simple.doubleValue)
                return -1;
            if (rL->simple.doubleValue > rR->simple.doubleValue)
                return 1;
            return 0;

        case String:
            return rL->stringValue.compare(rR->stringValue);

        case Object:
            return Document::compare(rL->getDocument(), rR->getDocument());

        case Array: {
            shared_ptr<ValueIterator> pli(rL->getArray());
            shared_ptr<ValueIterator> pri(rR->getArray());

            while(true) {
                /* have we run out of left array? */
                if (!pli->more()) {
                    if (!pri->more())
                        return 0; // the arrays are the same length

                    return -1; // the left array is shorter
                }

                /* have we run out of right array? */
                if (!pri->more())
                    return 1; // the right array is shorter

                /* compare the two corresponding elements */
                shared_ptr<const Value> plv(pli->next());
                shared_ptr<const Value> prv(pri->next());
                const int cmp = Value::compare(plv, prv);
                if (cmp)
                    return cmp; // values are unequal
            }

            /* NOTREACHED */
            assert(false);
            break;
        }

        case BinData:
            // pBuilder->appendBinData(fieldName, ...);
            assert(false); // CW TODO unimplemented
            break;

        case jstOID:
            if (rL->oidValue < rR->oidValue)
                return -1;
            if (rL->oidValue == rR->oidValue)
                return 0;
            return 1;

        case Bool:
            if (rL->simple.boolValue == rR->simple.boolValue)
                return 0;
            if (rL->simple.boolValue)
                return 1;
            return -1;

        case Date:
            if (rL->dateValue < rR->dateValue)
                return -1;
            if (rL->dateValue > rR->dateValue)
                return 1;
            return 0;

        case RegEx:
            return rL->stringValue.compare(rR->stringValue);

        case Symbol:
            assert(false); // CW TODO unimplemented
            break;

        case CodeWScope:
            assert(false); // CW TODO unimplemented
            break;

        case NumberInt:
            if (rL->simple.intValue < rR->simple.intValue)
                return -1;
            if (rL->simple.intValue > rR->simple.intValue)
                return 1;
            return 0;

        case Timestamp:
            if (rL->dateValue < rR->dateValue)
                return -1;
            if (rL->dateValue > rR->dateValue)
                return 1;
            return 0;

        case NumberLong:
            if (rL->simple.longValue < rR->simple.longValue)
                return -1;
            if (rL->simple.longValue > rR->simple.longValue)
                return 1;
            return 0;

        case Undefined:
        case jstNULL:
	    return 0; // treat two Undefined or NULL values as equal

            /* these shouldn't happen in this context */
        case MinKey:
        case EOO:
        case DBRef:
        case Code:
        case MaxKey:
            assert(false); // CW TODO better message
            break;
        } // switch(lType)

        /* NOTREACHED */
        return 0;
    }

    void Value::hash_combine(size_t &seed) const {
	BSONType type = getType();
	boost::hash_combine(seed, (int)type);

        switch(type) {
        case NumberDouble:
	    boost::hash_combine(seed, simple.doubleValue);
	    break;

        case String:
	    boost::hash_combine(seed, stringValue);
	    break;

        case Object:
	    getDocument()->hash_combine(seed);
	    break;

        case Array: {
	    shared_ptr<ValueIterator> pIter(getArray());
	    while(pIter->more()) {
		shared_ptr<const Value> pValue(pIter->next());
		pValue->hash_combine(seed);
	    };
            break;
        }

        case BinData:
            // pBuilder->appendBinData(fieldName, ...);
            assert(false); // CW TODO unimplemented
            break;

        case jstOID:
	    oidValue.hash_combine(seed);
	    break;

        case Bool:
	    boost::hash_combine(seed, simple.boolValue);
	    break;

        case Date:
	    boost::hash_combine(seed, (unsigned long long)dateValue);
	    break;

        case RegEx:
	    boost::hash_combine(seed, stringValue);
	    break;

        case Symbol:
            assert(false); // CW TODO unimplemented
            break;

        case CodeWScope:
            assert(false); // CW TODO unimplemented
            break;

        case NumberInt:
	    boost::hash_combine(seed, simple.intValue);
	    break;

        case Timestamp:
	    boost::hash_combine(seed, (unsigned long long)dateValue);
	    break;

        case NumberLong:
	    boost::hash_combine(seed, simple.longValue);
	    break;

        case Undefined:
        case jstNULL:
	    break;

            /* these shouldn't happen in this context */
        case MinKey:
        case EOO:
        case DBRef:
        case Code:
        case MaxKey:
            assert(false); // CW TODO better message
            break;
        } // switch(type)
    }

    BSONType Value::getWidestNumeric(BSONType lType, BSONType rType) {
	if (lType == NumberDouble) {
	    switch(rType) {
	    case NumberDouble:
	    case NumberLong:
	    case NumberInt:
	    case jstNULL:
	    case Undefined:
		return NumberDouble;

	    default:
		break;
	    }
	}
	else if (lType == NumberLong) {
	    switch(rType) {
	    case NumberDouble:
		return NumberDouble;

	    case NumberLong:
	    case NumberInt:
	    case jstNULL:
	    case Undefined:
		return NumberLong;

	    default:
		break;
	    }
	}
	else if (lType == NumberInt) {
	    switch(rType) {
	    case NumberDouble:
		return NumberDouble;

	    case NumberLong:
		return NumberLong;

	    case NumberInt:
	    case jstNULL:
	    case Undefined:
		return NumberInt;

	    default:
		break;
	    }
	}
	else if ((lType == jstNULL) || (lType == Undefined)) {
	    switch(rType) {
	    case NumberDouble:
		return NumberDouble;

	    case NumberLong:
		return NumberLong;

	    case NumberInt:
		return NumberInt;

	    default:
		break;
	    }
	}

        /* NOTREACHED */
        return Undefined;
    }
}
