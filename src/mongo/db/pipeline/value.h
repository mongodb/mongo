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

#include "mongo/db/pipeline/value_internal.h"

namespace mongo {
    class BSONElement;
    class Builder;

    /** A variant type that can hold any type of data representable in BSON
     *
     *  Small values are stored inline, but some values, such as large strings,
     *  are heap allocated. It has smart pointer capabilities built-in so it is
     *  safe and recommended to pass these around and return them by value.
     *
     *  Values are immutable, but can be assigned. This means that once you have
     *  a Value, you can be assured that none of the data in that Value will
     *  change. However if you have a non-const Value you replace it with
     *  operator=. These rules are the same as BSONObj, and similar to
     *  shared_ptr<const Object> with stronger guarantees of constness. This is
     *  also the same as Java's String type.
     *
     *  Thread-safety: A single Value instance can be safely shared between
     *  threads as long as there are no writers while other threads are
     *  accessing the object. Any number of threads can read from a Value
     *  concurrently. There are no restrictions on how threads access Value
     *  instances exclusively owned by them, even if they reference the same
     *  storage as Value in other threads.
     */
    class Value {
    public:
        /** Construct a Value
         *
         *  All types not listed will be rejected rather than converted (see private for why)
         *
         *  Note: Currently these are all explicit conversions.
         *        I'm not sure if we want implicit or not.
         *  //TODO decide
         */

        Value(): _storage() {} // "Missing" value
        explicit Value(bool value) : _storage(Bool, value) {}
        explicit Value(int value) : _storage(NumberInt, value) {}
        explicit Value(long long value) : _storage(NumberLong, value) {}
        explicit Value(double value) : _storage(NumberDouble, value) {}
        explicit Value(const OpTime& value) : _storage(Timestamp, value.asDate()) {}
        explicit Value(const OID& value) : _storage(jstOID, value) {}
        explicit Value(StringData value) : _storage(String, value) {}
        explicit Value(const string& value) : _storage(String, StringData(value)) {}
        explicit Value(const char* value) : _storage(String, StringData(value)) {}
        explicit Value(const Document& doc) : _storage(Object, doc) {}
        explicit Value(const vector<Value>& vec) : _storage(Array, new RCVector(vec)) {}

        /** Creates an empty or zero value of specified type.
         *  This is currently the only way to create Undefined or Null Values.
         */
        explicit Value(BSONType type);

        // TODO: add an unsafe version that can share storage with the BSONElement
        /// Deep-convert from BSONElement to Value
        explicit Value(const BSONElement& elem);

        /** Construct a long or integer-valued Value.
         *
         *  Used when preforming arithmetic operations with int where the
         *  result may be too large and need to be stored as long. The Value
         *  will be an int if value fits, otherwise it will be a long.
        */
        static Value createIntOrLong(long long value);

        /** A "missing" value indicates the lack of a Value.
         *  This is similar to undefined/null but should not appear in output to BSON.
         *  Missing Values are returned by Document when accessing non-existent fields.
         */
        bool missing() const { return _storage.type == EOO; }

        /** Get the BSON type of the field.
         *  Warning: currently asserts if missing. This will probably change in the future.
         */
        BSONType getType() const { return _storage.bsonType(); }

        /** Exact type getters.
         *  Asserts if the requested value type is not exactly correct.
         *  See coerceTo methods below for a more type-flexible alternative.
         */
        double getDouble() const;
        string getString() const;
        StringData getStringData() const; // May contain embedded NUL bytes
        Document getDocument() const;
        OID getOid() const;
        bool getBool() const;
        long long getDate() const; // in milliseconds
        OpTime getTimestamp() const;
        string getRegex() const;
        string getSymbol() const;
        int getInt() const;
        long long getLong() const;
        const vector<Value>& getArray() const { return _storage.getArray(); }
        size_t getArrayLength() const;

        /// Access an element of a subarray. Returns Value() if missing or getType() != Array
        Value operator[] (size_t index) const;

        /// Access a field of a subdocument. Returns Value() if missing or getType() != Object
        Value operator[] (StringData name) const;

        /// Add this value to the BSON object under construction.
        void addToBsonObj(BSONObjBuilder* pBuilder, StringData fieldName) const;

        /// Add this field to the BSON array under construction.
        void addToBsonArray(BSONArrayBuilder* pBuilder) const;

        /** Coerce a value to a bool using BSONElement::trueValue() rules.
         *  Some types unsupported.  SERVER-6120
         */
        bool coerceToBool() const;

