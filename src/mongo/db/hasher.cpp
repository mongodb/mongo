/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * Defines a simple hash function class.
 */

#include "mongo/db/hasher.h"


#include "mongo/db/jsobj.h"
#include "mongo/util/md5.hpp"

namespace mongo {

using std::unique_ptr;

namespace {

typedef unsigned char HashDigest[16];

class Hasher {
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

public:
    explicit Hasher(HashSeed seed);
    ~Hasher(){};

    // pointer to next part of input key, length in bytes to read
    void addData(const void* keyData, size_t numBytes);

    void addSeed(int32_t number) {
        addIntegerData(number);
    }

    // All numerical values should be converted to an int64_t before being added to the hash input.
    void addNumber(int64_t number) {
        addIntegerData(number);
    }

    // finish computing the hash, put the result in the digest
    // only call this once per Hasher
    void finish(HashDigest out);

private:
    // Convert 'number' to little endian and then append it to the digest input. The number of bytes
    // appended is determined by the input type, so ensure that type T has a well defined size that
    // is the same on all platforms.
    template <typename T>
    void addIntegerData(T number);

    md5_state_t _md5State;
    HashSeed _seed;
};

Hasher::Hasher(HashSeed seed) : _seed(seed) {
    md5_init(&_md5State);
    addSeed(seed);
}

void Hasher::addData(const void* keyData, size_t numBytes) {
    md5_append(&_md5State, static_cast<const md5_byte_t*>(keyData), numBytes);
}

template <typename T>
void Hasher::addIntegerData(T number) {
    const auto data = endian::nativeToLittle(number);
    addData(&data, sizeof(data));
}

void Hasher::finish(HashDigest out) {
    md5_finish(&_md5State, out);
}

void recursiveHash(Hasher* h, const BSONElement& e, bool includeFieldName) {
    int canonicalType = endian::nativeToLittle(e.canonicalType());
    h->addData(&canonicalType, sizeof(canonicalType));

    if (includeFieldName) {
        h->addData(e.fieldName(), e.fieldNameSize());
    }

    if (!e.mayEncapsulate()) {
        // if there are no embedded objects (subobjects or arrays),
        // compute the hash, squashing numeric types to 64-bit ints
        if (e.isNumber()) {
            // Use safeNumberLongForHash, because it is well-defined for troublesome doubles.
            h->addNumber(static_cast<int64_t>(e.safeNumberLongForHash()));
        } else {
            h->addData(e.value(), e.valuesize());
        }
    } else {
        // else identify the subobject.
        // hash any preceding stuff (in the case of codeWscope)
        // then each sub-element
        // then finish with the EOO element.
        BSONObj b;
        if (e.type() == CodeWScope) {
            h->addData(e.codeWScopeCode(), e.codeWScopeCodeLen());
            b = e.codeWScopeObject();
        } else {
            b = e.embeddedObject();
        }
        BSONObjIterator i(b);
        while (i.moreWithEOO()) {
            BSONElement el = i.next();
            recursiveHash(h, el, true);
        }
    }
}

}  // namespace

long long int BSONElementHasher::hash64(const BSONElement& e, HashSeed seed) {
    Hasher h(seed);
    recursiveHash(&h, e, false);
    HashDigest d;
    h.finish(d);
    // HashDigest is actually 16 bytes, but we just read 8 bytes
    ConstDataView digestView(reinterpret_cast<const char*>(d));
    return digestView.read<LittleEndian<long long int>>();
}

}  // namespace mongo
