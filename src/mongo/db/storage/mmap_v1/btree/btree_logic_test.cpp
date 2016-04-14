// btree_logic_test.cpp : Btree unit tests
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

// This file contains simple single-threaded tests, which check various aspects of the Btree logic
//

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/instance.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/mmap_v1/btree/btree_test_help.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"


namespace mongo {

using std::string;

/**
 * This class is made friend of BtreeLogic so we can add whatever private method accesses we
 * need to it, to be used by the tests.
 */
template <class BtreeLayoutType>
class BtreeLogicTestBase {
public:
    typedef typename BtreeLayoutType::BucketType BucketType;
    typedef typename BtreeLayoutType::FixedWidthKeyType FixedWidthKeyType;

    typedef typename BtreeLogic<BtreeLayoutType>::FullKey FullKey;
    typedef typename BtreeLogic<BtreeLayoutType>::KeyDataOwnedType KeyDataOwnedType;

    BtreeLogicTestBase() : _helper(BSON("TheKey" << 1)) {}

    virtual ~BtreeLogicTestBase() {}

protected:
    void checkValidNumKeys(int nKeys) {
        OperationContextNoop txn;
        ASSERT_EQUALS(nKeys, _helper.btree.fullValidate(&txn, NULL, true, false, 0));
    }

    Status insert(const BSONObj& key, const DiskLoc dl, bool dupsAllowed = true) {
        OperationContextNoop txn;
        return _helper.btree.insert(&txn, key, dl, dupsAllowed);
    }

    bool unindex(const BSONObj& key) {
        OperationContextNoop txn;
        return _helper.btree.unindex(&txn, key, _helper.dummyDiskLoc);
    }

    void locate(const BSONObj& key,
                int expectedPos,
                bool expectedFound,
                const RecordId& expectedLocation,
                int direction) {
        return locate(
            key, expectedPos, expectedFound, DiskLoc::fromRecordId(expectedLocation), direction);
    }
    void locate(const BSONObj& key,
                int expectedPos,
                bool expectedFound,
                const DiskLoc& expectedLocation,
                int direction) {
        int pos;
        DiskLoc loc;
        OperationContextNoop txn;
        ASSERT_EQUALS(expectedFound,
                      _helper.btree.locate(&txn, key, _helper.dummyDiskLoc, direction, &pos, &loc));
        ASSERT_EQUALS(expectedLocation, loc);
        ASSERT_EQUALS(expectedPos, pos);
    }

    const BucketType* child(const BucketType* bucket, int i) const {
        verify(i <= bucket->n);

        DiskLoc diskLoc;
        if (i == bucket->n) {
            diskLoc = bucket->nextChild;
        } else {
            FullKey fullKey = BtreeLogic<BtreeLayoutType>::getFullKey(bucket, i);
            diskLoc = fullKey.prevChildBucket;
        }

        verify(!diskLoc.isNull());

        return _helper.btree.getBucket(NULL, diskLoc);
    }

    BucketType* head() const {
        OperationContextNoop txn;
        return _helper.btree.getBucket(&txn, _helper.headManager.getHead(&txn));
    }

    void forcePackBucket(const RecordId bucketLoc) {
        BucketType* bucket = _helper.btree.getBucket(NULL, bucketLoc);

        bucket->topSize += bucket->emptySize;
        bucket->emptySize = 0;
        BtreeLogic<BtreeLayoutType>::setNotPacked(bucket);
    }

    void truncateBucket(BucketType* bucket, int N, int& refPos) {
        _helper.btree.truncateTo(bucket, N, refPos);
    }

    int bucketPackedDataSize(BucketType* bucket, int refPos) {
        return _helper.btree._packedDataSize(bucket, refPos);
    }

    int bucketRebalancedSeparatorPos(const RecordId bucketLoc, int leftIndex) {
        BucketType* bucket = _helper.btree.getBucket(NULL, bucketLoc);
        OperationContextNoop txn;
        return _helper.btree._rebalancedSeparatorPos(&txn, bucket, leftIndex);
    }

    FullKey getKey(const RecordId bucketLoc, int pos) const {
        const BucketType* bucket = _helper.btree.getBucket(NULL, bucketLoc);
        return BtreeLogic<BtreeLayoutType>::getFullKey(bucket, pos);
    }

    void markKeyUnused(const DiskLoc bucketLoc, int keyPos) {
        BucketType* bucket = _helper.btree.getBucket(NULL, bucketLoc);
        invariant(keyPos >= 0 && keyPos < bucket->n);

        _helper.btree.getKeyHeader(bucket, keyPos).setUnused();
    }

    DiskLoc newBucket() {
        OperationContextNoop txn;
        return _helper.btree._addBucket(&txn);
    }

    /**
     * Sets the nextChild pointer for the bucket at the specified location.
     */
    void setBucketNextChild(const DiskLoc bucketLoc, const DiskLoc nextChild) {
        OperationContextNoop txn;

        BucketType* bucket = _helper.btree.getBucket(&txn, bucketLoc);
        bucket->nextChild = nextChild;

        _helper.btree.fixParentPtrs(&txn, bucket, bucketLoc);
    }

protected:
    BtreeLogicTestHelper<BtreeLayoutType> _helper;
};

//
// TESTS
//

template <class OnDiskFormat>
class SimpleCreate : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        this->_helper.btree.initAsEmpty(&txn);

        this->checkValidNumKeys(0);
    }
};

template <class OnDiskFormat>
class SimpleInsertDelete : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        this->_helper.btree.initAsEmpty(&txn);

        BSONObj key = simpleKey('z');
        this->insert(key, this->_helper.dummyDiskLoc);

        this->checkValidNumKeys(1);
        this->locate(key, 0, true, this->_helper.headManager.getHead(&txn), 1);

        this->unindex(key);

        this->checkValidNumKeys(0);
        this->locate(key, 0, false, DiskLoc(), 1);
    }
};

template <class OnDiskFormat>
class SplitUnevenBucketBase : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        this->_helper.btree.initAsEmpty(&txn);

        for (int i = 0; i < 10; ++i) {
            BSONObj shortKey = simpleKey(shortToken(i), 1);
            this->insert(shortKey, this->_helper.dummyDiskLoc);

            BSONObj longKey = simpleKey(longToken(i), 800);
            this->insert(longKey, this->_helper.dummyDiskLoc);
        }

        this->checkValidNumKeys(20);
        ASSERT_EQUALS(1, this->head()->n);
        checkSplit();
    }

protected:
    virtual char shortToken(int i) const = 0;
    virtual char longToken(int i) const = 0;
    virtual void checkSplit() = 0;

    static char leftToken(int i) {
        return 'a' + i;
    }

    static char rightToken(int i) {
        return 'z' - i;
    }
};

template <class OnDiskFormat>
class SplitRightHeavyBucket : public SplitUnevenBucketBase<OnDiskFormat> {
private:
    virtual char shortToken(int i) const {
        return this->leftToken(i);
    }
    virtual char longToken(int i) const {
        return this->rightToken(i);
    }
    virtual void checkSplit() {
        ASSERT_EQUALS(15, this->child(this->head(), 0)->n);
        ASSERT_EQUALS(4, this->child(this->head(), 1)->n);
    }
};

template <class OnDiskFormat>
class SplitLeftHeavyBucket : public SplitUnevenBucketBase<OnDiskFormat> {
private:
    virtual char shortToken(int i) const {
        return this->rightToken(i);
    }
    virtual char longToken(int i) const {
        return this->leftToken(i);
    }
    virtual void checkSplit() {
        ASSERT_EQUALS(4, this->child(this->head(), 0)->n);
        ASSERT_EQUALS(15, this->child(this->head(), 1)->n);
    }
};

template <class OnDiskFormat>
class MissingLocate : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        this->_helper.btree.initAsEmpty(&txn);

        for (int i = 0; i < 3; ++i) {
            BSONObj k = simpleKey('b' + 2 * i);
            this->insert(k, this->_helper.dummyDiskLoc);
        }

        locateExtended(1, 'a', 'b', this->_helper.headManager.getHead(&txn));
        locateExtended(1, 'c', 'd', this->_helper.headManager.getHead(&txn));
        locateExtended(1, 'e', 'f', this->_helper.headManager.getHead(&txn));
        locateExtended(1, 'g', 'g' + 1, RecordId());  // of course, 'h' isn't in the index.

        // old behavior
        //       locateExtended( -1, 'a', 'b', dl() );
        //       locateExtended( -1, 'c', 'd', dl() );
        //       locateExtended( -1, 'e', 'f', dl() );
        //       locateExtended( -1, 'g', 'f', dl() );

        locateExtended(-1, 'a', 'a' - 1, RecordId());  // of course, 'a' - 1 isn't in the index
        locateExtended(-1, 'c', 'b', this->_helper.headManager.getHead(&txn));
        locateExtended(-1, 'e', 'd', this->_helper.headManager.getHead(&txn));
        locateExtended(-1, 'g', 'f', this->_helper.headManager.getHead(&txn));
    }

