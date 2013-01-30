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

#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/mutable_bson_heap.h"

namespace mongo {
namespace mutablebson {

    //
    // BasicHeap implementation
    //

    BasicHeap::BasicHeap() {}

    BasicHeap::~BasicHeap() {}

    uint32_t BasicHeap::alloc(uint32_t size) {
        uint32_t result = _heap.size();
        _heap.reserve(result + size);
        _heap.resize(result + size);
        return result;
    }

    template<class T> T* BasicHeap::deref(uint32_t index) {
        std::vector<uint8_t>::const_iterator it = _heap.begin();
        return (T*)(&*it + index);
    }

    template<class T> const T* BasicHeap::deref(uint32_t index) const {
        std::vector<uint8_t>::const_iterator it = _heap.begin();
        return (const T*)(&*it + index);
    }

    std::string BasicHeap::getString(uint32_t index) {
        return string(BasicHeap::deref<char>(index));
    }

    char* BasicHeap::getStringBuffer(uint32_t index) {
        return BasicHeap::deref<char>(index);
    }

    uint32_t BasicHeap::putString(const StringData& s) {
        uint32_t index = alloc(s.size() + 1);
        char* buf = deref<char>(index);
        s.copyTo( buf, true );
        return index;
    }

    // stubs:
    mongo::OID BasicHeap::getOID(uint32_t index) {
        return mongo::OID();
    }

    const char* BasicHeap::getBinary(uint32_t index) {
        return NULL;
    }

    uint32_t BasicHeap::putOID(const mongo::OID& id) {
        return 0;
    }

    uint32_t BasicHeap::putBinary(const char* buf) {
        return 0;
    }

    //
    // BSONObjHeap implementation
    //

    // BSON internal format reminder:
    //    <unsigned totalSize (includes self)> {<byte BSONType><cstring FieldName><Data>}* EOO
    //
    // Data:
    //    Bool: <byte>
    //    EOO: nothing follows
    //    Undefined: nothing follows
    //    OID: an OID object
    //    NumberDouble: <double>
    //    NumberInt: <int32>
    //    String: <unsigned32 strsizewithnull><cstring>
    //    Date: <8bytes>
    //    Regex: <cstring regex><cstring options>
    //    Object: a nested object, leading with its entire size, which terminates with EOO.
    //    Array: same as object
    //    DBRef: <strlen> <cstring ns> <oid>
    //    DBRef: a database reference: basically a collection name plus an Object ID
    //    BinData: <int len> <byte subtype> <byte[len] data>
    //    Code: a function (not a closure): same format as String.
    //    Symbol: a language symbol (say a python symbol). same format as String.
    //    Code With Scope: <total size><String><Object>

    BSONObjHeap::BSONObjHeap(const BSONObj* obj) :
        _obj(obj) {
    }

    BSONObjHeap::~BSONObjHeap() {
    }

    uint32_t BSONObjHeap::alloc(uint32_t size) {
        uint32_t result = _heap.size();
        _heap.reserve(result + size);
        _heap.resize(result + size);
        return (_obj->objsize()+result);
    }

    template<class T> T* BSONObjHeap::deref(uint32_t offset) {
        if ((int)offset < _obj->objsize()) return (T*)(_obj->objdata()+offset);
        uint32_t offset0 = (offset - _obj->objsize());
        std::vector<uint8_t>::const_iterator it = _heap.begin();
        return (T*)(&*it + offset0);
    }

    template<class T> const T* BSONObjHeap::deref(uint32_t offset) const {
        if ((int)offset < _obj->objsize()) return (const T*)(_obj->objdata()+offset);
        uint32_t offset0 = (offset - _obj->objsize());
        std::vector<uint8_t>::const_iterator it = _heap.begin();
        return (const T*)(&*it + offset0);
    }

    std::string BSONObjHeap::getString(uint32_t offset) {
        return string(BSONObjHeap::deref<char>(offset));
    }

    char* BSONObjHeap::getStringBuffer(uint32_t offset) {
        return BSONObjHeap::deref<char>(offset);
    }

    uint32_t BSONObjHeap::putString(const StringData& s) {
        uint32_t offset = alloc(s.size() + 1);
        char* buf = deref<char>(offset);
        s.copyTo( buf, true );
        return offset;
    }

    // stubs:
    mongo::OID BSONObjHeap::getOID(uint32_t offset) {
        return mongo::OID();
    }

    const char* BSONObjHeap::getBinary(uint32_t offset) {
        return NULL;
    }

    uint32_t BSONObjHeap::putOID(const mongo::OID& id) {
        return 0;
    }

    uint32_t BSONObjHeap::putBinary(const char* buf) {
        return 0;
    }

} // namespace mutablebson
} // namespace mongo
