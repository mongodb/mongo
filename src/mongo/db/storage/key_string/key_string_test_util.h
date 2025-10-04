/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/unittest/unittest.h"

#include <random>

namespace mongo::key_string_test {
class KeyStringBuilderTest : public unittest::Test {
public:
    void run();

protected:
    key_string::Version version;
};

extern Ordering ALL_ASCENDING;
extern Ordering ONE_ASCENDING;
extern Ordering ONE_DESCENDING;

const uint64_t kMinPerfMicros = 20 * 1000;
const uint64_t kMinPerfSamples = 50 * 1000;
typedef std::vector<BSONObj> Numbers;

BSONObj toBson(const key_string::Builder& ks, Ordering ord);

template <class T>
BSONObj toBsonAndCheckKeySize(const key_string::BuilderBase<T>& ks, Ordering ord) {
    // Validate size of the key in key_string::Builder.
    ASSERT_EQUALS(ks.getSize(), key_string::getKeySize(ks.getView(), ord, ks.version));
    return key_string::toBson(ks.getView(), ord, ks.getTypeBits());
}

BSONObj toBsonAndCheckKeySize(const key_string::Value& ks, Ordering ord);

template <typename T>
void checkSizeWhileAppendingTypeBits(int numOfBitsUsedForType, T&& appendBitsFunc) {
    key_string::TypeBits typeBits(key_string::Version::V1);
    const int kItems = 10000;  // Pick an arbitrary large number.
    for (int i = 0; i < kItems; i++) {
        appendBitsFunc(typeBits);
        size_t currentRawSize = ((i + 1) * numOfBitsUsedForType - 1) / 8 + 1;
        size_t currentSize = currentRawSize;
        if (currentRawSize > key_string::TypeBits::kMaxBytesForShortEncoding) {
            // Case 4: plus 1 signal byte + 4 size bytes.
            currentSize += 5;
            ASSERT(typeBits.isLongEncoding());
        } else {
            ASSERT(!typeBits.isLongEncoding());
            if (currentRawSize == 1 && !(typeBits.getBuffer()[0] & 0x80)) {  // Case 2
                currentSize = 1;
            } else {
                // Case 3: plus 1 size byte.
                currentSize += 1;
            }
        }
        ASSERT_EQ(typeBits.getSize(), currentSize);
    }
}

// To be used by perf test for seeding, so that the entire test is repeatable in case of error.
unsigned newSeed();

const std::vector<BSONObj>& getInterestingElements(key_string::Version version);

void testPermutation(key_string::Version version,
                     const std::vector<BSONObj>& elementsOrig,
                     const std::vector<BSONObj>& orderings,
                     bool debug);

std::vector<BSONObj> thinElements(std::vector<BSONObj> elements, unsigned seed, size_t maxElements);

RecordId ridFromOid(const OID& oid);

RecordId ridFromStr(StringData str);

int compareLongToDouble(long long lhs, double rhs);

int compareNumbers(const BSONElement& lhs, const BSONElement& rhs);

BSONObj buildKeyWhichWillHaveNByteOfTypeBits(size_t n, bool allZeros);

void checkKeyWithNByteOfTypeBits(key_string::Version version, size_t n, bool allZeros);

/**
 * Evaluates ROUNDTRIP on all items in Numbers a sufficient number of times to take at least
 * kMinPerfMicros microseconds. Logs the elapsed time per ROUNDTRIP evaluation.
 */
void perfTest(key_string::Version version, const Numbers& numbers);

#define ROUNDTRIP_ORDER(version, x, order)                            \
    do {                                                              \
        const BSONObj _orig = x;                                      \
        const key_string::Builder _ks(version, _orig, order);         \
        const BSONObj _converted = toBsonAndCheckKeySize(_ks, order); \
        ASSERT_BSONOBJ_EQ(_converted, _orig);                         \
        ASSERT(_converted.binaryEqual(_orig));                        \
    } while (0)

#define ROUNDTRIP(version, x)                        \
    do {                                             \
        ROUNDTRIP_ORDER(version, x, ALL_ASCENDING);  \
        ROUNDTRIP_ORDER(version, x, ONE_DESCENDING); \
    } while (0)

#define COMPARES_SAME(_v, _x, _y)                                          \
    do {                                                                   \
        key_string::Builder _xKS(_v, _x, ONE_ASCENDING);                   \
        key_string::Builder _yKS(_v, _y, ONE_ASCENDING);                   \
        if (SimpleBSONObjComparator::kInstance.evaluate(_x == _y)) {       \
            ASSERT_EQUALS(_xKS, _yKS);                                     \
        } else if (SimpleBSONObjComparator::kInstance.evaluate(_x < _y)) { \
            ASSERT_LESS_THAN(_xKS, _yKS);                                  \
        } else {                                                           \
            ASSERT_LESS_THAN(_yKS, _xKS);                                  \
        }                                                                  \
                                                                           \
        _xKS.resetToKey(_x, ONE_DESCENDING);                               \
        _yKS.resetToKey(_y, ONE_DESCENDING);                               \
        if (SimpleBSONObjComparator::kInstance.evaluate(_x == _y)) {       \
            ASSERT_EQUALS(_xKS, _yKS);                                     \
        } else if (SimpleBSONObjComparator::kInstance.evaluate(_x < _y)) { \
            ASSERT_GREATER_THAN(_xKS, _yKS);                               \
        } else {                                                           \
            ASSERT_GREATER_THAN(_yKS, _xKS);                               \
        }                                                                  \
    } while (0)

#define COMPARE_KS_BSON(ks, bson, order)                             \
    do {                                                             \
        const BSONObj _converted = toBsonAndCheckKeySize(ks, order); \
        ASSERT_BSONOBJ_EQ(_converted, bson);                         \
        ASSERT(_converted.binaryEqual(bson));                        \
    } while (0)

#define COMPARE_HELPER(LHS, RHS) (((LHS) < (RHS)) ? -1 : (((LHS) == (RHS)) ? 0 : 1))

}  // namespace mongo::key_string_test