private:
    void locateExtended(int direction, char token, char expectedMatch, RecordId expectedLocation) {
        const BSONObj k = simpleKey(token);
        int expectedPos = (expectedMatch - 'b') / 2;

        this->locate(k, expectedPos, false, expectedLocation, direction);
    }
};

template <class OnDiskFormat>
class MissingLocateMultiBucket : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        this->_helper.btree.initAsEmpty(&txn);

        this->insert(simpleKey('A', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('B', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('C', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('D', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('E', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('F', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('G', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('H', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('J', 800), this->_helper.dummyDiskLoc);

        // This causes split
        this->insert(simpleKey('I', 800), this->_helper.dummyDiskLoc);

        int pos;
        DiskLoc loc;

        // 'E' is the split point and should be in the head the rest should be ~50/50
        const BSONObj splitPoint = simpleKey('E', 800);
        this->_helper.btree.locate(&txn, splitPoint, this->_helper.dummyDiskLoc, 1, &pos, &loc);
        ASSERT_EQUALS(this->_helper.headManager.getHead(&txn), loc.toRecordId());
        ASSERT_EQUALS(0, pos);

        // Find the one before 'E'
        int largePos;
        DiskLoc largeLoc;
        this->_helper.btree.locate(
            &txn, splitPoint, this->_helper.dummyDiskLoc, 1, &largePos, &largeLoc);
        this->_helper.btree.advance(&txn, &largeLoc, &largePos, -1);

        // Find the one after 'E'
        int smallPos;
        DiskLoc smallLoc;
        this->_helper.btree.locate(
            &txn, splitPoint, this->_helper.dummyDiskLoc, 1, &smallPos, &smallLoc);
        this->_helper.btree.advance(&txn, &smallLoc, &smallPos, 1);

        ASSERT_NOT_EQUALS(smallLoc, largeLoc);
        ASSERT_NOT_EQUALS(smallLoc, loc);
        ASSERT_NOT_EQUALS(largeLoc, loc);
    }
};

/**
 * Validates that adding keys incrementally produces buckets, which are 90%/10% full.
 */
template <class OnDiskFormat>
class SERVER983 : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        this->_helper.btree.initAsEmpty(&txn);

        this->insert(simpleKey('A', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('B', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('C', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('D', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('E', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('F', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('G', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('H', 800), this->_helper.dummyDiskLoc);
        this->insert(simpleKey('I', 800), this->_helper.dummyDiskLoc);

        // This will cause split
        this->insert(simpleKey('J', 800), this->_helper.dummyDiskLoc);

        int pos;
        DiskLoc loc;

        // 'H' is the maximum 'large' interval key, 90% should be < 'H' and 10% larger
        const BSONObj splitPoint = simpleKey('H', 800);
        this->_helper.btree.locate(&txn, splitPoint, this->_helper.dummyDiskLoc, 1, &pos, &loc);
        ASSERT_EQUALS(this->_helper.headManager.getHead(&txn), loc.toRecordId());
        ASSERT_EQUALS(0, pos);

        // Find the one before 'H'
        int largePos;
        DiskLoc largeLoc;
        this->_helper.btree.locate(
            &txn, splitPoint, this->_helper.dummyDiskLoc, 1, &largePos, &largeLoc);
        this->_helper.btree.advance(&txn, &largeLoc, &largePos, -1);

        // Find the one after 'H'
        int smallPos;
        DiskLoc smallLoc;
        this->_helper.btree.locate(
            &txn, splitPoint, this->_helper.dummyDiskLoc, 1, &smallPos, &smallLoc);
        this->_helper.btree.advance(&txn, &smallLoc, &smallPos, 1);

        ASSERT_NOT_EQUALS(smallLoc, largeLoc);
        ASSERT_NOT_EQUALS(smallLoc, loc);
        ASSERT_NOT_EQUALS(largeLoc, loc);
    }
};

template <class OnDiskFormat>
class DontReuseUnused : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        this->_helper.btree.initAsEmpty(&txn);

        for (int i = 0; i < 10; ++i) {
            const BSONObj k = simpleKey('b' + 2 * i, 800);
            this->insert(k, this->_helper.dummyDiskLoc);
        }

        const BSONObj root = simpleKey('p', 800);
        this->unindex(root);

        this->insert(root, this->_helper.dummyDiskLoc);
        this->locate(root, 0, true, this->head()->nextChild, 1);
    }
};

template <class OnDiskFormat>
class MergeBucketsTestBase : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        this->_helper.btree.initAsEmpty(&txn);

        for (int i = 0; i < 10; ++i) {
            const BSONObj k = simpleKey('b' + 2 * i, 800);
            this->insert(k, this->_helper.dummyDiskLoc);
        }

        // numRecords() - 1, because this->_helper.dummyDiskLoc is actually in the record store too
        ASSERT_EQUALS(3, this->_helper.recordStore.numRecords(NULL) - 1);

        long long expectedCount = 10 - unindexKeys();
        ASSERT_EQUALS(1, this->_helper.recordStore.numRecords(NULL) - 1);

        long long unusedCount = 0;
        ASSERT_EQUALS(expectedCount,
                      this->_helper.btree.fullValidate(&txn, &unusedCount, true, false, 0));
        ASSERT_EQUALS(0, unusedCount);
    }

protected:
    virtual int unindexKeys() = 0;
};

template <class OnDiskFormat>
class MergeBucketsLeft : public MergeBucketsTestBase<OnDiskFormat> {
    virtual int unindexKeys() {
        BSONObj k = simpleKey('b', 800);
        this->unindex(k);

        k = simpleKey('b' + 2, 800);
        this->unindex(k);

        k = simpleKey('b' + 4, 800);
        this->unindex(k);

        k = simpleKey('b' + 6, 800);
        this->unindex(k);

        return 4;
    }
};

template <class OnDiskFormat>
class MergeBucketsRight : public MergeBucketsTestBase<OnDiskFormat> {
    virtual int unindexKeys() {
        const BSONObj k = simpleKey('b' + 2 * 9, 800);
        this->unindex(k);
        return 1;
    }
};

template <class OnDiskFormat>
class MergeBucketsDontReplaceHead : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        this->_helper.btree.initAsEmpty(&txn);

        for (int i = 0; i < 18; ++i) {
            const BSONObj k = simpleKey('a' + i, 800);
            this->insert(k, this->_helper.dummyDiskLoc);
        }

        // numRecords(NULL) - 1, because fixedDiskLoc is actually in the record store too
        ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL) - 1);

        const BSONObj k = simpleKey('a' + 17, 800);
        this->unindex(k);
        ASSERT_EQUALS(3, this->_helper.recordStore.numRecords(NULL) - 1);

        long long unusedCount = 0;
        ASSERT_EQUALS(17, this->_helper.btree.fullValidate(&txn, &unusedCount, true, false, 0));
        ASSERT_EQUALS(0, unusedCount);
    }
};

template <class OnDiskFormat>
class MergeBucketsDelInternal : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{d:{b:{a:null},bb:null,_:{c:null}},_:{f:{e:null},_:{g:null}}}");
        ASSERT_EQUALS(8, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 7 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(8, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "bb");
        verify(this->unindex(k));

        ASSERT_EQUALS(7, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 5 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(6, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure("{b:{a:null},d:{c:null},f:{e:null},_:{g:null}}");
    }
};

template <class OnDiskFormat>
class MergeBucketsRightNull : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{d:{b:{a:null},bb:null,cc:{c:null}},_:{f:{e:null},h:{g:null}}}");
        ASSERT_EQUALS(10, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 7 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(8, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "bb");
        verify(this->unindex(k));

        ASSERT_EQUALS(9, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 5 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(6, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure("{b:{a:null},cc:{c:null},d:null,f:{e:null},h:{g:null}}");
    }
};

// This comment was here during porting, not sure what it means:
//
// "Not yet handling this case"
template <class OnDiskFormat>
class DontMergeSingleBucket : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{d:{b:{a:null},c:null}}");

        ASSERT_EQUALS(4, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "c");
        verify(this->unindex(k));

        ASSERT_EQUALS(3, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure("{d:{b:{a:null}}}");
    }
};

template <class OnDiskFormat>
class ParentMergeNonRightToLeft : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{d:{b:{a:null},bb:null,cc:{c:null}},i:{f:{e:null},h:{g:null}}}");

        ASSERT_EQUALS(11, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 7 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(8, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "bb");
        verify(this->unindex(k));

        ASSERT_EQUALS(10, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // Child does not currently replace parent in this case. Also, the tree
        // has 6 buckets + 1 for the this->_helper.dummyDiskLoc.
        ASSERT_EQUALS(7, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure("{i:{b:{a:null},cc:{c:null},d:null,f:{e:null},h:{g:null}}}");
    }
};

template <class OnDiskFormat>
class ParentMergeNonRightToRight : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{d:{b:{a:null},cc:{c:null}},i:{f:{e:null},ff:null,h:{g:null}}}");

        ASSERT_EQUALS(11, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 7 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(8, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "ff");
        verify(this->unindex(k));

        ASSERT_EQUALS(10, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // Child does not currently replace parent in this case. Also, the tree
        // has 6 buckets + 1 for the this->_helper.dummyDiskLoc.
        ASSERT_EQUALS(7, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure("{i:{b:{a:null},cc:{c:null},d:null,f:{e:null},h:{g:null}}}");
    }
};

template <class OnDiskFormat>
class CantMergeRightNoMerge : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(
            "{d:{b:{a:null},bb:null,cc:{c:null}},"
            "dd:null,"
            "_:{f:{e:null},h:{g:null}}}");

        ASSERT_EQUALS(11, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 7 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(8, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "bb");
        verify(this->unindex(k));

        ASSERT_EQUALS(10, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 7 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(8, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure(
            "{d:{b:{a:null},cc:{c:null}},"
            "dd:null,"
            "_:{f:{e:null},h:{g:null}}}");
    }
};

template <class OnDiskFormat>
class CantMergeLeftNoMerge : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{c:{b:{a:null}},d:null,_:{f:{e:null},g:null}}");

        ASSERT_EQUALS(7, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 5 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(6, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "g");
        verify(this->unindex(k));

        ASSERT_EQUALS(6, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 5 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(6, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure("{c:{b:{a:null}},d:null,_:{f:{e:null}}}");
    }
};

template <class OnDiskFormat>
class MergeOption : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{c:{b:{a:null}},f:{e:{d:null},ee:null},_:{h:{g:null}}}");

        ASSERT_EQUALS(9, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 7 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(8, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "ee");
        verify(this->unindex(k));

        ASSERT_EQUALS(8, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 6 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(7, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure("{c:{b:{a:null}},_:{e:{d:null},f:null,h:{g:null}}}");
    }
};

template <class OnDiskFormat>
class ForceMergeLeft : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{c:{b:{a:null}},f:{e:{d:null},ee:null},ff:null,_:{h:{g:null}}}");

        ASSERT_EQUALS(10, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 7 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(8, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "ee");
        verify(this->unindex(k));

        ASSERT_EQUALS(9, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 6 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(7, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure("{f:{b:{a:null},c:null,e:{d:null}},ff:null,_:{h:{g:null}}}");
    }
};

template <class OnDiskFormat>
class ForceMergeRight : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{c:{b:{a:null}},cc:null,f:{e:{d:null},ee:null},_:{h:{g:null}}}");

        ASSERT_EQUALS(10, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 7 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(8, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "ee");
        verify(this->unindex(k));

        ASSERT_EQUALS(9, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 6 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(7, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure("{c:{b:{a:null}},cc:null,_:{e:{d:null},f:null,h:{g:null}}}");
    }
};

template <class OnDiskFormat>
class RecursiveMerge : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{h:{e:{b:{a:null},c:null,d:null},g:{f:null}},j:{i:null}}");

        ASSERT_EQUALS(10, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 6 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(7, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "c");
        verify(this->unindex(k));

        ASSERT_EQUALS(9, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));

        // Height is not currently reduced in this case
        builder.checkStructure("{j:{g:{b:{a:null},d:null,e:null,f:null},h:null,i:null}}");
    }
};

template <class OnDiskFormat>
class RecursiveMergeRightBucket : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{h:{e:{b:{a:null},c:null,d:null},g:{f:null}},_:{i:null}}");

        ASSERT_EQUALS(9, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 6 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(7, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "c");
        verify(this->unindex(k));

        ASSERT_EQUALS(8, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure("{g:{b:{a:null},d:null,e:null,f:null},h:null,i:null}");
    }
};

template <class OnDiskFormat>
class RecursiveMergeDoubleRightBucket : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{h:{e:{b:{a:null},c:null,d:null},_:{f:null}},_:{i:null}}");

        ASSERT_EQUALS(8, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 6 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(7, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "c");
        verify(this->unindex(k));

        ASSERT_EQUALS(7, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));

        // no recursion currently in this case
        builder.checkStructure("{h:{b:{a:null},d:null,e:null,f:null},_:{i:null}}");
    }
};

template <class OnDiskFormat>
class MergeSizeTestBase : public BtreeLogicTestBase<OnDiskFormat> {
public:
    MergeSizeTestBase() : _count(0) {}

    void run() {
        OperationContextNoop txn;
        this->_helper.btree.initAsEmpty(&txn);

        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        const BSONObj& topKey = biggestKey('m');

        DiskLoc leftChild = this->newBucket();
        builder.push(
            DiskLoc::fromRecordId(this->_helper.headManager.getHead(&txn)), topKey, leftChild);
        _count++;

        DiskLoc rightChild = this->newBucket();
        this->setBucketNextChild(DiskLoc::fromRecordId(this->_helper.headManager.getHead(&txn)),
                                 rightChild);

        _count += builder.fillBucketToExactSize(leftChild, leftSize(), 'a');
        _count += builder.fillBucketToExactSize(rightChild, rightSize(), 'n');

        ASSERT(leftAdditional() <= 2);
        if (leftAdditional() >= 2) {
            builder.push(leftChild, bigKey('k'), DiskLoc());
        }
        if (leftAdditional() >= 1) {
            builder.push(leftChild, bigKey('l'), DiskLoc());
        }

        ASSERT(rightAdditional() <= 2);
        if (rightAdditional() >= 2) {
            builder.push(rightChild, bigKey('y'), DiskLoc());
        }
        if (rightAdditional() >= 1) {
            builder.push(rightChild, bigKey('z'), DiskLoc());
        }

        _count += leftAdditional() + rightAdditional();

        initCheck();

        const char* keys = delKeys();
        for (const char* i = keys; *i; ++i) {
            long long unused = 0;
            ASSERT_EQUALS(_count, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));
            ASSERT_EQUALS(0, unused);

            // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
            ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));

            const BSONObj k = bigKey(*i);
            this->unindex(k);

            --_count;
        }

        long long unused = 0;
        ASSERT_EQUALS(_count, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));
        ASSERT_EQUALS(0, unused);

        validate();

        if (!merge()) {
            // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
            ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));
        } else {
            // The tree has 1 bucket + 1 for the this->_helper.dummyDiskLoc
            ASSERT_EQUALS(2, this->_helper.recordStore.numRecords(NULL));
        }
    }

protected:
    virtual int leftAdditional() const {
        return 2;
    }
    virtual int rightAdditional() const {
        return 2;
    }
    virtual void initCheck() {}
    virtual void validate() {}
    virtual int leftSize() const = 0;
    virtual int rightSize() const = 0;
    virtual const char* delKeys() const {
        return "klyz";
    }
    virtual bool merge() const {
        return true;
    }

    static BSONObj bigKey(char a) {
        return simpleKey(a, 801);
    }

    static BSONObj biggestKey(char a) {
        int size = OnDiskFormat::KeyMax - bigSize() + 801;
        return simpleKey(a, size);
    }

    static int bigSize() {
        return typename BtreeLogicTestBase<OnDiskFormat>::KeyDataOwnedType(bigKey('a')).dataSize();
    }

    static int biggestSize() {
        return
            typename BtreeLogicTestBase<OnDiskFormat>::KeyDataOwnedType(biggestKey('a')).dataSize();
    }

    int _count;
};

template <class OnDiskFormat>
class MergeSizeJustRightRight : public MergeSizeTestBase<OnDiskFormat> {
protected:
    virtual int rightSize() const {
        return BtreeLogic<OnDiskFormat>::lowWaterMark() - 1;
    }

    virtual int leftSize() const {
        return OnDiskFormat::BucketBodySize - MergeSizeTestBase<OnDiskFormat>::biggestSize() -
            sizeof(typename BtreeLogicTestBase<OnDiskFormat>::FixedWidthKeyType) -
            (BtreeLogic<OnDiskFormat>::lowWaterMark() - 1);
    }
};

template <class OnDiskFormat>
class MergeSizeJustRightLeft : public MergeSizeTestBase<OnDiskFormat> {
protected:
    virtual int leftSize() const {
        return BtreeLogic<OnDiskFormat>::lowWaterMark() - 1;
    }

    virtual int rightSize() const {
        return OnDiskFormat::BucketBodySize - MergeSizeTestBase<OnDiskFormat>::biggestSize() -
            sizeof(typename BtreeLogicTestBase<OnDiskFormat>::FixedWidthKeyType) -
            (BtreeLogic<OnDiskFormat>::lowWaterMark() - 1);
    }

    virtual const char* delKeys() const {
        return "yzkl";
    }
};

template <class OnDiskFormat>
class MergeSizeRight : public MergeSizeJustRightRight<OnDiskFormat> {
    virtual int rightSize() const {
        return MergeSizeJustRightRight<OnDiskFormat>::rightSize() - 1;
    }
    virtual int leftSize() const {
        return MergeSizeJustRightRight<OnDiskFormat>::leftSize() + 1;
    }
};

template <class OnDiskFormat>
class MergeSizeLeft : public MergeSizeJustRightLeft<OnDiskFormat> {
    virtual int rightSize() const {
        return MergeSizeJustRightLeft<OnDiskFormat>::rightSize() + 1;
    }
    virtual int leftSize() const {
        return MergeSizeJustRightLeft<OnDiskFormat>::leftSize() - 1;
    }
};

template <class OnDiskFormat>
class NoMergeBelowMarkRight : public MergeSizeJustRightRight<OnDiskFormat> {
    virtual int rightSize() const {
        return MergeSizeJustRightRight<OnDiskFormat>::rightSize() + 1;
    }
    virtual int leftSize() const {
        return MergeSizeJustRightRight<OnDiskFormat>::leftSize() - 1;
    }
    virtual bool merge() const {
        return false;
    }
};

template <class OnDiskFormat>
class NoMergeBelowMarkLeft : public MergeSizeJustRightLeft<OnDiskFormat> {
    virtual int rightSize() const {
        return MergeSizeJustRightLeft<OnDiskFormat>::rightSize() - 1;
    }
    virtual int leftSize() const {
        return MergeSizeJustRightLeft<OnDiskFormat>::leftSize() + 1;
    }
    virtual bool merge() const {
        return false;
    }
};

template <class OnDiskFormat>
class MergeSizeRightTooBig : public MergeSizeJustRightLeft<OnDiskFormat> {
    virtual int rightSize() const {
        return MergeSizeJustRightLeft<OnDiskFormat>::rightSize() + 1;
    }
    virtual bool merge() const {
        return false;
    }
};

template <class OnDiskFormat>
class MergeSizeLeftTooBig : public MergeSizeJustRightRight<OnDiskFormat> {
    virtual int leftSize() const {
        return MergeSizeJustRightRight<OnDiskFormat>::leftSize() + 1;
    }
    virtual bool merge() const {
        return false;
    }
};

template <class OnDiskFormat>
class MergeRightEmpty : public MergeSizeTestBase<OnDiskFormat> {
protected:
    virtual int rightAdditional() const {
        return 1;
    }
    virtual int leftAdditional() const {
        return 1;
    }
    virtual const char* delKeys() const {
        return "lz";
    }
    virtual int rightSize() const {
        return 0;
    }
    virtual int leftSize() const {
        return OnDiskFormat::BucketBodySize - MergeSizeTestBase<OnDiskFormat>::biggestSize() -
            sizeof(typename BtreeLogicTestBase<OnDiskFormat>::FixedWidthKeyType);
    }
};

template <class OnDiskFormat>
class MergeMinRightEmpty : public MergeSizeTestBase<OnDiskFormat> {
protected:
    virtual int rightAdditional() const {
        return 1;
    }
    virtual int leftAdditional() const {
        return 0;
    }
    virtual const char* delKeys() const {
        return "z";
    }
    virtual int rightSize() const {
        return 0;
    }
    virtual int leftSize() const {
        return MergeSizeTestBase<OnDiskFormat>::bigSize() +
            sizeof(typename BtreeLogicTestBase<OnDiskFormat>::FixedWidthKeyType);
    }
};

template <class OnDiskFormat>
class MergeLeftEmpty : public MergeSizeTestBase<OnDiskFormat> {
protected:
    virtual int rightAdditional() const {
        return 1;
    }
    virtual int leftAdditional() const {
        return 1;
    }
    virtual const char* delKeys() const {
        return "zl";
    }
    virtual int leftSize() const {
        return 0;
    }
    virtual int rightSize() const {
        return OnDiskFormat::BucketBodySize - MergeSizeTestBase<OnDiskFormat>::biggestSize() -
            sizeof(typename BtreeLogicTestBase<OnDiskFormat>::FixedWidthKeyType);
    }
};

template <class OnDiskFormat>
class MergeMinLeftEmpty : public MergeSizeTestBase<OnDiskFormat> {
protected:
    virtual int leftAdditional() const {
        return 1;
    }
    virtual int rightAdditional() const {
        return 0;
    }
    virtual const char* delKeys() const {
        return "l";
    }
    virtual int leftSize() const {
        return 0;
    }
    virtual int rightSize() const {
        return MergeSizeTestBase<OnDiskFormat>::bigSize() +
            sizeof(typename BtreeLogicTestBase<OnDiskFormat>::FixedWidthKeyType);
    }
};

template <class OnDiskFormat>
class BalanceRightEmpty : public MergeRightEmpty<OnDiskFormat> {
protected:
    virtual int leftSize() const {
        return OnDiskFormat::BucketBodySize - MergeSizeTestBase<OnDiskFormat>::biggestSize() -
            sizeof(typename BtreeLogicTestBase<OnDiskFormat>::FixedWidthKeyType) + 1;
    }

    virtual bool merge() const {
        return false;
    }

    virtual void initCheck() {
        OperationContextNoop txn;
        _oldTop = this->getKey(this->_helper.headManager.getHead(&txn), 0).data.toBson();
    }

    virtual void validate() {
        OperationContextNoop txn;
        ASSERT_NOT_EQUALS(_oldTop,
                          this->getKey(this->_helper.headManager.getHead(&txn), 0).data.toBson());
    }

private:
    BSONObj _oldTop;
};

template <class OnDiskFormat>
class BalanceLeftEmpty : public MergeLeftEmpty<OnDiskFormat> {
protected:
    virtual int rightSize() const {
        return OnDiskFormat::BucketBodySize - MergeSizeTestBase<OnDiskFormat>::biggestSize() -
            sizeof(typename BtreeLogicTestBase<OnDiskFormat>::FixedWidthKeyType) + 1;
    }

    virtual bool merge() const {
        return false;
    }

    virtual void initCheck() {
        OperationContextNoop txn;
        _oldTop = this->getKey(this->_helper.headManager.getHead(&txn), 0).data.toBson();
    }

    virtual void validate() {
        OperationContextNoop txn;
        ASSERT_TRUE(_oldTop !=
                    this->getKey(this->_helper.headManager.getHead(&txn), 0).data.toBson());
    }

private:
    BSONObj _oldTop;
};

template <class OnDiskFormat>
class BalanceOneLeftToRight : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(
            "{$10:{$1:null,$2:null,$3:null,$4:null,$5:null,$6:null},"
            "b:{$20:null,$30:null,$40:null,$50:null,a:null},"
            "_:{c:null}}");

        ASSERT_EQUALS(14, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON("" << bigNumString(0x40, 800));
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(13, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure(
            "{$6:{$1:null,$2:null,$3:null,$4:null,$5:null},"
            "b:{$10:null,$20:null,$30:null,$50:null,a:null},"
            "_:{c:null}}");
    }
};

template <class OnDiskFormat>
class BalanceOneRightToLeft : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(
            "{$10:{$1:null,$2:null,$3:null,$4:null},"
            "b:{$20:null,$30:null,$40:null,$50:null,$60:null,$70:null},"
            "_:{c:null}}");

        ASSERT_EQUALS(13, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON("" << bigNumString(0x3, 800));
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(12, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure(
            "{$20:{$1:null,$2:null,$4:null,$10:null},"
            "b:{$30:null,$40:null,$50:null,$60:null,$70:null},"
            "_:{c:null}}");
    }
};

template <class OnDiskFormat>
class BalanceThreeLeftToRight : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(
            "{$20:{$1:{$0:null},$3:{$2:null},$5:{$4:null},$7:{$6:null},"
            "$9:{$8:null},$11:{$10:null},$13:{$12:null},_:{$14:null}},"
            "b:{$30:null,$40:{$35:null},$50:{$45:null}},"
            "_:{c:null}}");

        ASSERT_EQUALS(23, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 14 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(15, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON("" << bigNumString(0x30, 800));
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(22, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 14 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(15, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure(
            "{$9:{$1:{$0:null},$3:{$2:null},"
            "$5:{$4:null},$7:{$6:null},_:{$8:null}},"
            "b:{$11:{$10:null},$13:{$12:null},$20:{$14:null},"
            "$40:{$35:null},$50:{$45:null}},"
            "_:{c:null}}");
    }
};

template <class OnDiskFormat>
class BalanceThreeRightToLeft : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(
            "{$20:{$1:{$0:null},$3:{$2:null},$5:null,_:{$14:null}},"
            "b:{$30:{$25:null},$40:{$35:null},$50:{$45:null},$60:{$55:null},"
            "$70:{$65:null},$80:{$75:null},"
            "$90:{$85:null},$100:{$95:null}},"
            "_:{c:null}}");

        ASSERT_EQUALS(25, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 15 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(16, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON("" << bigNumString(0x5, 800));
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(24, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 15 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(16, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure(
            "{$50:{$1:{$0:null},$3:{$2:null},$20:{$14:null},"
            "$30:{$25:null},$40:{$35:null},_:{$45:null}},"
            "b:{$60:{$55:null},$70:{$65:null},$80:{$75:null},"
            "$90:{$85:null},$100:{$95:null}},"
            "_:{c:null}}");
    }
};

template <class OnDiskFormat>
class BalanceSingleParentKey : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(
            "{$10:{$1:null,$2:null,$3:null,$4:null,$5:null,$6:null},"
            "_:{$20:null,$30:null,$40:null,$50:null,a:null}}");

        ASSERT_EQUALS(12, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON("" << bigNumString(0x40, 800));
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(11, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure(
            "{$6:{$1:null,$2:null,$3:null,$4:null,$5:null},"
            "_:{$10:null,$20:null,$30:null,$50:null,a:null}}");
    }
};

template <class OnDiskFormat>
class PackEmptyBucket : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{a:null}");

        const BSONObj k = BSON(""
                               << "a");
        ASSERT(this->unindex(k));

        this->forcePackBucket(this->_helper.headManager.getHead(&txn));

        typename BtreeLogicTestBase<OnDiskFormat>::BucketType* headBucket = this->head();

        ASSERT_EQUALS(0, headBucket->n);
        ASSERT_FALSE(headBucket->flags & Packed);

        int unused = 0;
        this->truncateBucket(headBucket, 0, unused);

        ASSERT_EQUALS(0, headBucket->n);
        ASSERT_EQUALS(0, headBucket->topSize);
        ASSERT_EQUALS((int)OnDiskFormat::BucketBodySize, headBucket->emptySize);
        ASSERT_TRUE(headBucket->flags & Packed);
    }
};

template <class OnDiskFormat>
class PackedDataSizeEmptyBucket : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{a:null}");

        const BSONObj k = BSON(""
                               << "a");
        ASSERT(this->unindex(k));

        this->forcePackBucket(this->_helper.headManager.getHead(&txn));

        typename BtreeLogicTestBase<OnDiskFormat>::BucketType* headBucket = this->head();

        ASSERT_EQUALS(0, headBucket->n);
        ASSERT_FALSE(headBucket->flags & Packed);
        ASSERT_EQUALS(0, this->bucketPackedDataSize(headBucket, 0));
        ASSERT_FALSE(headBucket->flags & Packed);
    }
};

template <class OnDiskFormat>
class BalanceSingleParentKeyPackParent : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(
            "{$10:{$1:null,$2:null,$3:null,$4:null,$5:null,$6:null},"
            "_:{$20:null,$30:null,$40:null,$50:null,a:null}}");

        ASSERT_EQUALS(12, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));

        // force parent pack
        this->forcePackBucket(this->_helper.headManager.getHead(&txn));

        const BSONObj k = BSON("" << bigNumString(0x40, 800));
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(11, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure(
            "{$6:{$1:null,$2:null,$3:null,$4:null,$5:null},"
            "_:{$10:null,$20:null,$30:null,$50:null,a:null}}");
    }
};

