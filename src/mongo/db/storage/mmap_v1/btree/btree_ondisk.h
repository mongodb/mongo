/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/jsobj.h"
#include "mongo/db/storage/mmap_v1/btree/key.h"
#include "mongo/db/storage/mmap_v1/diskloc.h"

namespace mongo {

const int OldBucketSize = 8192;

//
// On-disk index format
//

#pragma pack(1)
/**
 * This is the fixed width data component for storage of a key within a bucket.  It contains an
 * offset pointer to the variable width bson data component.  This may be 'unused', please see
 * below.
 *
 * Why is this templated on Loc?  Because V0 and V1 have different size DiskLoc(s) but otherwise
 * the same layout.
 */
template <class LocType>
struct FixedWidthKey {
    //
    // Data
    //

    /**
     * The 'left' child bucket of this key.  If this is the i-th key, it points to the i index
     * child bucket.
     */
    LocType prevChildBucket;

    /**
     * The location of the record associated with this key.
     */
    LocType recordLoc;

    /**
     * Offset within current bucket of the variable width bson key for this _KeyNode.
     */
    unsigned short _kdo;

    //
    // Accessors / mutators
    //

    short keyDataOfs() const {
        return static_cast<short>(_kdo);
    }

    void setKeyDataOfs(short s) {
        _kdo = s;
        invariant(s >= 0);
    }

    void setKeyDataOfsSavingUse(short s) {
        // XXX kill this func
        setKeyDataOfs(s);
    }

    /**
     * Unused keys are not returned by read operations.  Keys may be marked
     * as unused in cases where it is difficult to delete them while
     * maintaining the constraints required of a btree.
     *
     * Setting ofs to odd is the sentinel for unused, as real recordLoc's
     * are always even numbers.  Note we need to keep its value basically
     * the same as we use the recordLoc as part of the key in the index
     * (to handle duplicate keys efficiently).
     *
     * Flagging keys as unused is a feature that is being phased out in favor
     * of deleting the keys outright.  The current btree implementation is
     * not expected to mark a key as unused in a non legacy btree.
     */
    void setUnused() {
        recordLoc.GETOFS() |= 1;
    }

    void setUsed() {
        recordLoc.GETOFS() &= ~1;
    }

    int isUnused() const {
        return recordLoc.getOfs() & 1;
    }

    int isUsed() const {
        return !isUnused();
    }
};

/**
 * This structure represents header data for a btree bucket.  An object of
 * this type is typically allocated inside of a buffer of size BucketSize,
 * resulting in a full bucket with an appropriate header.
 *
 * The body of a btree bucket contains an array of _KeyNode objects starting
 * from its lowest indexed bytes and growing to higher indexed bytes.  The
 * body also contains variable width bson keys, which are allocated from the
 * highest indexed bytes toward lower indexed bytes.
 *
 * |hhhh|kkkkkkk--------bbbbbbbbbbbuuubbbuubbb|
 * h = header data
 * k = KeyNode data
 * - = empty space
 * b = bson key data
 * u = unused (old) bson key data, that may be garbage collected
 */
struct BtreeBucketV0 {
    /**
     * Parent bucket of this bucket, which isNull() for the root bucket.
     */
    DiskLoc parent;

    /**
     * Given that there are n keys, this is the n index child.
     */
    DiskLoc nextChild;

    /**
     * Can be reused, value is 8192 in current pdfile version Apr2010
     */
    unsigned short _wasSize;

    /**
     * zero
     */
    unsigned short _reserved1;

    int flags;

    /** basicInsert() assumes the next three members are consecutive and in this order: */

    /** Size of the empty region. */
    int emptySize;

    /** Size used for bson storage, including storage of old keys. */
    int topSize;

    /* Number of keys in the bucket. */
    int n;

    int reserved;

    /* Beginning of the bucket's body */
    char data[4];

    // Precalculated size constants
    enum { HeaderSize = 40 };
};

// BtreeBucketV0 is part of the on-disk format, so it should never be changed
static_assert(sizeof(BtreeBucketV0) - sizeof(static_cast<BtreeBucketV0*>(NULL)->data) ==
                  BtreeBucketV0::HeaderSize,
              "sizeof(BtreeBucketV0) - sizeof(static_cast<BtreeBucketV0*>(NULL)->data) == "
              "BtreeBucketV0::HeaderSize");

/**
 * A variant of DiskLoc Used by the V1 bucket type.
 */
struct DiskLoc56Bit {
    //
    // Data
    //

    int ofs;

    unsigned char _a[3];

    //
    // Accessors XXX rename these, this is terrible
    //

    int& GETOFS() {
        return ofs;
    }

    int getOfs() const {
        return ofs;
    }

    //
    // Comparison
    //

    bool isNull() const {
        return ofs < 0;
    }

