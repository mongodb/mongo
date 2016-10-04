// btree_test_help.cpp : Helper functions for Btree unit-testing
//

/**
 *    Copyright (C) 2014 MongoDB
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/db/storage/mmap_v1/btree/btree_test_help.h"

#include "mongo/db/operation_context_noop.h"
#include "mongo/unittest/unittest.h"


namespace mongo {

using std::string;

string bigNumString(long long n, int len) {
    char sub[17];
    sprintf(sub, "%.16llx", n);
    string val(len, ' ');
    for (int i = 0; i < len; ++i) {
        val[i] = sub[i % 16];
    }
    return val;
}

BSONObj simpleKey(char c, int n) {
    BSONObjBuilder builder;
    string val(n, c);
    builder.append("a", val);
    return builder.obj();
}

//
// BtreeLogicTestHelper
//

template <class OnDiskFormat>
BtreeLogicTestHelper<OnDiskFormat>::BtreeLogicTestHelper(const BSONObj& order)
    : recordStore("TestRecordStore"),
      btree(&headManager,
            &recordStore,
            &cursorRegistry,
            Ordering::make(order),
            "TestIndex",
            /*isUnique*/ false) {
    static const string randomData("RandomStuff");

    // Generate a valid record location for a "fake" record, which we will repeatedly use
    // thoughout the tests.
    OperationContextNoop txn;
    StatusWith<RecordId> s =
        recordStore.insertRecord(&txn, randomData.c_str(), randomData.length(), false);

    ASSERT_TRUE(s.isOK());
    ASSERT_EQUALS(1, recordStore.numRecords(NULL));

    dummyDiskLoc = DiskLoc::fromRecordId(s.getValue());
}


//
// ArtificialTreeBuilder
//

template <class OnDiskFormat>
void ArtificialTreeBuilder<OnDiskFormat>::makeTree(const string& spec) {
    _helper->headManager.setHead(_txn, makeTree(fromjson(spec)).toRecordId());
}

template <class OnDiskFormat>
DiskLoc ArtificialTreeBuilder<OnDiskFormat>::makeTree(const BSONObj& spec) {
    DiskLoc bucketLoc = _helper->btree._addBucket(_txn);
    BucketType* bucket = _helper->btree.getBucket(_txn, bucketLoc);

    BSONObjIterator i(spec);
    while (i.more()) {
        BSONElement e = i.next();
        DiskLoc child;
        if (e.type() == Object) {
            child = makeTree(e.embeddedObject());
        }

        if (e.fieldName() == string("_")) {
            bucket->nextChild = child;
        } else {
            KeyDataOwnedType key(BSON("" << expectedKey(e.fieldName())));
            invariant(_helper->btree.pushBack(bucket, _helper->dummyDiskLoc, key, child));
        }
    }

    _helper->btree.fixParentPtrs(_txn, bucket, bucketLoc);
    return bucketLoc;
}

template <class OnDiskFormat>
void ArtificialTreeBuilder<OnDiskFormat>::checkStructure(const string& spec) const {
    checkStructure(fromjson(spec), DiskLoc::fromRecordId(_helper->headManager.getHead(_txn)));
}

template <class OnDiskFormat>
void ArtificialTreeBuilder<OnDiskFormat>::push(const DiskLoc bucketLoc,
                                               const BSONObj& key,
                                               const DiskLoc child) {
    KeyDataOwnedType k(key);
    BucketType* bucket = _helper->btree.getBucket(_txn, bucketLoc);

    invariant(_helper->btree.pushBack(bucket, _helper->dummyDiskLoc, k, child));
    _helper->btree.fixParentPtrs(_txn, bucket, bucketLoc);
}

template <class OnDiskFormat>
void ArtificialTreeBuilder<OnDiskFormat>::checkStructure(const BSONObj& spec,
                                                         const DiskLoc node) const {
    BucketType* bucket = _helper->btree.getBucket(_txn, node);

    BSONObjIterator j(spec);
    for (int i = 0; i < bucket->n; ++i) {
        ASSERT(j.more());
        BSONElement e = j.next();
        KeyHeaderType kn = BtreeLogic<OnDiskFormat>::getKeyHeader(bucket, i);
        string expected = expectedKey(e.fieldName());
        ASSERT(isPresent(BSON("" << expected), 1));
        ASSERT(isPresent(BSON("" << expected), -1));

        // ASSERT_EQUALS(expected, kn.key.toBson().firstElement().valuestr());
        if (kn.prevChildBucket.isNull()) {
            ASSERT(e.type() == jstNULL);
        } else {
            ASSERT(e.type() == Object);
            checkStructure(e.embeddedObject(), kn.prevChildBucket);
        }
    }
    if (bucket->nextChild.isNull()) {
        // maybe should allow '_' field with null value?
        ASSERT(!j.more());
    } else {
        BSONElement e = j.next();
        ASSERT_EQUALS(string("_"), e.fieldName());
        ASSERT(e.type() == Object);
        checkStructure(e.embeddedObject(), bucket->nextChild);
    }
    ASSERT(!j.more());
}

template <class OnDiskFormat>
bool ArtificialTreeBuilder<OnDiskFormat>::isPresent(const BSONObj& key, int direction) const {
    int pos;
    DiskLoc loc;
    OperationContextNoop txn;
    return _helper->btree.locate(&txn, key, _helper->dummyDiskLoc, direction, &pos, &loc);
}

// Static
template <class OnDiskFormat>
string ArtificialTreeBuilder<OnDiskFormat>::expectedKey(const char* spec) {
    if (spec[0] != '$') {
        return spec;
    }
    char* endPtr;

    // parsing a long long is a pain, so just allow shorter keys for now
    unsigned long long num = strtol(spec + 1, &endPtr, 16);
    int len = 800;
    if (*endPtr == '$') {
        len = strtol(endPtr + 1, 0, 16);
    }

    return bigNumString(num, len);
}

template <class OnDiskFormat>
int ArtificialTreeBuilder<OnDiskFormat>::fillBucketToExactSize(const DiskLoc bucketLoc,
                                                               int targetSize,
                                                               char startKey) {
    ASSERT_FALSE(bucketLoc.isNull());

    BucketType* bucket = _helper->btree.getBucket(_txn, bucketLoc);
    ASSERT_EQUALS(0, bucket->n);

    static const int bigSize = KeyDataOwnedType(simpleKey('a', 801)).dataSize();

    int size = 0;
    int keyCount = 0;
    while (size < targetSize) {
        int space = targetSize - size;
        int nextSize = space - sizeof(FixedWidthKeyType);
        verify(nextSize > 0);

        BSONObj newKey;
        if (nextSize >= bigSize) {
            newKey = simpleKey(startKey++, 801);
        } else {
            newKey = simpleKey(startKey++, nextSize - (bigSize - 801));
        }

        push(bucketLoc, newKey, DiskLoc());

        size += KeyDataOwnedType(newKey).dataSize() + sizeof(FixedWidthKeyType);
        keyCount += 1;
    }

    ASSERT_EQUALS(_helper->btree._packedDataSize(bucket, 0), targetSize);

    return keyCount;
}

//
// This causes actual code to be generated for the usages of the templates in this file.
//

// V0 format.
template struct BtreeLogicTestHelper<BtreeLayoutV0>;
template class ArtificialTreeBuilder<BtreeLayoutV0>;

// V1 format.
template struct BtreeLogicTestHelper<BtreeLayoutV1>;
template class ArtificialTreeBuilder<BtreeLayoutV1>;
}