template <class OnDiskFormat>
class BalanceSplitParent : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(
            "{$10$10:{$1:null,$2:null,$3:null,$4:null},"
            "$100:{$20:null,$30:null,$40:null,$50:null,$60:null,$70:null,$80:null},"
            "$200:null,$300:null,$400:null,$500:null,$600:null,"
            "$700:null,$800:null,$900:null,_:{c:null}}");

        ASSERT_EQUALS(22, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON("" << bigNumString(0x3, 800));
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(21, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 6 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(7, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure(
            "{$500:{  $30:{$1:null,$2:null,$4:null,$10$10:null,$20:null},"
            "$100:{$40:null,$50:null,$60:null,$70:null,$80:null},"
            "$200:null,$300:null,$400:null},"
            "_:{$600:null,$700:null,$800:null,$900:null,_:{c:null}}}");
    }
};

template <class OnDiskFormat>
class RebalancedSeparatorBase : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(treeSpec());
        modTree();

        ASSERT_EQUALS(
            expectedSeparator(),
            this->bucketRebalancedSeparatorPos(this->_helper.headManager.getHead(&txn), 0));
    }

    virtual string treeSpec() const = 0;
    virtual int expectedSeparator() const = 0;
    virtual void modTree() {}
};

template <class OnDiskFormat>
class EvenRebalanceLeft : public RebalancedSeparatorBase<OnDiskFormat> {
    virtual string treeSpec() const {
        return "{$7:{$1:null,$2$31f:null,$3:null,"
               "$4$31f:null,$5:null,$6:null},"
               "_:{$8:null,$9:null,$10$31e:null}}";
    }
    virtual int expectedSeparator() const {
        return 4;
    }
};

