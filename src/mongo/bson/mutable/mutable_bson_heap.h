/* Copyright 2010 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once


#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/platform/cstdint.h"

namespace mongo {
namespace mutablebson {

    /*
     * Interface for heap implementations
     *
     * See mutable_bson_internal.h for further details.
     */
    class Heap {
    public:
         virtual ~Heap() {}

        /** Can we make these 'const' functions without triggering a 'const storm'? */
        virtual std::string getString(uint32_t index) = 0;
        virtual uint32_t putString(const StringData& s) = 0;

        virtual char* getStringBuffer(uint32_t index) = 0;

        virtual mongo::OID getOID(uint32_t index) = 0;
        virtual uint32_t putOID(const mongo::OID& oid) = 0;

        virtual const char* getBinary(uint32_t index) = 0;
        virtual uint32_t putBinary(const char* binData) = 0;
    };


    /*
     * Vector implemetation of Heap interface
     *
     * See mutable_bson_internal.h for further details.
     */
    class BasicHeap : public Heap {
    public:
        BasicHeap();
        ~BasicHeap();

        std::string getString(uint32_t index);
        uint32_t putString(const StringData& s);

        char* getStringBuffer(uint32_t index);

        mongo::OID getOID(uint32_t index);
        uint32_t putOID(const mongo::OID& oid);

        const char* getBinary(uint32_t index);
        uint32_t putBinary(const char* binData);

    private:
        uint32_t alloc(uint32_t size);
        template<class T> T* deref(uint32_t index);
        template<class T> const T* deref(uint32_t index) const;

        std::vector<uint8_t> _heap;
    };

    /*
     * BSONObj implementation of Heap interface
      * The design: store offsets directly into a given BSONObj.  These offsets can be used for
     * read operations and in-place updates.  Any update that requires additional storage is
     * handled by an auxiliary vector<byte> structure.
     *
     * See mutable_bson_internal.h for further details.
     */
    class BSONObjHeap : public Heap {
    public:
         BSONObjHeap(const BSONObj*);
         ~BSONObjHeap();

        std::string getString(uint32_t index);
        uint32_t putString(const StringData& s);

        char* getStringBuffer(uint32_t index);

        mongo::OID getOID(uint32_t index);
        uint32_t putOID(const mongo::OID& oid);

        const char* getBinary(uint32_t index);
        uint32_t putBinary(const char* binData);

    private:
        uint32_t alloc(uint32_t size);
        template<class T> T* deref(uint32_t index);
        template<class T> const T* deref(uint32_t index) const;

        const BSONObj* _obj;               // static size, read and in-place updates only
        std::vector<uint8_t> _heap;        // for insertions and non-in-place updates
    };

} // namespace mutablebson
} // namespace mongo
