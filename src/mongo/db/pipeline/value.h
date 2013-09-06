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
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/value_internal.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {
    class BSONElement;

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
        explicit Value(bool value)                : _storage(Bool, value) {}
        explicit Value(int value)                 : _storage(NumberInt, value) {}
        explicit Value(long long value)           : _storage(NumberLong, value) {}
        explicit Value(double value)              : _storage(NumberDouble, value) {}
        explicit Value(const OpTime& value)       : _storage(Timestamp, value.asDate()) {}
        explicit Value(const OID& value)          : _storage(jstOID, value) {}
        explicit Value(const StringData& value)   : _storage(String, value) {}
        explicit Value(const string& value)       : _storage(String, StringData(value)) {}
        explicit Value(const char* value)         : _storage(String, StringData(value)) {}
        explicit Value(const Document& doc)       : _storage(Object, doc) {}
        explicit Value(const BSONObj& obj);
        explicit Value(const BSONArray& arr);
        explicit Value(const vector<Value>& vec)  : _storage(Array, new RCVector(vec)) {}
        explicit Value(const BSONBinData& bd)     : _storage(BinData, bd) {}
        explicit Value(const BSONRegEx& re)       : _storage(RegEx, re) {}
        explicit Value(const BSONCodeWScope& cws) : _storage(CodeWScope, cws) {}
        explicit Value(const BSONDBRef& dbref)    : _storage(DBRef, dbref) {}
        explicit Value(const BSONSymbol& sym)     : _storage(Symbol, sym.symbol) {}
        explicit Value(const BSONCode& code)      : _storage(Code, code.code) {}
        explicit Value(const NullLabeler&)        : _storage(jstNULL) {}   // BSONNull
        explicit Value(const UndefinedLabeler&)   : _storage(Undefined) {} // BSONUndefined
        explicit Value(const MinKeyLabeler&)      : _storage(MinKey) {}    // MINKEY
        explicit Value(const MaxKeyLabeler&)      : _storage(MaxKey) {}    // MAXKEY
        explicit Value(const Date_t& date)
            : _storage(Date, static_cast<long long>(date.millis)) // millis really signed
        {}

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

        /** Construct an Array-typed Value from consumed without copying the vector.
         *  consumed is replaced with an empty vector.
         *  In C++11 this would be spelled Value(std::move(consumed)).
         */
        static Value consume(vector<Value>& consumed) {
            RCVector* vec = new RCVector();
            std::swap(vec->vec, consumed);
            return Value(ValueStorage(Array, vec));
        }

        /** A "missing" value indicates the lack of a Value.
         *  This is similar to undefined/null but should not appear in output to BSON.
         *  Missing Values are returned by Document when accessing non-existent fields.
         */
        bool missing() const { return _storage.type == EOO; }

        /// true if missing() or type is jstNULL or Undefined
        bool nullish() const {
            return missing()
                || _storage.type == jstNULL
                || _storage.type == Undefined;
        }

        /// true if type represents a number
        bool numeric() const {
            return _storage.type == NumberDouble
                || _storage.type == NumberLong
                || _storage.type == NumberInt;
        }

        /// Get the BSON type of the field.
        BSONType getType() const { return _storage.bsonType(); }

        /** Exact type getters.
         *  Asserts if the requested value type is not exactly correct.
         *  See coerceTo methods below for a more type-flexible alternative.
         */
        double getDouble() const;
        string getString() const;
        Document getDocument() const;
        OID getOid() const;
        bool getBool() const;
        long long getDate() const; // in milliseconds
        OpTime getTimestamp() const;
        const char* getRegex() const;
        const char* getRegexFlags() const;
        string getSymbol() const;
        string getCode() const;
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

        // Support BSONObjBuilder and BSONArrayBuilder "stream" API
        friend BSONObjBuilder& operator << (BSONObjBuilderValueStream& builder, const Value& val);

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
        
        friend bool operator!=(const Value& v1, const Value& v2) {
            return !(v1 == v2);
        }

        friend bool operator<(const Value& lhs, const Value& rhs) {
            return (Value::compare(lhs, rhs) < 0);
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

        /// members for Sorter
        struct SorterDeserializeSettings {}; // unused
        void serializeForSorter(BufBuilder& buf) const;
        static Value deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&);
        int memUsageForSorter() const { return getApproximateSize(); }
        Value getOwned() const { return *this; }

    private:
        /** This is a "honeypot" to prevent unexpected implicit conversions to the accepted argument
         *  types. bool is especially bad since without this it will accept any pointer.
         *
         *  Template argument name was chosen to make produced error easier to read.
         */
        template <typename InvalidArgumentType>
        explicit Value(const InvalidArgumentType& invalidArgument);

        explicit Value(const ValueStorage& storage) :_storage(storage) {}

        // does no type checking
        StringData getStringData() const; // May contain embedded NUL bytes

        ValueStorage _storage;
        friend class MutableValue; // gets and sets _storage.genericRCPtr
    };
    BOOST_STATIC_ASSERT(sizeof(Value) == 16);

    typedef unordered_set<Value, Value::Hash> ValueSet;
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
        return _storage.getString();
    }

    inline string Value::getString() const {
        verify(getType() == String);
        return _storage.getString().toString();
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

    inline const char* Value::getRegex() const {
        verify(getType() == RegEx);
        return _storage.getString().rawData(); // this is known to be NUL terminated
    }
    inline const char* Value::getRegexFlags() const {
        verify(getType() == RegEx);
        const char* pattern = _storage.getString().rawData(); // this is known to be NUL terminated
        const char* flags = pattern + strlen(pattern) + 1; // first byte after pattern's NUL
        dassert(flags + strlen(flags) == pattern + _storage.getString().size());
        return flags;
    }

    inline string Value::getSymbol() const {
        verify(getType() == Symbol);
        return _storage.getString().toString();
    }
    inline string Value::getCode() const {
        verify(getType() == Code);
        return _storage.getString().toString();
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