template <class OnDiskFormat>
class EvenRebalanceLeftCusp : public RebalancedSeparatorBase<OnDiskFormat> {
    virtual string treeSpec() const {
        return "{$6:{$1:null,$2$31f:null,$3:null,$4$31f:null,$5:null},"
               "_:{$7:null,$8:null,$9$31e:null,$10:null}}";
    }
    virtual int expectedSeparator() const {
        return 4;
    }
};

template <class OnDiskFormat>
class EvenRebalanceRight : public RebalancedSeparatorBase<OnDiskFormat> {
    virtual string treeSpec() const {
        return "{$3:{$1:null,$2$31f:null},_:{$4$31f:null,$5:null,$6:null,$7:null,$8$31e:null,$9:"
               "null,$10:null}}";
    }
    virtual int expectedSeparator() const {
        return 4;
    }
};

template <class OnDiskFormat>
class EvenRebalanceRightCusp : public RebalancedSeparatorBase<OnDiskFormat> {
    virtual string treeSpec() const {
        return "{$4$31f:{$1:null,$2$31f:null,$3:null},_:{$5:null,$6:null,$7$31e:null,$8:null,$9:"
               "null,$10:null}}";
    }
    virtual int expectedSeparator() const {
        return 4;
    }
};

template <class OnDiskFormat>
class EvenRebalanceCenter : public RebalancedSeparatorBase<OnDiskFormat> {
    virtual string treeSpec() const {
        return "{$5:{$1:null,$2$31f:null,$3:null,$4$31f:null},_:{$6:null,$7$31e:null,$8:null,$9:"
               "null,$10:null}}";
    }
    virtual int expectedSeparator() const {
        return 4;
    }
};

