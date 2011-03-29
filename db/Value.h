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

#pragma once

#include "pch.h"
#include "jsobj.h"

#include "db/ValueIterator.h"

namespace mongo
{
    class Document;

    /*
      Values are immutable, so these are passed around as
      shared_ptr<const Value>.  
     */
    class Value :
        public boost::enable_shared_from_this<Value>,
        boost::noncopyable
    {
    public:
	~Value();

	/*
	  Construct a Value from a BSONElement.

	  @returns a new Value initialized from the bsonElement
	*/
	static shared_ptr<const Value> createFromBsonElement(
	    BSONElement *pBsonElement);

	/*
	  Construct an integer-valued Value.

	  For commonly used values, consider using one of the singleton
	  instances defined below.

	  @param value the value
	  @returns a Value with the given value
	*/
	static shared_ptr<const Value> createInt(int value);

	/*
	  Construct an long(long)-valued Value.

	  For commonly used values, consider using one of the singleton
	  instances defined below.

	  @param value the value
	  @returns a Value with the given value
	*/
	static shared_ptr<const Value> createLong(long long value);

	/*
	  Construct a double-valued Value.

	  @param value the value
	  @returns a Value with the given value
	*/
	static shared_ptr<const Value> createDouble(double value);

	/*
	  Construct a document-valued Value.

	  @param value the value
	  @returns a Value with the given value
	*/
	static shared_ptr<const Value> createDocument(
	    shared_ptr<Document> pDocument);

	/*
	  Construct an array-valued Value.

	  @param value the value
	  @returns a Value with the given value
	*/
	static shared_ptr<const Value> createArray(
	    const vector<shared_ptr<const Value>> &vpValue);

	/*
	  Get the BSON type of the field.

	  If the type is jstNULL, no value getter will work.

	  @return the BSON type of the field.
	*/
	BSONType getType() const;

	/*
	  Getters.

	  @return the Value's value; asserts if the requested value type is
	  incorrect.
	*/
	double getDouble() const;
	string getString() const;
	shared_ptr<Document> getDocument() const;
	shared_ptr<ValueIterator> getArray() const;
	OID getOid() const;
	bool getBool() const;
	Date_t getDate() const;
	string getRegex() const;
	string getSymbol() const;
	int getInt() const;
	unsigned long long getTimestamp() const;
	long long getLong() const;

	/*
	  Add this value to the BSON object under construction.
	*/
	void addToBsonObj(BSONObjBuilder *pBuilder, string fieldName) const;

	/*
	  Add this field to the BSON array under construction.

	  As part of an array, the Value's name will be ignored.
	*/
	void addToBsonArray(BSONArrayBuilder *pBuilder) const;

	/*
	  Get references to singleton instances of commonly used field values.
	 */
	static shared_ptr<const Value> getNull();
	static shared_ptr<const Value> getTrue();
	static shared_ptr<const Value> getFalse();
	static shared_ptr<const Value> getMinusOne();
	static shared_ptr<const Value> getZero();
	static shared_ptr<const Value> getOne();

	/*
	  Coerce (cast) a value to a native bool, using JSON rules.

	  @returns the bool value
	*/
	bool coerceToBool() const;

	/*
	  Coerce (cast) a value to a Boolean Value, using JSON rules.

	  @returns the Boolean Value value
	*/
	shared_ptr<const Value> coerceToBoolean() const;

	/*
	  Coerce (cast) a value to a long long, using JSON rules.

	  @returns the long value
	*/
	long long coerceToLong() const;

	/*
	  Coerce (cast) a value to a double, using JSON rules.

	  @returns the double value
	*/
	double coerceToDouble() const;
	
	/*
	  Compare two Values.

	  @param rL left value
	  @param rR right value
	  @returns an integer less than zero, zero, or an integer greater than
	    zero, depending on whether rL < rR, rL == rR, or rL > rR
	 */
	static int compare(const shared_ptr<const Value> &rL,
			   const shared_ptr<const Value> &rR);


	/*
	  Figure out what the widest of two numeric types is.

	  Widest can be thought of as "most capable," or "able to hold the
	  largest or most precise value."  The progression is Int, Long, Double.

	  @param rL left value
	  @param rR right value
	  @returns a BSONType of NumberInt, NumberLong, or NumberDouble
	*/
	static BSONType getWidestNumeric(BSONType lType, BSONType rType);

    private:
	Value(); // creates null value
	Value(BSONElement *pBsonElement);

	Value(bool boolValue);
	Value(int intValue);
	Value(long long longValue);
	Value(double doubleValue);
	Value(shared_ptr<Document> pDocument);
	Value(const vector<shared_ptr<const Value>> &vpValue);

	BSONType type;

	/* store value in one of these */
	union
	{
	    double doubleValue;
	    bool boolValue;
	    int intValue;
	    unsigned long long timestampValue;
	    long long longValue;
	    
	} simple; // values that don't need a ctor/dtor
	OID oidValue;
	Date_t dateValue;
	string stringValue; // String, Regex, Symbol
	shared_ptr<Document> pDocumentValue;
	vector<shared_ptr<const Value>> vpValue; // for arrays


        /*
	  These are often used as the result of boolean or comparison
	  expressions.  Because these are static, and because Values are
	  passed around as shared_ptr<>, returns of these must be wrapped
	  as per this pattern:
	  http://live.boost.org/doc/libs/1_46_0/libs/smart_ptr/sp_techniques.html#static
	  Use the null_deleter defined below.

	  These are obtained via public static getters defined above.
	*/
	static const Value fieldNull;
	static const Value fieldTrue;
	static const Value fieldFalse;
	static const Value fieldMinusOne;
	static const Value fieldZero;
	static const Value fieldOne;
	
	struct null_deleter
	{
	    void operator()(void const *) const
	    {
	    }
	};

	/* this implementation is used for getArray() */
	class vi :
	    public ValueIterator,
	    boost::noncopyable
	{
	public:
	    // virtuals from ValueIterator
	    virtual bool more() const;
	    virtual shared_ptr<const Value> next();

	private:
	    friend class Value;
	    vi(shared_ptr<const Value> pSource,
	       const vector<shared_ptr<const Value>> *pvpValue);

	    size_t size;
	    size_t nextIndex;
	    const vector<shared_ptr<const Value>> *pvpValue;
	};

    };
}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo
{
    inline BSONType Value::getType() const
    {
	return type;
    }

    inline shared_ptr<const Value> Value::getNull()
    {
	shared_ptr<const Value> pValue(&fieldNull, null_deleter());
	return pValue;
    }

    inline shared_ptr<const Value> Value::getTrue()
    {
	shared_ptr<const Value> pValue(&fieldTrue, null_deleter());
	return pValue;
    }

    inline shared_ptr<const Value> Value::getFalse()
    {
	shared_ptr<const Value> pValue(&fieldFalse, null_deleter());
	return pValue;
    }

    inline shared_ptr<const Value> Value::getMinusOne()
    {
	shared_ptr<const Value> pValue(&fieldMinusOne, null_deleter());
	return pValue;
    }

    inline shared_ptr<const Value> Value::getZero()
    {
	shared_ptr<const Value> pValue(&fieldZero, null_deleter());
	return pValue;
    }

    inline shared_ptr<const Value> Value::getOne()
    {
	shared_ptr<const Value> pValue(&fieldOne, null_deleter());
	return pValue;
    }
};
