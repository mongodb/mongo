// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Defines a simple hash function class.
 */

#include "mongo/db/hasher.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/platform/endian.h"
#include "mongo/util/md5.h"

#include <cstddef>
#include <memory>

namespace mongo {

namespace {

typedef unsigned char HashDigest[16];

class Hasher {
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

public:
    using HashSeed = BSONElementHasher::HashSeed;

    explicit Hasher(HashSeed seed);
    ~Hasher() {};

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
};

Hasher::Hasher(HashSeed seed) {
    md5_init_state_deprecated(&_md5State);
    addSeed(seed);
}

void Hasher::addData(const void* keyData, size_t numBytes) {
    md5_append_deprecated(&_md5State, static_cast<const md5_byte_t*>(keyData), numBytes);
}

template <typename T>
void Hasher::addIntegerData(T number) {
    const auto data = endian::nativeToLittle(number);
    addData(&data, sizeof(data));
}

void Hasher::finish(HashDigest out) {
    md5_finish_deprecated(&_md5State, out);
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
        if (e.type() == BSONType::codeWScope) {
            h->addData(e.codeWScopeCode(), e.codeWScopeCodeLen());
            b = e.codeWScopeObject();
        } else {
            b = e.embeddedObject();
        }
        for (auto&& el : b) {
            recursiveHash(h, el, true);
        }
        // Handle EOO case
        recursiveHash(h, BSONElement{}, true);
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