template <class OnDiskFormat>
class OddRebalanceLeft : public RebalancedSeparatorBase<OnDiskFormat> {
    virtual string treeSpec() const {
        return "{$6$31f:{$1:null,$2:null,$3:null,$4:null,$5:null},_:{$7:null,$8:null,$9:null,$10:"
               "null}}";
    }
    virtual int expectedSeparator() const {
        return 4;
    }
};

template <class OnDiskFormat>
class OddRebalanceRight : public RebalancedSeparatorBase<OnDiskFormat> {
    virtual string treeSpec() const {
        return "{$4:{$1:null,$2:null,$3:null},_:{$5:null,$6:null,$7:null,$8$31f:null,$9:null,$10:"
               "null}}";
    }
    virtual int expectedSeparator() const {
        return 4;
    }
};

template <class OnDiskFormat>
class OddRebalanceCenter : public RebalancedSeparatorBase<OnDiskFormat> {
    virtual string treeSpec() const {
        return "{$5:{$1:null,$2:null,$3:null,$4:null},_:{$6:null,$7:null,$8:null,$9:null,$10$31f:"
               "null}}";
    }
    virtual int expectedSeparator() const {
        return 4;
    }
};

template <class OnDiskFormat>
class RebalanceEmptyRight : public RebalancedSeparatorBase<OnDiskFormat> {
    virtual string treeSpec() const {
        return "{$a:{$1:null,$2:null,$3:null,$4:null,$5:null,$6:null,$7:null,$8:null,$9:null},_:{$"
               "b:null}}";
    }
    virtual void modTree() {
        BSONObj k = BSON("" << bigNumString(0xb, 800));
        ASSERT(this->unindex(k));
    }
    virtual int expectedSeparator() const {
        return 4;
    }
};

