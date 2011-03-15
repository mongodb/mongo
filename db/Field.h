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

namespace mongo
{
    class Document;

    /*
      Fields are immutable, so these are passed around as
      shared_ptr<const Field>.  
     */
    class Field :
        public boost::enable_shared_from_this<Field>,
        boost::noncopyable
    {
    public:
	~Field();

	/*
	  Construct a Field from a BSONElement.

	  @returns a new Field initialized from the bsonElement
	*/
	static shared_ptr<const Field> createFromBsonElement(
	    BSONElement *pBsonElement);

	/*
	  Construct a Field from an existing field, but with a new name.  The
	  new Field will have the same value as the original.

	  @returns a new Field with the same value, but with the new name
	*/
	static shared_ptr<const Field> createRename(
	    string newName, shared_ptr<const Field> pField);

	/*
	  Construct an integer-valued Field.

	  For commonly used values, consider using one of the singleton
	  instances defined below.
	*/
	static shared_ptr<const Field> createInt(string fieldName, int value);

	/*
	  @return a pointer to the name of the field.  The pointer will live
	  as long as this Field does.
	*/
	const char *getName() const;

	/*
	  Get the BSON type of the field.

	  If the type is jstNULL, no value getter will work.

	  @return the BSON type of the field.
	*/
	BSONType getType() const;

	/*
	  Getters.

	  @return the Field's value; asserts if the requested value type is
	  incorrect.
	*/
	double getDouble() const;
	string getString() const;
	shared_ptr<Document> getDocument() const;
	const vector<shared_ptr<const Field>> *getArray() const;
	OID getOid() const;
	bool getBool() const;
	Date_t getDate() const;
	string getRegex() const;
	string getSymbol() const;
	int getInt() const;
	unsigned long long getTimestamp() const;
	long long getLong() const;

	/*
	  Add this field to the BSON object under construction.

	  As part of an object, the Field's name will be used as
	  "fieldName: value".
	*/
	void addToBsonObj(BSONObjBuilder *pBuilder) const;

	/*
	  Add this field to the BSON array under construction.

	  As part of an array, the Field's name will be ignored.
	*/
	void addToBsonArray(BSONArrayBuilder *pBuilder) const;

	/*
	  Get references to singleton instances of commonly used field values.
	 */
	static shared_ptr<const Field> getNull();
	static shared_ptr<const Field> getTrue();
	static shared_ptr<const Field> getFalse();
	static shared_ptr<const Field> getMinusOne();
	static shared_ptr<const Field> getZero();
	static shared_ptr<const Field> getOne();

	/*
	  Coerce a value to a boolean, using JSON rules.
	*/
	static shared_ptr<const Field> coerceToBoolean(
	    shared_ptr<const Field> pField);

    private:
	Field(string fieldName); // creates nul value
	Field(BSONElement *pBsonElement);
	Field(string newName, shared_ptr<const Field> pField);

	Field(string fieldName, bool boolValue);
	Field(string fieldName, int intValue);
	string fieldName;
	BSONType type;

	/* store values here */
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
	vector<shared_ptr<const Field>> vpField; // for arrays


        /*
	  These are often used as the result of boolean or comparison
	  expressions.  Because these are static, and because Fields are
	  passed around as shared_ptr<>, returns of these must be wrapped
	  as per this pattern:
	  http://live.boost.org/doc/libs/1_46_0/libs/smart_ptr/sp_techniques.html#static
	  Use the null_deleter defined below.

	  These are obtained via public static getters defined above.
	*/
	static const Field fieldNull;
	static const Field fieldTrue;
	static const Field fieldFalse;
	static const Field fieldMinusOne;
	static const Field fieldZero;
	static const Field fieldOne;
	
	struct null_deleter
	{
	    void operator()(void const *) const
	    {
	    }
	};

    };
}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo
{
    inline shared_ptr<const Field> Field::getNull()
    {
	shared_ptr<const Field> pField(&fieldNull, null_deleter());
	return pField;
    }

    inline shared_ptr<const Field> Field::getTrue()
    {
	shared_ptr<const Field> pField(&fieldTrue, null_deleter());
	return pField;
    }

    inline shared_ptr<const Field> Field::getFalse()
    {
	shared_ptr<const Field> pField(&fieldFalse, null_deleter());
	return pField;
    }

    inline shared_ptr<const Field> Field::getMinusOne()
    {
	shared_ptr<const Field> pField(&fieldMinusOne, null_deleter());
	return pField;
    }

    inline shared_ptr<const Field> Field::getZero()
    {
	shared_ptr<const Field> pField(&fieldZero, null_deleter());
	return pField;
    }

    inline shared_ptr<const Field> Field::getOne()
    {
	shared_ptr<const Field> pField(&fieldOne, null_deleter());
	return pField;
    }
};
