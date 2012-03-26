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
#include "bson/bsontypes.h"
#include "bson/oid.h"
#include "util/intrusive_counter.h"

namespace mongo {
    class BSONElement;
    class Builder;
    class Document;
    class Value;

    class ValueIterator :
        public IntrusiveCounterUnsigned {
    public:
        virtual ~ValueIterator();

        /*
          Ask if there are more fields to return.

          @returns true if there are more fields, false otherwise
        */
        virtual bool more() const = 0;

        /*
          Move the iterator to point to the next field and return it.

          @returns the next field's <name, Value>
        */
        virtual intrusive_ptr<const Value> next() = 0;
    };


    /*
      Values are immutable, so these are passed around as
      intrusive_ptr<const Value>.
     */
    class Value :
        public IntrusiveCounterUnsigned {
    public:
        ~Value();

        /*
          Construct a Value from a BSONElement.

          This ignores the name of the element, and only uses the value,
          whatever type it is.

          @returns a new Value initialized from the bsonElement
        */
        static intrusive_ptr<const Value> createFromBsonElement(
            BSONElement *pBsonElement);

        /*
          Construct an integer-valued Value.

          For commonly used values, consider using one of the singleton
          instances defined below.

          @param value the value
          @returns a Value with the given value
        */
        static intrusive_ptr<const Value> createInt(int value);

        /*
          Construct an long(long)-valued Value.

          For commonly used values, consider using one of the singleton
          instances defined below.

          @param value the value
          @returns a Value with the given value
        */
        static intrusive_ptr<const Value> createLong(long long value);

        /*
          Construct a double-valued Value.

          @param value the value
          @returns a Value with the given value
        */
        static intrusive_ptr<const Value> createDouble(double value);

        /*
          Construct a string-valued Value.

          @param value the value
          @returns a Value with the given value
        */
        static intrusive_ptr<const Value> createString(const string &value);

        /*
          Construct a date-valued Value.

          @param value the value
          @returns a Value with the given value
        */
        static intrusive_ptr<const Value> createDate(const Date_t &value);

        /*
          Construct a document-valued Value.

          @param value the value
          @returns a Value with the given value
        */
        static intrusive_ptr<const Value> createDocument(
            const intrusive_ptr<Document> &pDocument);

        /*
          Construct an array-valued Value.

          @param value the value
          @returns a Value with the given value
        */
        static intrusive_ptr<const Value> createArray(
            const vector<intrusive_ptr<const Value> > &vpValue);

        /*
          Get the BSON type of the field.

          If the type is jstNULL, no value getter will work.

          @return the BSON type of the field.
        */
        BSONType getType() const;

        /*
          Getters.

          @returns the Value's value; asserts if the requested value type is
          incorrect.
        */
        double getDouble() const;
        string getString() const;
        intrusive_ptr<Document> getDocument() const;
        intrusive_ptr<ValueIterator> getArray() const;
        OID getOid() const;
        bool getBool() const;
        Date_t getDate() const;
        string getRegex() const;
        string getSymbol() const;
        int getInt() const;
        unsigned long long getTimestamp() const;
        long long getLong() const;

        /*
          Get the length of an array value.

          @returns the length of the array, if this is array-valued; otherwise
             throws an error
        */
        size_t getArrayLength() const;

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
        static intrusive_ptr<const Value> getUndefined();
        static intrusive_ptr<const Value> getNull();
        static intrusive_ptr<const Value> getTrue();
        static intrusive_ptr<const Value> getFalse();
        static intrusive_ptr<const Value> getMinusOne();
        static intrusive_ptr<const Value> getZero();
        static intrusive_ptr<const Value> getOne();

        /*
          Coerce (cast) a value to a native bool, using JSON rules.

          @returns the bool value
        */
        bool coerceToBool() const;

        /*
          Coerce (cast) a value to a Boolean Value, using JSON rules.

          @returns the Boolean Value value
        */
        intrusive_ptr<const Value> coerceToBoolean() const;

        /*
          Coerce (cast) a value to an int, using JSON rules.

          @returns the int value
        */
        int coerceToInt() const;

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
          Coerce (cast) a value to a date, using JSON rules.

          @returns the date value
        */
        Date_t coerceToDate() const;

        /*
          Coerce (cast) a value to a string, using JSON rules.

          @returns the date value
        */
        string coerceToString() const;

        /*
          Compare two Values.

          @param rL left value
          @param rR right value
          @returns an integer less than zero, zero, or an integer greater than
            zero, depending on whether rL < rR, rL == rR, or rL > rR
         */
        static int compare(const intrusive_ptr<const Value> &rL,
                           const intrusive_ptr<const Value> &rR);


        /*
          Figure out what the widest of two numeric types is.

          Widest can be thought of as "most capable," or "able to hold the
          largest or most precise value."  The progression is Int, Long, Double.

          @param rL left value
          @param rR right value
          @returns a BSONType of NumberInt, NumberLong, or NumberDouble
        */
        static BSONType getWidestNumeric(BSONType lType, BSONType rType);

        /*
          Get the approximate storage size of the value, in bytes.

          @returns approximate storage size of the value.
         */
        size_t getApproximateSize() const;

        /*
          Calculate a hash value.

          Meant to be used to create composite hashes suitable for
          boost classes such as unordered_map<>.

          @param seed value to augment with this' hash
        */
        void hash_combine(size_t &seed) const;

        /*
          struct Hash is defined to enable the use of Values as
          keys in boost::unordered_map<>.

          Values are always referenced as immutables in the form
          intrusive_ptr<const Value>, so these operate on that construction.
        */
        struct Hash :
            unary_function<intrusive_ptr<const Value>, size_t> {
            size_t operator()(const intrusive_ptr<const Value> &rV) const;
        };

    protected:
        Value(); // creates null value
        Value(BSONType type); // creates an empty (unitialized value) of type
                                                // mostly useful for Undefined
        Value(bool boolValue);
        Value(int intValue);

    private:
        Value(BSONElement *pBsonElement);

        Value(long long longValue);
        Value(double doubleValue);
        Value(const Date_t &dateValue);
        Value(const string &stringValue);
        Value(const intrusive_ptr<Document> &pDocument);
        Value(const vector<intrusive_ptr<const Value> > &vpValue);

        void addToBson(Builder *pBuilder) const;

        BSONType type;

        /* store value in one of these */
        union {
            double doubleValue;
            bool boolValue;
            int intValue;
            unsigned long long timestampValue;
            long long longValue;

        } simple; // values that don't need a ctor/dtor
        OID oidValue;
        Date_t dateValue;
        string stringValue; // String, Regex, Symbol
        intrusive_ptr<Document> pDocumentValue;
        vector<intrusive_ptr<const Value> > vpValue; // for arrays


        /*
        These are often used as the result of boolean or comparison
        expressions.

        These are obtained via public static getters defined above.
        */
        static const intrusive_ptr<const Value> pFieldUndefined;
        static const intrusive_ptr<const Value> pFieldNull;
        static const intrusive_ptr<const Value> pFieldTrue;
        static const intrusive_ptr<const Value> pFieldFalse;
        static const intrusive_ptr<const Value> pFieldMinusOne;
        static const intrusive_ptr<const Value> pFieldZero;
        static const intrusive_ptr<const Value> pFieldOne;

        /* this implementation is used for getArray() */
        class vi :
            public ValueIterator {
        public:
            // virtuals from ValueIterator
            virtual ~vi();
            virtual bool more() const;
            virtual intrusive_ptr<const Value> next();

        private:
            friend class Value;
            vi(const intrusive_ptr<const Value> &pSource,
               const vector<intrusive_ptr<const Value> > *pvpValue);

            size_t size;
            size_t nextIndex;
            const vector<intrusive_ptr<const Value> > *pvpValue;
        }; /* class vi */

    };