template <class OnDiskFormat>
class RebalanceEmptyLeft : public RebalancedSeparatorBase<OnDiskFormat> {
    virtual string treeSpec() const {
        return "{$a:{$1:null},_:{$11:null,$12:null,$13:null,$14:null,$15:null,$16:null,$17:null,$"
               "18:null,$19:null}}";
    }
    virtual void modTree() {
        BSONObj k = BSON("" << bigNumString(0x1, 800));
        ASSERT(this->unindex(k));
    }
    virtual int expectedSeparator() const {
        return 4;
    }
};

template <class OnDiskFormat>
class NoMoveAtLowWaterMarkRight : public MergeSizeJustRightRight<OnDiskFormat> {
    virtual int rightSize() const {
        return MergeSizeJustRightRight<OnDiskFormat>::rightSize() + 1;
    }

    virtual void initCheck() {
        OperationContextNoop txn;
        _oldTop = this->getKey(this->_helper.headManager.getHead(&txn), 0).data.toBson();
    }

    virtual void validate() {
        OperationContextNoop txn;
        ASSERT_EQUALS(_oldTop,
                      this->getKey(this->_helper.headManager.getHead(&txn), 0).data.toBson());
    }

    virtual bool merge() const {
        return false;
    }

protected:
    BSONObj _oldTop;
};

template <class OnDiskFormat>
class MoveBelowLowWaterMarkRight : public NoMoveAtLowWaterMarkRight<OnDiskFormat> {
    virtual int rightSize() const {
        return MergeSizeJustRightRight<OnDiskFormat>::rightSize();
    }
    virtual int leftSize() const {
        return MergeSizeJustRightRight<OnDiskFormat>::leftSize() + 1;
    }

    virtual void validate() {
        OperationContextNoop txn;
        // Different top means we rebalanced
        ASSERT_NOT_EQUALS(this->_oldTop,
                          this->getKey(this->_helper.headManager.getHead(&txn), 0).data.toBson());
    }
};

template <class OnDiskFormat>
class NoMoveAtLowWaterMarkLeft : public MergeSizeJustRightLeft<OnDiskFormat> {
    virtual int leftSize() const {
        return MergeSizeJustRightLeft<OnDiskFormat>::leftSize() + 1;
    }
    virtual void initCheck() {
        OperationContextNoop txn;
        this->_oldTop = this->getKey(this->_helper.headManager.getHead(&txn), 0).data.toBson();
    }

    virtual void validate() {
        OperationContextNoop txn;
        ASSERT_EQUALS(this->_oldTop,
                      this->getKey(this->_helper.headManager.getHead(&txn), 0).data.toBson());
    }
    virtual bool merge() const {
        return false;
    }

protected:
    BSONObj _oldTop;
};

template <class OnDiskFormat>
class MoveBelowLowWaterMarkLeft : public NoMoveAtLowWaterMarkLeft<OnDiskFormat> {
    virtual int leftSize() const {
        return MergeSizeJustRightLeft<OnDiskFormat>::leftSize();
    }
    virtual int rightSize() const {
        return MergeSizeJustRightLeft<OnDiskFormat>::rightSize() + 1;
    }

    virtual void validate() {
        OperationContextNoop txn;
        // Different top means we rebalanced
        ASSERT_NOT_EQUALS(this->_oldTop,
                          this->getKey(this->_helper.headManager.getHead(&txn), 0).data.toBson());
    }
};

template <class OnDiskFormat>
class PreferBalanceLeft : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(
            "{$10:{$1:null,$2:null,$3:null,$4:null,$5:null,$6:null},"
            "$20:{$11:null,$12:null,$13:null,$14:null},"
            "_:{$30:null}}");

        ASSERT_EQUALS(13, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON("" << bigNumString(0x12, 800));
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(12, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure(
            "{$5:{$1:null,$2:null,$3:null,$4:null},"
            "$20:{$6:null,$10:null,$11:null,$13:null,$14:null},"
            "_:{$30:null}}");
    }
};

template <class OnDiskFormat>
class PreferBalanceRight : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(
            "{$10:{$1:null},"
            "$20:{$11:null,$12:null,$13:null,$14:null},"
            "_:{$31:null,$32:null,$33:null,$34:null,$35:null,$36:null}}");

        ASSERT_EQUALS(13, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON("" << bigNumString(0x12, 800));
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(12, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure(
            "{$10:{$1:null},"
            "$31:{$11:null,$13:null,$14:null,$20:null},"
            "_:{$32:null,$33:null,$34:null,$35:null,$36:null}}");
    }
};