    unsigned long long toLongLong() const {
        // endian
        unsigned long long result = ofs;
        char* cursor = reinterpret_cast<char*>(&result);
        *reinterpret_cast<uint16_t*>(cursor + 4) = *reinterpret_cast<const uint16_t*>(&_a[0]);
        *reinterpret_cast<uint8_t*>(cursor + 6) = *reinterpret_cast<const uint8_t*>(&_a[2]);
        *reinterpret_cast<uint8_t*>(cursor + 7) = uint8_t(0);
        return result;
    }

    bool operator<(const DiskLoc56Bit& rhs) const {
        // the orderering of dup keys in btrees isn't too critical, but we'd like to put items
        // that are close together on disk close together in the tree, so we do want the file #
        // to be the most significant bytes
        return toLongLong() < rhs.toLongLong();
    }

    int compare(const DiskLoc56Bit& rhs) const {
        unsigned long long a = toLongLong();
        unsigned long long b = rhs.toLongLong();
        if (a < b) {
            return -1;
        } else {
            return a == b ? 0 : 1;
        }
    }

    bool operator==(const DiskLoc56Bit& rhs) const {
        return toLongLong() == rhs.toLongLong();
    }

    bool operator!=(const DiskLoc56Bit& rhs) const {
        return toLongLong() != rhs.toLongLong();
    }

    bool operator==(const DiskLoc& rhs) const {
        return DiskLoc(*this) == rhs;
    }

    bool operator!=(const DiskLoc& rhs) const {
        return !(*this == rhs);
    }

    //
    // Mutation
    //

    enum {
        OurNullOfs = -2,     // first bit of offsets used in _KeyNode we don't use -1 here
        OurMaxA = 0xffffff,  // highest 3-byte value
    };

    void Null() {
        ofs = OurNullOfs;
        _a[0] = _a[1] = _a[2] = 0;
    }

    void operator=(const DiskLoc& loc);

    //
    // Type Conversion
    //

    RecordId toRecordId() const {
        return DiskLoc(*this).toRecordId();
    }

    operator DiskLoc() const {
        // endian
        if (isNull())
            return DiskLoc();
        unsigned a = *((unsigned*)(_a - 1));
        return DiskLoc(a >> 8, ofs);
    }

    std::string toString() const {
        return DiskLoc(*this).toString();
    }
};

struct BtreeBucketV1 {
    /** Parent bucket of this bucket, which isNull() for the root bucket. */
    DiskLoc56Bit parent;

    /** Given that there are n keys, this is the n index child. */
    DiskLoc56Bit nextChild;

    unsigned short flags;

    /** Size of the empty region. */
    unsigned short emptySize;

    /** Size used for bson storage, including storage of old keys. */
    unsigned short topSize;

    /* Number of keys in the bucket. */
    unsigned short n;

    /* Beginning of the bucket's body */
    char data[4];

    // Precalculated size constants
    enum { HeaderSize = 22 };
};

// BtreeBucketV1 is part of the on-disk format, so it should never be changed
static_assert(sizeof(BtreeBucketV1) - sizeof(static_cast<BtreeBucketV1*>(NULL)->data) ==
                  BtreeBucketV1::HeaderSize,
              "sizeof(BtreeBucketV1) - sizeof(static_cast<BtreeBucketV1*>(NULL)->data) == "
              "BtreeBucketV1::HeaderSize");

enum Flags { Packed = 1 };

struct BtreeLayoutV0 {
    typedef FixedWidthKey<DiskLoc> FixedWidthKeyType;
    typedef DiskLoc LocType;
    typedef KeyBson KeyType;
    typedef KeyBson KeyOwnedType;
    typedef BtreeBucketV0 BucketType;

    enum { BucketSize = 8192, BucketBodySize = BucketSize - BucketType::HeaderSize };

    // largest key size we allow.  note we very much need to support bigger keys (somehow) in
    // the future.

    static const int KeyMax = OldBucketSize / 10;

    // A sentinel value sometimes used to identify a deallocated bucket.
    static const int INVALID_N_SENTINEL = -1;

    static void initBucket(BucketType* bucket) {
        bucket->_reserved1 = 0;
        bucket->_wasSize = BucketSize;
        bucket->reserved = 0;
    }
};

struct BtreeLayoutV1 {
    typedef FixedWidthKey<DiskLoc56Bit> FixedWidthKeyType;
    typedef KeyV1 KeyType;
    typedef KeyV1Owned KeyOwnedType;
    typedef DiskLoc56Bit LocType;
    typedef BtreeBucketV1 BucketType;

    enum {
        BucketSize = 8192 - 16,  // The -16 is to leave room for the MmapV1RecordHeader header
        BucketBodySize = BucketSize - BucketType::HeaderSize
    };

    static const int KeyMax = 1024;

    // A sentinel value sometimes used to identify a deallocated bucket.
    static const unsigned short INVALID_N_SENTINEL = 0xffff;

    static void initBucket(BucketType* bucket) {}
};

#pragma pack()

}  // namespace mongo
