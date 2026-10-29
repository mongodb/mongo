// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PRIVATE]] mongo {
namespace key_string_test {
class KeyStringBuilderTest : public testing::TestWithParam<key_string::Version> {
public:
    void SetUp() override {
        version = GetParam();
    }

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

RecordId ridFromStr(std::string_view str);

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

}  // namespace key_string_test
}  // namespace mongo