template <class OnDiskFormat>
class RecursiveMergeThenBalance : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(
            "{$10:{$5:{$1:null,$2:null},$8:{$6:null,$7:null}},"
            "_:{$20:null,$30:null,$40:null,$50:null,"
            "$60:null,$70:null,$80:null,$90:null}}");

        ASSERT_EQUALS(15, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 5 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(6, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON("" << bigNumString(0x7, 800));
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(14, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure(
            "{$40:{$8:{$1:null,$2:null,$5:null,$6:null},$10:null,$20:null,$30:null},"
            "_:{$50:null,$60:null,$70:null,$80:null,$90:null}}");
    }
};

template <class OnDiskFormat>
class DelEmptyNoNeighbors : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{b:{a:null}}");

        ASSERT_EQUALS(2, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 2 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(3, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "a");
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(1, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 1 bucket + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(2, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure("{b:null}");
    }
};

template <class OnDiskFormat>
class DelEmptyEmptyNeighbors : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{a:null,c:{b:null},d:null}");

        ASSERT_EQUALS(4, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 2 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(3, this->_helper.recordStore.numRecords(NULL));

        const BSONObj k = BSON(""
                               << "b");
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(3, this->_helper.btree.fullValidate(&txn, NULL, true, false, 0));

        // The tree has 1 bucket + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(2, this->_helper.recordStore.numRecords(NULL));

        builder.checkStructure("{a:null,c:null,d:null}");
    }
};

template <class OnDiskFormat>
class DelInternal : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{a:null,c:{b:null},d:null}");

        long long unused = 0;
        ASSERT_EQUALS(4, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 2 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(3, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        const BSONObj k = BSON(""
                               << "c");
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(3, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 1 bucket + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(2, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        builder.checkStructure("{a:null,b:null,d:null}");
    }
};

template <class OnDiskFormat>
class DelInternalReplaceWithUnused : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{a:null,c:{b:null},d:null}");

        const DiskLoc prevChildBucket =
            this->getKey(this->_helper.headManager.getHead(&txn), 1).prevChildBucket;
        this->markKeyUnused(prevChildBucket, 0);

        long long unused = 0;
        ASSERT_EQUALS(3, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 2 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(3, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(1, unused);

        const BSONObj k = BSON(""
                               << "c");
        ASSERT(this->unindex(k));

        unused = 0;
        ASSERT_EQUALS(2, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 1 bucket + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(2, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(1, unused);

        // doesn't discriminate between used and unused
        builder.checkStructure("{a:null,b:null,d:null}");
    }
};

template <class OnDiskFormat>
class DelInternalReplaceRight : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{a:null,_:{b:null}}");

        long long unused = 0;
        ASSERT_EQUALS(2, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 2 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(3, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        const BSONObj k = BSON(""
                               << "a");
        ASSERT(this->unindex(k));

        unused = 0;
        ASSERT_EQUALS(1, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 1 bucket + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(2, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        builder.checkStructure("{b:null}");
    }
};

template <class OnDiskFormat>
class DelInternalPromoteKey : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{a:null,y:{d:{c:{b:null}},_:{e:null}},z:null}");

        long long unused = 0;
        ASSERT_EQUALS(7, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 5 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(6, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        const BSONObj k = BSON(""
                               << "y");
        ASSERT(this->unindex(k));

        unused = 0;
        ASSERT_EQUALS(6, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        builder.checkStructure("{a:null,e:{c:{b:null},d:null},z:null}");
    }
};

template <class OnDiskFormat>
class DelInternalPromoteRightKey : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{a:null,_:{e:{c:null},_:{f:null}}}");

        long long unused = 0;
        ASSERT_EQUALS(4, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        const BSONObj k = BSON(""
                               << "a");
        ASSERT(this->unindex(k));

        unused = 0;
        ASSERT_EQUALS(3, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 2 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(3, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        builder.checkStructure("{c:null,_:{e:null,f:null}}");
    }
};

template <class OnDiskFormat>
class DelInternalReplacementPrevNonNull : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{a:null,d:{c:{b:null}},e:null}");

        long long unused = 0;
        ASSERT_EQUALS(5, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        const BSONObj k = BSON(""
                               << "d");
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(4, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(1, unused);

        builder.checkStructure("{a:null,d:{c:{b:null}},e:null}");

        // Check 'unused' key
        ASSERT(this->getKey(this->_helper.headManager.getHead(&txn), 1).recordLoc.getOfs() & 1);
    }
};

template <class OnDiskFormat>
class DelInternalReplacementNextNonNull : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree("{a:null,_:{c:null,_:{d:null}}}");

        long long unused = 0;
        ASSERT_EQUALS(3, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        const BSONObj k = BSON(""
                               << "a");
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(2, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 3 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(4, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(1, unused);

        builder.checkStructure("{a:null,_:{c:null,_:{d:null}}}");

        // Check 'unused' key
        ASSERT(this->getKey(this->_helper.headManager.getHead(&txn), 0).recordLoc.getOfs() & 1);
    }
};

template <class OnDiskFormat>
class DelInternalSplitPromoteLeft : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(
            "{$10:null,$20:null,$30$10:{$25:{$23:null},_:{$27:null}},"
            "$40:null,$50:null,$60:null,$70:null,$80:null,$90:null,$100:null}");

        long long unused = 0;
        ASSERT_EQUALS(13, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        const BSONObj k = BSON("" << bigNumString(0x30, 0x10));
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(12, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        builder.checkStructure(
            "{$60:{$10:null,$20:null,"
            "$27:{$23:null,$25:null},$40:null,$50:null},"
            "_:{$70:null,$80:null,$90:null,$100:null}}");
    }
};

template <class OnDiskFormat>
class DelInternalSplitPromoteRight : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        ArtificialTreeBuilder<OnDiskFormat> builder(&txn, &this->_helper);

        builder.makeTree(
            "{$10:null,$20:null,$30:null,$40:null,$50:null,$60:null,$70:null,"
            "$80:null,$90:null,$100$10:{$95:{$93:null},_:{$97:null}}}");

        long long unused = 0;
        ASSERT_EQUALS(13, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        const BSONObj k = BSON("" << bigNumString(0x100, 0x10));
        ASSERT(this->unindex(k));

        ASSERT_EQUALS(12, this->_helper.btree.fullValidate(&txn, &unused, true, false, 0));

        // The tree has 4 buckets + 1 for the this->_helper.dummyDiskLoc
        ASSERT_EQUALS(5, this->_helper.recordStore.numRecords(NULL));
        ASSERT_EQUALS(0, unused);

        builder.checkStructure(
            "{$80:{$10:null,$20:null,$30:null,$40:null,$50:null,$60:null,$70:null},"
            "_:{$90:null,$97:{$93:null,$95:null}}}");
    }
};

template <class OnDiskFormat>
class LocateEmptyForward : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        this->_helper.btree.initAsEmpty(&txn);

        BSONObj key1 = simpleKey('a');
        this->insert(key1, this->_helper.dummyDiskLoc);
        BSONObj key2 = simpleKey('b');
        this->insert(key2, this->_helper.dummyDiskLoc);
        BSONObj key3 = simpleKey('c');
        this->insert(key3, this->_helper.dummyDiskLoc);

        this->checkValidNumKeys(3);
        this->locate(BSONObj(), 0, false, this->_helper.headManager.getHead(&txn), 1);
    }
};

template <class OnDiskFormat>
class LocateEmptyReverse : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        this->_helper.btree.initAsEmpty(&txn);

        BSONObj key1 = simpleKey('a');
        this->insert(key1, this->_helper.dummyDiskLoc);
        BSONObj key2 = simpleKey('b');
        this->insert(key2, this->_helper.dummyDiskLoc);
        BSONObj key3 = simpleKey('c');
        this->insert(key3, this->_helper.dummyDiskLoc);

        this->checkValidNumKeys(3);
        this->locate(BSONObj(), -1, false, DiskLoc(), -1);
    }
};

template <class OnDiskFormat>
class DuplicateKeys : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        OperationContextNoop txn;
        this->_helper.btree.initAsEmpty(&txn);

        BSONObj key1 = simpleKey('z');
        ASSERT_OK(this->insert(key1, this->_helper.dummyDiskLoc, true));
        this->checkValidNumKeys(1);
        this->locate(key1, 0, true, this->_helper.headManager.getHead(&txn), 1);

        // Attempt to insert a dup key/value, which is okay.
        ASSERT_EQUALS(Status::OK(), this->insert(key1, this->_helper.dummyDiskLoc, true));
        this->checkValidNumKeys(1);
        this->locate(key1, 0, true, this->_helper.headManager.getHead(&txn), 1);

        // Attempt to insert a dup key/value with dupsAllowed=false.
        ASSERT_EQUALS(ErrorCodes::DuplicateKeyValue,
                      this->insert(key1, this->_helper.dummyDiskLoc, false));
        this->checkValidNumKeys(1);
        this->locate(key1, 0, true, this->_helper.headManager.getHead(&txn), 1);

        // Add another record to produce another diskloc.
        StatusWith<RecordId> s = this->_helper.recordStore.insertRecord(&txn, "a", 1, false);

        ASSERT_TRUE(s.isOK());
        ASSERT_EQUALS(3, this->_helper.recordStore.numRecords(NULL));

        const DiskLoc dummyDiskLoc2 = DiskLoc::fromRecordId(s.getValue());

        // Attempt to insert a dup key but this time with a different value.
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, this->insert(key1, dummyDiskLoc2, false));
        this->checkValidNumKeys(1);

        // Insert a dup key with dupsAllowed=true, should succeed.
        ASSERT_OK(this->insert(key1, dummyDiskLoc2, true));
        this->checkValidNumKeys(2);

        // Clean up.
        this->_helper.recordStore.deleteRecord(&txn, s.getValue());
        ASSERT_EQUALS(2, this->_helper.recordStore.numRecords(NULL));
    }
};


/* This test requires the entire server to be linked-in and it is better implemented using
   the JS framework. Disabling here and will put in jsCore.

template<class OnDiskFormat>
class SignedZeroDuplication : public BtreeLogicTestBase<OnDiskFormat> {
public:
    void run() {
        ASSERT_EQUALS(0.0, -0.0);
        DBDirectClient c;

        static const string ns("unittests.SignedZeroDuplication");

        c.ensureIndex(ns, BSON("b" << 1), true);
        c.insert(ns, BSON("b" << 0.0));
        c.insert(ns, BSON("b" << 1.0));
        c.update(ns, BSON("b" << 1.0), BSON("b" << -0.0));

        ASSERT_EQUALS(1U, c.count(ns, BSON("b" << 0.0)));
    }
};
*/

/*
// QUERY_MIGRATION: port later
    class PackUnused : public Base {
    public:
        void run() {
            for ( long long i = 0; i < 1000000; i += 1000 ) {
                insert( i );
            }
            string orig, after;
            {
                stringstream ss;
                bt()->shape( ss );
                orig = ss.str();
            }
            vector< string > toDel;
            vector< string > other;
            BSONObjBuilder start;
            start.appendMinKey( "a" );
            BSONObjBuilder end;
            end.appendMaxKey( "a" );
            unique_ptr< BtreeCursor > c( BtreeCursor::make( nsdetails( ns() ),
                                                          id(),
                                                          start.done(),
                                                          end.done(),
                                                          false,
                                                          1 ) );
            while( c->ok() ) {
                bool has_child =
                    c->getBucket().btree()->keyNode(c->getKeyOfs()).prevChildBucket.isNull();

                if (has_child) {
                    toDel.push_back( c->currKey().firstElement().valuestr() );
                }
                else {
                    other.push_back( c->currKey().firstElement().valuestr() );
                }
                c->advance();
            }
            ASSERT( toDel.size() > 0 );
            for( vector< string >::const_iterator i = toDel.begin(); i != toDel.end(); ++i ) {
                BSONObj o = BSON( "a" << *i );
                this->unindex( o );
            }
            ASSERT( other.size() > 0 );
            for( vector< string >::const_iterator i = other.begin(); i != other.end(); ++i ) {
                BSONObj o = BSON( "a" << *i );
                this->unindex( o );
            }

            long long unused = 0;
            ASSERT_EQUALS( 0, bt()->fullValidate(&txn,  dl(), order(), &unused, true ) );

            for ( long long i = 50000; i < 50100; ++i ) {
                insert( i );
            }

            long long unused2 = 0;
            ASSERT_EQUALS( 100, bt()->fullValidate(&txn,  dl(), order(), &unused2, true ) );

//            log() << "old unused: " << unused << ", new unused: " << unused2 << endl;
//
            ASSERT( unused2 <= unused );
        }
    protected:
        void insert( long long n ) {
            string val = bigNumString( n );
            BSONObj k = BSON( "a" << val );
            Base::insert( k );
        }
    };

    class DontDropReferenceKey : public PackUnused {
    public:
        void run() {
            // with 80 root node is full
            for ( long long i = 0; i < 80; i += 1 ) {
                insert( i );
            }

            BSONObjBuilder start;
            start.appendMinKey( "a" );
            BSONObjBuilder end;
            end.appendMaxKey( "a" );
            BSONObj l = bt()->keyNode( 0 ).key.toBson();
            string toInsert;
            unique_ptr< BtreeCursor > c( BtreeCursor::make( nsdetails( ns() ),
                                                          id(),
                                                          start.done(),
                                                          end.done(),
                                                          false,
                                                          1 ) );
            while( c->ok() ) {
                if ( c->currKey().woCompare( l ) > 0 ) {
                    toInsert = c->currKey().firstElement().valuestr();
                    break;
                }
                c->advance();
            }
            // too much work to try to make this happen through inserts and deletes
            // we are intentionally manipulating the btree bucket directly here
            BtreeBucket::Loc* L = const_cast< BtreeBucket::Loc* >(
                            &bt()->keyNode( 1 ).prevChildBucket );
            writing(L)->Null();
            writingInt( const_cast< BtreeBucket::Loc& >(
                            bt()->keyNode( 1 ).recordLoc ).GETOFS() ) |= 1; // make unused
            BSONObj k = BSON( "a" << toInsert );
            Base::insert( k );
        }
    };
    */

//
// TEST SUITE DEFINITION
//

template <class OnDiskFormat>
class BtreeLogicTestSuite : public unittest::Suite {
public:
    BtreeLogicTestSuite(const std::string& name) : Suite(name) {}

    void setupTests() {
        add<SimpleCreate<OnDiskFormat>>();
        add<SimpleInsertDelete<OnDiskFormat>>();
        add<SplitRightHeavyBucket<OnDiskFormat>>();
        add<SplitLeftHeavyBucket<OnDiskFormat>>();
        add<MissingLocate<OnDiskFormat>>();
        add<MissingLocateMultiBucket<OnDiskFormat>>();
        add<SERVER983<OnDiskFormat>>();
        add<DontReuseUnused<OnDiskFormat>>();
        add<MergeBucketsLeft<OnDiskFormat>>();
        add<MergeBucketsRight<OnDiskFormat>>();
        add<MergeBucketsDontReplaceHead<OnDiskFormat>>();
        add<MergeBucketsDelInternal<OnDiskFormat>>();
        add<MergeBucketsRightNull<OnDiskFormat>>();
        add<DontMergeSingleBucket<OnDiskFormat>>();
        add<ParentMergeNonRightToLeft<OnDiskFormat>>();
        add<ParentMergeNonRightToRight<OnDiskFormat>>();
        add<CantMergeRightNoMerge<OnDiskFormat>>();
        add<CantMergeLeftNoMerge<OnDiskFormat>>();
        add<MergeOption<OnDiskFormat>>();
        add<ForceMergeLeft<OnDiskFormat>>();
        add<ForceMergeRight<OnDiskFormat>>();
        add<RecursiveMerge<OnDiskFormat>>();
        add<RecursiveMergeRightBucket<OnDiskFormat>>();
        add<RecursiveMergeDoubleRightBucket<OnDiskFormat>>();

        add<MergeSizeJustRightRight<OnDiskFormat>>();
        add<MergeSizeJustRightLeft<OnDiskFormat>>();
        add<MergeSizeRight<OnDiskFormat>>();
        add<MergeSizeLeft<OnDiskFormat>>();
        add<NoMergeBelowMarkRight<OnDiskFormat>>();
        add<NoMergeBelowMarkLeft<OnDiskFormat>>();
        add<MergeSizeRightTooBig<OnDiskFormat>>();
        add<MergeSizeLeftTooBig<OnDiskFormat>>();
        add<MergeRightEmpty<OnDiskFormat>>();
        add<MergeMinRightEmpty<OnDiskFormat>>();
        add<MergeLeftEmpty<OnDiskFormat>>();
        add<MergeMinLeftEmpty<OnDiskFormat>>();
        add<BalanceRightEmpty<OnDiskFormat>>();
        add<BalanceLeftEmpty<OnDiskFormat>>();

        add<BalanceOneLeftToRight<OnDiskFormat>>();
        add<BalanceOneRightToLeft<OnDiskFormat>>();
        add<BalanceThreeLeftToRight<OnDiskFormat>>();
        add<BalanceThreeRightToLeft<OnDiskFormat>>();
        add<BalanceSingleParentKey<OnDiskFormat>>();

        add<PackEmptyBucket<OnDiskFormat>>();
        add<PackedDataSizeEmptyBucket<OnDiskFormat>>();

        add<BalanceSingleParentKeyPackParent<OnDiskFormat>>();
        add<BalanceSplitParent<OnDiskFormat>>();
        add<EvenRebalanceLeft<OnDiskFormat>>();
        add<EvenRebalanceLeftCusp<OnDiskFormat>>();
        add<EvenRebalanceRight<OnDiskFormat>>();
        add<EvenRebalanceRightCusp<OnDiskFormat>>();
        add<EvenRebalanceCenter<OnDiskFormat>>();
        add<OddRebalanceLeft<OnDiskFormat>>();
        add<OddRebalanceRight<OnDiskFormat>>();
        add<OddRebalanceCenter<OnDiskFormat>>();
        add<RebalanceEmptyRight<OnDiskFormat>>();
        add<RebalanceEmptyLeft<OnDiskFormat>>();

        add<NoMoveAtLowWaterMarkRight<OnDiskFormat>>();
        add<MoveBelowLowWaterMarkRight<OnDiskFormat>>();
        add<NoMoveAtLowWaterMarkLeft<OnDiskFormat>>();
        add<MoveBelowLowWaterMarkLeft<OnDiskFormat>>();

        add<PreferBalanceLeft<OnDiskFormat>>();
        add<PreferBalanceRight<OnDiskFormat>>();
        add<RecursiveMergeThenBalance<OnDiskFormat>>();
        add<DelEmptyNoNeighbors<OnDiskFormat>>();
        add<DelEmptyEmptyNeighbors<OnDiskFormat>>();
        add<DelInternal<OnDiskFormat>>();
        add<DelInternalReplaceWithUnused<OnDiskFormat>>();
        add<DelInternalReplaceRight<OnDiskFormat>>();
        add<DelInternalPromoteKey<OnDiskFormat>>();
        add<DelInternalPromoteRightKey<OnDiskFormat>>();
        add<DelInternalReplacementPrevNonNull<OnDiskFormat>>();
        add<DelInternalReplacementNextNonNull<OnDiskFormat>>();
        add<DelInternalSplitPromoteLeft<OnDiskFormat>>();
        add<DelInternalSplitPromoteRight<OnDiskFormat>>();

        add<LocateEmptyForward<OnDiskFormat>>();
        add<LocateEmptyReverse<OnDiskFormat>>();

        add<DuplicateKeys<OnDiskFormat>>();
    }
};

// Test suite for both V0 and V1
static unittest::SuiteInstance<BtreeLogicTestSuite<BtreeLayoutV0>> SUITE_V0("BTreeLogicTests_V0");

static unittest::SuiteInstance<BtreeLogicTestSuite<BtreeLayoutV1>> SUITE_V1("BTreeLogicTests_V1");
}