        /** Coercion operators to extract values with fuzzy type logic.
         *
         *  These currently assert if called on an unconvertible type.
         *  TODO: decided how to handle unsupported types.
         */
        string coerceToString() const;
        int coerceToInt() const;
        long long coerceToLong() const;
        double coerceToDouble() const;
        OpTime coerceToTimestamp() const;
        long long coerceToDate() const;
        time_t coerceToTimeT() const;
        tm coerceToTm() const; // broken-out time struct (see man gmtime)


        /** Compare two Values.
         *  @returns an integer less than zero, zero, or an integer greater than
         *           zero, depending on whether lhs < rhs, lhs == rhs, or lhs > rhs
         *  Warning: may return values other than -1, 0, or 1
         */
        static int compare(const Value& lhs, const Value& rhs);

        friend
        bool operator==(const Value& v1, const Value& v2) {
            if (v1._storage.identical(v2._storage)) {
                // Simple case
                return true;
            }
            return (Value::compare(v1, v2) == 0);
        }

        /// This is for debugging, logging, etc. See getString() for how to extract a string.
        string toString() const;
        friend ostream& operator << (ostream& out, const Value& v);

        void swap(Value& rhs) {
            _storage.swap(rhs._storage);
        }

        /** Figure out what the widest of two numeric types is.
         *
         *  Widest can be thought of as "most capable," or "able to hold the
         *  largest or most precise value."  The progression is Int, Long, Double.
         */
        static BSONType getWidestNumeric(BSONType lType, BSONType rType);

        /// Get the approximate memory size of the value, in bytes. Includes sizeof(Value)
        size_t getApproximateSize() const;

        /** Calculate a hash value.
         *
         *  Meant to be used to create composite hashes suitable for
         *  hashed container classes such as unordered_map<>.
         */
        void hash_combine(size_t& seed) const;

        /// struct Hash is defined to enable the use of Values as keys in unordered_map.
        struct Hash : unary_function<const Value&, size_t> {
            size_t operator()(const Value& rV) const;
        };

        /// Call this after memcpying to update ref counts if needed
        void memcpyed() const { _storage.memcpyed(); }

        // LEGACY creation functions
        static Value createFromBsonElement(const BSONElement* pBsonElement);
        static Value createInt(int value) { return Value(value); }
        static Value createLong(long long value) { return Value(value); }
        static Value createDouble(double value) { return Value(value); }
        static Value createTimestamp(const OpTime& value) { return Value(value); }
        static Value createString(const string& value) { return Value(value); }
        static Value createDocument(const Document& doc) { return Value(doc); }
        static Value createArray(const vector<Value>& vec) { return Value(vec); }
        static Value createDate(const long long value);

    private:
        /** This is a "honeypot" to prevent unexpected implicit conversions to the accepted argument
         *  types. bool is especially bad since without this it will accept any pointer.
         *
         *  Template argument name was chosen to make produced error easier to read.
         */
        template <typename InvalidArgumentType>
        explicit Value(const InvalidArgumentType& invalidArgument);

        void addToBson(Builder* pBuilder) const;

        ValueStorage _storage;
        friend class MutableValue; // gets and sets _storage.genericRCPtr
    };
    BOOST_STATIC_ASSERT(sizeof(Value) == 16);
}

namespace std {
    // This is used by std::sort and others
    template <>
    inline void swap(mongo::Value& lhs, mongo::Value& rhs) { lhs.swap(rhs); }
}

/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline size_t Value::getArrayLength() const {
        verify(getType() == Array);
        return getArray().size();
    }

    inline size_t Value::Hash::operator()(const Value& v) const {
        size_t seed = 0xf0afbeef;
        v.hash_combine(seed);
        return seed;
    }

    inline StringData Value::getStringData() const {
        verify(getType() == String);
        return _storage.getString();
    }

    inline string Value::getString() const {
        verify(getType() == String);
        StringData sd = _storage.getString();
        return sd.toString();
    }

    inline OID Value::getOid() const {
        verify(getType() == jstOID);
        return OID(_storage.oid);
    }

    inline bool Value::getBool() const {
        verify(getType() == Bool);
        return _storage.boolValue;
    }

    inline long long Value::getDate() const {
        verify(getType() == Date);
        return _storage.dateValue;
    }

    inline OpTime Value::getTimestamp() const {
        verify(getType() == Timestamp);
        return _storage.timestampValue;
    }

    inline string Value::getRegex() const {
        verify(getType() == RegEx);
        StringData sd = _storage.getString();
        return sd.toString();
    }

    inline string Value::getSymbol() const {
        verify(getType() == Symbol);
        StringData sd = _storage.getString();
        return sd.toString();
    }

    inline int Value::getInt() const {
        verify(getType() == NumberInt);
        return _storage.intValue;
    }

    inline long long Value::getLong() const {
        BSONType type = getType();
        if (type == NumberInt)
            return _storage.intValue;

        verify(type == NumberLong);
        return _storage.longValue;
    }
};
