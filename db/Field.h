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

    class Field :
        public boost::enable_shared_from_this<Field>,
        boost::noncopyable
    {
    public:
	~Field();

	/*
	  Construct a Field from a BSONElement.
	*/
	static shared_ptr<Field> createFromBsonElement(BSONElement bsonElement);

	/*
	  Construct a Field from an existing field, but with a new name.  The
	  new Field will have the same value as the original.
	*/
	static shared_ptr<Field> createRename(
	    string newName, shared_ptr<Field> pField);

	/*
	  @return a pointer to the name of the field.  The pointer will live
	  as long as this Field does.
	*/
	const char *getName() const;

	/*
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
	const vector<shared_ptr<Field>> *getArray() const;
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

    private:
	Field(BSONElement bsonElement);
	Field(string newName, shared_ptr<Field> pField);

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
	vector<shared_ptr<Field>> vpField; // for arrays
    };
}
