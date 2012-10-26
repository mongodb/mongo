/**
 * Copyright (c) 2012 10gen Inc.
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

#include <algorithm>
#include "bson/bsontypes.h"
#include "bson/oid.h"
#include "util/intrusive_counter.h"
#include "util/optime.h"


namespace mongo {
    class Document;
    class DocumentStorage;
    class Value;

    //TODO: a MutableVector, similar to MutableDocument
    /// A heap-allocated reference-counted std::vector
    class RCVector : public RefCountable {
    public:
        RCVector() {}
        RCVector(const vector<Value>& v) :vec(v) {}
        vector<Value> vec;
    };

#pragma pack(1)
    class ValueStorage {
    public:
        // This is a "missing" Value
        ValueStorage() { zero(); type = EOO; }

        ValueStorage(BSONType t)                    { zero(); type = t;}
        ValueStorage(BSONType t, int i)             { zero(); type = t; intValue = i; }
        ValueStorage(BSONType t, long long l)       { zero(); type = t; longValue = l; }
        ValueStorage(BSONType t, double d)          { zero(); type = t; doubleValue = d; }
        ValueStorage(BSONType t, ReplTime r)        { zero(); type = t; timestampValue = r; }
        ValueStorage(BSONType t, bool b)            { zero(); type = t; boolValue = b; }
        ValueStorage(BSONType t, const Document& d) { zero(); type = t; putDocument(d); }
        ValueStorage(BSONType t, const RCVector* a) { zero(); type = t; putVector(a); }
        ValueStorage(BSONType t, StringData s)      { zero(); type = t; putString(s); }

        ValueStorage(BSONType t, OID& o) {
            zero();
            type = t;
            memcpy(&oid, &o, sizeof(OID));
            BOOST_STATIC_ASSERT(sizeof(OID) == sizeof(oid));
        }

        ValueStorage(const ValueStorage& rhs) {
            memcpy(this, &rhs, sizeof(*this));
            memcpyed();
        }

        ~ValueStorage() {
            if (refCounter)
                intrusive_ptr_release(genericRCPtr);
            DEV memset(this, 0xee, sizeof(*this));
        }

        ValueStorage& operator= (ValueStorage rhsCopy) {
            this->swap(rhsCopy);
            return *this;
        }

        void swap(ValueStorage& rhs) {
            // Don't need to update ref-counts because they will be the same in the end
            char temp[sizeof(ValueStorage)];
            memcpy(temp, this, sizeof(*this));
            memcpy(this, &rhs, sizeof(*this));
            memcpy(&rhs, temp, sizeof(*this));
        }

        /// Call this after memcpying to update ref counts if needed
        void memcpyed() const {
            if (refCounter)
                intrusive_ptr_add_ref(genericRCPtr);
        }

        /// These are only to be called during Value construction on an empty Value
        void putString(StringData s);
        void putVector(const RCVector* v);
        void putDocument(const Document& d);

        StringData getString() const {
            if (shortStr) {
                return StringData(shortStrStorage, shortStrSize);
            }
            else {
                dassert(typeid(*genericRCPtr) == typeid(const RCString));
                const RCString* stringPtr = static_cast<const RCString*>(genericRCPtr);
                return StringData(stringPtr->c_str(), stringPtr->size());
            }
        }

        const vector<Value>& getArray() const {
            dassert(typeid(*genericRCPtr) == typeid(const RCVector));
            const RCVector* arrayPtr = static_cast<const RCVector*>(genericRCPtr);
            return arrayPtr->vec;
        }

        // Document is incomplete here so this can't be inline
        Document getDocument() const;

        BSONType bsonType() const {
            verify(type != EOO);
            return type;
        }

        void zero() {
            // This is important for identical()
            memset(this, 0, sizeof(*this));
        }

        // Byte-for-byte identical
        bool identical(const ValueStorage& other) const {
            return  (i64[0] == other.i64[0]
                  && i64[1] == other.i64[1]);
        }

        // This data is public because this should only be used by Value which would be a friend
        union {
            struct {
                BSONType type : 8; // one byte (offset 0)
                union { // one byte (offset 1)
                    unsigned int flags : 8;
                    struct {
                        bool refCounter : 1; // true if we need to refCount
                        bool shortStr : 1; // true if we are using short strings
                    };
                };
                union { // 14 bytes (offset 2)
                    unsigned char oid[12];
                    struct {
                        int shortStrSize : 8; // TODO Consider moving into flags union (4 bits)
                        char shortStrStorage[16 - 3]; // ValueStorage is 16 bytes, 3 byte offset
                    };
                    struct {
                        union { // 6 bytes (offset 2)
                            char pad[6];
                            char stringCache[6]; // TODO copy first few bytes of strings in here
                        };
                        union { // 8 bytes  (offset 8 and aligned)
                            // There should be no pointers to non-const data
                            const RefCountable* genericRCPtr;

                            double doubleValue;
                            bool boolValue;
                            int intValue;
                            long long longValue;
                            ReplTime timestampValue;
                            long long dateValue;
                        };
                    };
                };
            };
            long long i64[2];

            // Note the following are currently unused, but may be useful later
            int i32[4];
            unsigned long long u64[2];
            unsigned int u32[4];
            char raw[16];
        };
    };
    BOOST_STATIC_ASSERT(sizeof(ValueStorage) == 16);
#pragma pack()

}