    /*
      Equality operator for values.

      Useful for unordered_map<>, etc.
     */
    inline bool operator==(const intrusive_ptr<const Value> &v1,
                    const intrusive_ptr<const Value> &v2) {
        return (Value::compare(v1, v2) == 0);
    }

    /*
      For performance reasons, there are various sharable static values
      defined in class Value, obtainable by methods such as getUndefined(),
      getTrue(), getOne(), etc.  We don't want these to go away as they are
      used by a multitude of threads evaluating pipelines.  In order to avoid
      having to use atomic integers in the intrusive reference counter, this
      class overrides the reference counting methods to do nothing, making it
      safe to use for static Values.

      At this point, only the constructors necessary for the static Values in
      common use have been defined.  The remainder can be defined if necessary.
     */
    class ValueStatic :
        public Value {
    public:
        // virtuals from IntrusiveCounterUnsigned
        virtual void addRef() const;
        virtual void release() const;

        // constructors
        ValueStatic();
        ValueStatic(BSONType type);
        ValueStatic(bool boolValue);
        ValueStatic(int intValue);
    };
}

/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline BSONType Value::getType() const {
        return type;
    }

    inline size_t Value::getArrayLength() const {
        verify(getType() == Array);
        return vpValue.size();
    }

    inline intrusive_ptr<const Value> Value::getUndefined() {
        return pFieldUndefined;
    }

    inline intrusive_ptr<const Value> Value::getNull() {
        return pFieldNull;
    }

    inline intrusive_ptr<const Value> Value::getTrue() {
        return pFieldTrue;
    }

    inline intrusive_ptr<const Value> Value::getFalse() {
        return pFieldFalse;
    }

    inline intrusive_ptr<const Value> Value::getMinusOne() {
        return pFieldMinusOne;
    }

    inline intrusive_ptr<const Value> Value::getZero() {
        return pFieldZero;
    }

    inline intrusive_ptr<const Value> Value::getOne() {
        return pFieldOne;
    }

    inline size_t Value::Hash::operator()(
        const intrusive_ptr<const Value> &rV) const {
        size_t seed = 0xf0afbeef;
        rV->hash_combine(seed);
        return seed;
    }

    inline ValueStatic::ValueStatic():
        Value() {
    }

    inline ValueStatic::ValueStatic(BSONType type):
        Value(type) {
    }

    inline ValueStatic::ValueStatic(bool boolValue):
        Value(boolValue) {
    }

    inline ValueStatic::ValueStatic(int intValue):
        Value(intValue) {
    }

};
