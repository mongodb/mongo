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
#include "util/mongoutils/str.h"

namespace mongo {
    using namespace mongoutils;

    const intrusive_ptr<const Value> Value::pFieldUndefined(
        new ValueStatic(Undefined));
    const intrusive_ptr<const Value> Value::pFieldNull(new ValueStatic());
    const intrusive_ptr<const Value> Value::pFieldTrue(new ValueStatic(true));
    const intrusive_ptr<const Value> Value::pFieldFalse(new ValueStatic(false));
    const intrusive_ptr<const Value> Value::pFieldMinusOne(new ValueStatic(-1));
    const intrusive_ptr<const Value> Value::pFieldZero(new ValueStatic(0));
    const intrusive_ptr<const Value> Value::pFieldOne(new ValueStatic(1));

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
            uassert(16001, str::stream() <<
                    "can't create empty Value of type " << type, false);
            break;
        }
    }

    Value::Value(bool boolValue):
        type(Bool),
        pDocumentValue(),
        vpValue() {
        simple.boolValue = boolValue;
    }

    intrusive_ptr<const Value> Value::createFromBsonElement(
        BSONElement *pBsonElement) {
        intrusive_ptr<const Value> pValue(new Value(pBsonElement));
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

        case BinData:
        case Symbol:
        case CodeWScope:

            /* these shouldn't happen in this context */
        case MinKey:
        case EOO:
        case Undefined:
        case DBRef:
        case Code:
        case MaxKey:
            uassert(16002, str::stream() <<
                    "can't create Value of BSON type " << type, false);
            break;
        }
    }

    Value::Value(int intValue):
        type(NumberInt),
        pDocumentValue(),
        vpValue() {
        simple.intValue = intValue;
    }

    intrusive_ptr<const Value> Value::createInt(int value) {
        intrusive_ptr<const Value> pValue(new Value(value));
        return pValue;
    }

    Value::Value(long long longValue):
        type(NumberLong),
        pDocumentValue(),
        vpValue() {
        simple.longValue = longValue;
    }

    intrusive_ptr<const Value> Value::createLong(long long value) {
        intrusive_ptr<const Value> pValue(new Value(value));
        return pValue;
    }

    Value::Value(double value):
        type(NumberDouble),
        pDocumentValue(),
        vpValue() {
        simple.doubleValue = value;
    }

    intrusive_ptr<const Value> Value::createDouble(double value) {
        intrusive_ptr<const Value> pValue(new Value(value));
        return pValue;
    }

    Value::Value(const Date_t &value):
        type(Date),
        pDocumentValue(),
        vpValue() {
        dateValue = value;
    }

    intrusive_ptr<const Value> Value::createDate(const Date_t &value) {
        intrusive_ptr<const Value> pValue(new Value(value));
        return pValue;
    }

    Value::Value(const string &value):
        type(String),
        pDocumentValue(),
        vpValue() {
        stringValue = value;
    }

    intrusive_ptr<const Value> Value::createString(const string &value) {
        intrusive_ptr<const Value> pValue(new Value(value));
        return pValue;
    }

    Value::Value(const intrusive_ptr<Document> &pDocument):
        type(Object),
        pDocumentValue(pDocument),
        vpValue() {
    }

    intrusive_ptr<const Value> Value::createDocument(
        const intrusive_ptr<Document> &pDocument) {
        intrusive_ptr<const Value> pValue(new Value(pDocument));
        return pValue;
    }

    Value::Value(const vector<intrusive_ptr<const Value> > &thevpValue):
        type(Array),
        pDocumentValue(),
        vpValue(thevpValue) {
    }

    intrusive_ptr<const Value> Value::createArray(
        const vector<intrusive_ptr<const Value> > &vpValue) {
        intrusive_ptr<const Value> pValue(new Value(vpValue));
        return pValue;
    }

    double Value::getDouble() const {
        BSONType type = getType();
        if (type == NumberInt)
            return simple.intValue;
        if (type == NumberLong)
            return static_cast< double >( simple.longValue );

        verify(type == NumberDouble);
        return simple.doubleValue;
    }

    string Value::getString() const {
        verify(getType() == String);
        return stringValue;
    }

    intrusive_ptr<Document> Value::getDocument() const {
        verify(getType() == Object);
        return pDocumentValue;
    }

    ValueIterator::~ValueIterator() {
    }

    Value::vi::~vi() {
    }

    bool Value::vi::more() const {
        return (nextIndex < size);
    }

    intrusive_ptr<const Value> Value::vi::next() {
        verify(more());
        return (*pvpValue)[nextIndex++];
    }

    Value::vi::vi(const intrusive_ptr<const Value> &pValue,
                  const vector<intrusive_ptr<const Value> > *thepvpValue):
        size(thepvpValue->size()),
        nextIndex(0),
        pvpValue(thepvpValue) {
    }

    intrusive_ptr<ValueIterator> Value::getArray() const {
        verify(getType() == Array);
        intrusive_ptr<ValueIterator> pVI(
            new vi(intrusive_ptr<const Value>(this), &vpValue));
        return pVI;
    }

    OID Value::getOid() const {
        verify(getType() == jstOID);
        return oidValue;
    }

    bool Value::getBool() const {
        verify(getType() == Bool);
        return simple.boolValue;
    }

    Date_t Value::getDate() const {
        verify(getType() == Date);
        return dateValue;
    }

    string Value::getRegex() const {
        verify(getType() == RegEx);
        return stringValue;
    }

    string Value::getSymbol() const {
        verify(getType() == Symbol);
        return stringValue;
    }

    int Value::getInt() const {
        verify(getType() == NumberInt);
        return simple.intValue;
    }

    unsigned long long Value::getTimestamp() const {
        verify(getType() == Timestamp);
        return dateValue;
    }

    long long Value::getLong() const {
        BSONType type = getType();
        if (type == NumberInt)
            return simple.intValue;

        verify(type == NumberLong);
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
            intrusive_ptr<Document> pDocument(getDocument());
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
            verify(false); // CW TODO unimplemented
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
            verify(false); // CW TODO unimplemented
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
            verify(false); // CW TODO better message
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
            verify(false); // CW TODO unimplemented
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
            verify(false); // CW TODO better message
            break;
        }

        return false;
    }

    intrusive_ptr<const Value> Value::coerceToBoolean() const {
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

        case jstNULL:
        case Undefined:
            break;

        case String:
        default:
            uassert(16003, str::stream() <<
                    "can't convert from BSON type " << type <<
                    " to int",
                    false);
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

        case jstNULL:
        case Undefined:
            break;

        case String:
        default:
            uassert(16004, str::stream() <<
                    "can't convert from BSON type " << type <<
                    " to long",
                    false);
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

        case jstNULL:
        case Undefined:
            break;

        case String:
        default:
            uassert(16005, str::stream() <<
                    "can't convert from BSON type " << type <<
                    " to double",
                    false);
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
            uassert(16006, str::stream() <<
                    "can't convert from BSON type " << type <<
                    " to double",
                    false);
        } // switch(type)

            verify(false); // CW TODO no conversion available
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
            uassert(16007, str::stream() <<
                    "can't convert from BSON type " << type <<
                    " to double",
                    false);
        } // switch(type)

        return "";
    }

    int Value::compare(const intrusive_ptr<const Value> &rL,
                       const intrusive_ptr<const Value> &rR) {
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

        /* if the comparisons are numeric, prepare to promote the values */
        if (((lType == NumberDouble) || (lType == NumberLong) ||
             (lType == NumberInt)) &&
            ((rType == NumberDouble) || (rType == NumberLong) ||
             (rType == NumberInt))) {

            /* if the biggest type of either is a double, compare as doubles */
            if ((lType == NumberDouble) || (rType == NumberDouble)) {
                const double left = rL->getDouble();
                const double right = rR->getDouble();
                if (left < right)
                    return -1;
                if (left > right)
                    return 1;
                return 0;
            }

            /* if the biggest type of either is a long, compare as longs */
            if ((lType == NumberLong) || (rType == NumberLong)) {
                const long long left = rL->getLong();
                const long long right = rR->getLong();
                if (left < right)
                    return -1;
                if (left > right)
                    return 1;
                return 0;
            }

            /* if we got here, they must both be ints; compare as ints */
            {
                const int left = rL->getInt();
                const int right = rR->getInt();
                if (left < right)
                    return -1;
                if (left > right)
                    return 1;
                return 0;
            }
        }

        // CW TODO for now, only compare like values
        uassert(16016, str::stream() <<
                "can't compare values of BSON types " << lType <<
                " and " << rType,
                lType == rType);

        switch(lType) {
        case NumberDouble:
        case NumberInt:
        case NumberLong:
            /* these types were handled above */
            verify(false);

        case String:
            return rL->stringValue.compare(rR->stringValue);

        case Object:
            return Document::compare(rL->getDocument(), rR->getDocument());

        case Array: {
            intrusive_ptr<ValueIterator> pli(rL->getArray());
            intrusive_ptr<ValueIterator> pri(rR->getArray());

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
                intrusive_ptr<const Value> plv(pli->next());
                intrusive_ptr<const Value> prv(pri->next());
                const int cmp = Value::compare(plv, prv);
                if (cmp)
                    return cmp; // values are unequal
            }

            /* NOTREACHED */
            verify(false);
            break;
        }

        case BinData:
        case Symbol:
        case CodeWScope:
            uassert(16017, str::stream() <<
                    "comparisons of values of BSON type " << lType <<
                    " are not supported", false);
            // pBuilder->appendBinData(fieldName, ...);
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

        case Timestamp:
            if (rL->dateValue < rR->dateValue)
                return -1;
            if (rL->dateValue > rR->dateValue)
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
            verify(false);
            break;
        } // switch(lType)

        /* NOTREACHED */
        return 0;
    }

    void Value::hash_combine(size_t &seed) const {
        BSONType type = getType();

        switch(type) {
            /*
              Numbers whose values are equal need to hash to the same thing
              as well.  Note that Value::compare() promotes numeric values to
              their largest common form in order for comparisons to work.
              We must hash all numeric values as if they are doubles so that
              things like grouping work.  We don't know what values will come
              down the pipe later, but if we start out with int representations
              of a value, and later see double representations of it, they need
              to end up in the same buckets.
             */
        case NumberDouble:
        case NumberLong:
        case NumberInt:
        {
            const double d = getDouble();
            boost::hash_combine(seed, d);
            break;
        }

        case String:
            boost::hash_combine(seed, stringValue);
            break;

        case Object:
            getDocument()->hash_combine(seed);
            break;

        case Array: {
            intrusive_ptr<ValueIterator> pIter(getArray());
            while(pIter->more()) {
                intrusive_ptr<const Value> pValue(pIter->next());
                pValue->hash_combine(seed);
            };
            break;
        }

        case BinData:
        case Symbol:
        case CodeWScope:
            uassert(16018, str::stream() <<
                    "hashes of values of BSON type " << type <<
                    " are not supported", false);
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

        case Timestamp:
            boost::hash_combine(seed, (unsigned long long)dateValue);
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
            verify(false); // CW TODO better message
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

    size_t Value::getApproximateSize() const {
        switch(type) {
        case String:
            return sizeof(Value) + stringValue.length();

        case Object:
            return sizeof(Value) + pDocumentValue->getApproximateSize();

        case Array: {
            size_t size = sizeof(Value);
            const size_t n = vpValue.size();
            for(size_t i = 0; i < n; ++i) {
                size += vpValue[i]->getApproximateSize();
            }
            return size;
        }

        case NumberDouble:
        case BinData:
        case jstOID:
        case Bool:
        case Date:
        case RegEx:
        case Symbol:
        case CodeWScope:
        case NumberInt:
        case Timestamp:
        case NumberLong:
        case jstNULL:
        case Undefined:
            return sizeof(Value);

            /* these shouldn't happen in this context */
        case MinKey:
        case EOO:
        case DBRef:
        case Code:
        case MaxKey:
            verify(false); // CW TODO better message
            return sizeof(Value);
        }

        /*
          We shouldn't get here.  In order to make the implementor think about
          these cases, they are all listed explicitly, above.  The compiler
          should complain if they aren't all listed, because there's no
          default.  However, not all the compilers seem to do that.  Therefore,
          this final catch-all is here.
         */
        verify(false);
        return sizeof(Value);
    }


    void ValueStatic::addRef() const {
    }

    void ValueStatic::release() const {
    }

}
