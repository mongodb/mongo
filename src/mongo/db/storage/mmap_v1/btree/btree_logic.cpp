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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include <numeric>

#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mmap_v1/btree/btree_logic.h"
#include "mongo/db/storage/mmap_v1/btree/key.h"
#include "mongo/db/storage/mmap_v1/diskloc.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_base.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::unique_ptr;
using std::dec;
using std::endl;
using std::hex;
using std::make_pair;
using std::pair;
using std::string;
using std::stringstream;
using std::vector;

// BtreeLogic::Builder algorithm
//
// Phase 1:
//   Handled by caller. Extracts keys from raw documents and puts them in external sorter
//
// Phase 2 (the addKeys phase):
//   Add all keys to buckets. When a bucket gets full, pop the highest key (setting the
//   nextChild pointer of the bucket to the prevChild of the popped key), add the popped key to
//   a parent bucket, and create a new right sibling bucket to add the new key to. If the parent
//   bucket is full, this same operation is performed on the parent and all full ancestors. If
//   we get to the root and it is full, a new root is created above the current root. When
//   creating a new right sibling, it is set as its parent's nextChild as all keys in the right
//   sibling will be higher than all keys currently in the parent.

//
// Public Builder logic
//

template <class BtreeLayout>
typename BtreeLogic<BtreeLayout>::Builder* BtreeLogic<BtreeLayout>::newBuilder(
    OperationContext* txn, bool dupsAllowed) {
    return new Builder(this, txn, dupsAllowed);
}

template <class BtreeLayout>
BtreeLogic<BtreeLayout>::Builder::Builder(BtreeLogic* logic,
                                          OperationContext* txn,
                                          bool dupsAllowed)
    : _logic(logic), _dupsAllowed(dupsAllowed), _txn(txn) {
    // The normal bulk building path calls initAsEmpty, so we already have an empty root bucket.
    // This isn't the case in some unit tests that use the Builder directly rather than going
    // through an IndexAccessMethod.
    _rightLeafLoc = DiskLoc::fromRecordId(_logic->_headManager->getHead(txn));
    if (_rightLeafLoc.isNull()) {
        _rightLeafLoc = _logic->_addBucket(txn);
        _logic->_headManager->setHead(_txn, _rightLeafLoc.toRecordId());
    }

    // must be empty when starting
    invariant(_getBucket(_rightLeafLoc)->n == 0);
}

template <class BtreeLayout>
class BtreeLogic<BtreeLayout>::Builder::SetRightLeafLocChange : public RecoveryUnit::Change {
public:
    SetRightLeafLocChange(Builder* builder, DiskLoc oldLoc) : _builder(builder), _oldLoc(oldLoc) {}

    virtual void commit() {}
    virtual void rollback() {
        _builder->_rightLeafLoc = _oldLoc;
    }

    Builder* _builder;
    const DiskLoc _oldLoc;
};

template <class BtreeLayout>
Status BtreeLogic<BtreeLayout>::Builder::addKey(const BSONObj& keyObj, const DiskLoc& loc) {
    unique_ptr<KeyDataOwnedType> key(new KeyDataOwnedType(keyObj));

    if (key->dataSize() > BtreeLayout::KeyMax) {
        string msg = str::stream() << "Btree::insert: key too large to index, failing "
                                   << _logic->_indexName << ' ' << key->dataSize() << ' '
                                   << key->toString();
        log() << msg << endl;
        return Status(ErrorCodes::KeyTooLong, msg);
    }

    // If we have a previous key to compare to...
    if (_keyLast.get()) {
        int cmp = _keyLast->woCompare(*key, _logic->_ordering);

        // This shouldn't happen ever.  We expect keys in sorted order.
        if (cmp > 0) {
            return Status(ErrorCodes::InternalError, "Bad key order in btree builder");
        }

        // This could easily happen..
        if (!_dupsAllowed && (cmp == 0)) {
            return Status(ErrorCodes::DuplicateKey, _logic->dupKeyError(*_keyLast));
        }
    }

    BucketType* rightLeaf = _getModifiableBucket(_rightLeafLoc);
    if (!_logic->pushBack(rightLeaf, loc, *key, DiskLoc())) {
        // bucket was full, so split and try with the new node.
        _txn->recoveryUnit()->registerChange(new SetRightLeafLocChange(this, _rightLeafLoc));
        _rightLeafLoc = newBucket(rightLeaf, _rightLeafLoc);
        rightLeaf = _getModifiableBucket(_rightLeafLoc);
        invariant(_logic->pushBack(rightLeaf, loc, *key, DiskLoc()));
    }

    _keyLast = std::move(key);
    return Status::OK();
}

//
// Private Builder logic
//

template <class BtreeLayout>
DiskLoc BtreeLogic<BtreeLayout>::Builder::newBucket(BucketType* leftSib, DiskLoc leftSibLoc) {
    invariant(leftSib->n >= 2);  // Guaranteed by sufficiently small KeyMax.

    if (leftSib->parent.isNull()) {
        // Making a new root
        invariant(leftSibLoc.toRecordId() == _logic->_headManager->getHead(_txn));
        const DiskLoc newRootLoc = _logic->_addBucket(_txn);
        leftSib->parent = newRootLoc;
        _logic->_headManager->setHead(_txn, newRootLoc.toRecordId());

        // Set the newRoot's nextChild to point to leftSib for the invariant below.
        BucketType* newRoot = _getBucket(newRootLoc);
        *_txn->recoveryUnit()->writing(&newRoot->nextChild) = leftSibLoc;
    }

    DiskLoc parentLoc = leftSib->parent;
    BucketType* parent = _getModifiableBucket(parentLoc);

    // For the pushBack below to be correct, leftSib must be the right-most child of parent.
    invariant(parent->nextChild == leftSibLoc);

    // Pull right-most key out of leftSib and move to parent, splitting parent if necessary.
    // Note that popBack() handles setting leftSib's nextChild to the former prevChildNode of
    // the popped key.
    KeyDataType key;
    DiskLoc val;
    _logic->popBack(leftSib, &val, &key);
    if (!_logic->pushBack(parent, val, key, leftSibLoc)) {
        // parent is full, so split it.
        parentLoc = newBucket(parent, parentLoc);
        parent = _getModifiableBucket(parentLoc);
        invariant(_logic->pushBack(parent, val, key, leftSibLoc));
        leftSib->parent = parentLoc;
    }

    // Create a new bucket to the right of leftSib and set its parent pointer and the downward
    // nextChild pointer from the parent.
    DiskLoc newBucketLoc = _logic->_addBucket(_txn);
    BucketType* newBucket = _getBucket(newBucketLoc);
    *_txn->recoveryUnit()->writing(&newBucket->parent) = parentLoc;
    *_txn->recoveryUnit()->writing(&parent->nextChild) = newBucketLoc;
    return newBucketLoc;
}

template <class BtreeLayout>
typename BtreeLogic<BtreeLayout>::BucketType*
BtreeLogic<BtreeLayout>::Builder::_getModifiableBucket(DiskLoc loc) {
    return _logic->btreemod(_txn, _logic->getBucket(_txn, loc));
}

template <class BtreeLayout>
typename BtreeLogic<BtreeLayout>::BucketType* BtreeLogic<BtreeLayout>::Builder::_getBucket(
    DiskLoc loc) {
    return _logic->getBucket(_txn, loc);
}

//
// BtreeLogic logic
//

// static
template <class BtreeLayout>
typename BtreeLogic<BtreeLayout>::FullKey BtreeLogic<BtreeLayout>::getFullKey(
    const BucketType* bucket, int i) {
    if (i >= bucket->n) {
        int code = 13000;
        massert(code,
                (string) "invalid keyNode: " + BSON("i" << i << "n" << bucket->n).jsonString(),
                i < bucket->n);
    }
    return FullKey(bucket, i);
}

// static
template <class BtreeLayout>
typename BtreeLogic<BtreeLayout>::KeyHeaderType& BtreeLogic<BtreeLayout>::getKeyHeader(
    BucketType* bucket, int i) {
    return ((KeyHeaderType*)bucket->data)[i];
}

// static
template <class BtreeLayout>
const typename BtreeLogic<BtreeLayout>::KeyHeaderType& BtreeLogic<BtreeLayout>::getKeyHeader(
    const BucketType* bucket, int i) {
    return ((const KeyHeaderType*)bucket->data)[i];
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::markUnused(BucketType* bucket, int keyPos) {
    invariant(keyPos >= 0 && keyPos < bucket->n);
    getKeyHeader(bucket, keyPos).setUnused();
}

template <class BtreeLayout>
char* BtreeLogic<BtreeLayout>::dataAt(BucketType* bucket, short ofs) {
    return bucket->data + ofs;
}

template <class BtreeLayout>
typename BtreeLogic<BtreeLayout>::BucketType* BtreeLogic<BtreeLayout>::btreemod(
    OperationContext* txn, BucketType* bucket) {
    txn->recoveryUnit()->writingPtr(bucket, BtreeLayout::BucketSize);
    return bucket;
}

template <class BtreeLayout>
int BtreeLogic<BtreeLayout>::totalDataSize(BucketType* bucket) {
    return (int)(BtreeLayout::BucketSize - (bucket->data - (char*)bucket));
}

// We define this value as the maximum number of bytes such that, if we have
// fewer than this many bytes, we must be able to either merge with or receive
// keys from any neighboring node.  If our utilization goes below this value we
// know we can bring up the utilization with a simple operation.  Ignoring the
// 90/10 split policy which is sometimes employed and our 'unused' nodes, this
// is a lower bound on bucket utilization for non root buckets.
//
// Note that the exact value here depends on the implementation of
// _rebalancedSeparatorPos().  The conditions for lowWaterMark - 1 are as
// follows:  We know we cannot merge with the neighbor, so the total data size
// for us, the neighbor, and the separator must be at least
// BucketType::bodySize() + 1.  We must be able to accept one key of any
// allowed size, so our size plus storage for that additional key must be
// <= BucketType::bodySize() / 2.  This way, with the extra key we'll have a
// new bucket data size < half the total data size and by the implementation
// of _rebalancedSeparatorPos() the key must be added.
template <class BtreeLayout>
int BtreeLogic<BtreeLayout>::lowWaterMark() {
    return BtreeLayout::BucketBodySize / 2 - BtreeLayout::KeyMax - sizeof(KeyHeaderType) + 1;
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::init(BucketType* bucket) {
    BtreeLayout::initBucket(bucket);
    bucket->parent.Null();
    bucket->nextChild.Null();
    bucket->flags = Packed;
    bucket->n = 0;
    bucket->emptySize = totalDataSize(bucket);
    bucket->topSize = 0;
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::_unalloc(BucketType* bucket, int bytes) {
    bucket->topSize -= bytes;
    bucket->emptySize += bytes;
}

/**
 * We allocate space from the end of the buffer for data.  The keynodes grow from the front.
 */
template <class BtreeLayout>
int BtreeLogic<BtreeLayout>::_alloc(BucketType* bucket, int bytes) {
    invariant(bucket->emptySize >= bytes);
    bucket->topSize += bytes;
    bucket->emptySize -= bytes;
    int ofs = totalDataSize(bucket) - bucket->topSize;
    invariant(ofs > 0);
    return ofs;
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::setNotPacked(BucketType* bucket) {
    bucket->flags &= ~Packed;
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::setPacked(BucketType* bucket) {
    bucket->flags |= Packed;
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::_delKeyAtPos(BucketType* bucket, int keypos, bool mayEmpty) {
    invariant(keypos >= 0 && keypos <= bucket->n);
    invariant(childLocForPos(bucket, keypos).isNull());
    invariant((mayEmpty && bucket->n > 0) || bucket->n > 1 || bucket->nextChild.isNull());

    bucket->emptySize += sizeof(KeyHeaderType);
    bucket->n--;

    for (int j = keypos; j < bucket->n; j++) {
        getKeyHeader(bucket, j) = getKeyHeader(bucket, j + 1);
    }

    setNotPacked(bucket);
}

/**
 * Pull rightmost key from the bucket and set its prevChild pointer to be the nextChild for the
 * whole bucket. It is assumed that caller already has the old value of the nextChild
 * pointer and is about to add a pointer to it elsewhere in the tree.
 *
 * This is only used by BtreeLogic::Builder. Think very hard (and change this comment) before
 * using it anywhere else.
 *
 * WARNING: The keyDataOut that is filled out by this function points to newly unalloced memory
 * inside of this bucket. It only remains valid until the next write to this bucket.
 */
template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::popBack(BucketType* bucket,
                                      DiskLoc* recordLocOut,
                                      KeyDataType* keyDataOut) {
    massert(17435, "n==0 in btree popBack()", bucket->n > 0);

    invariant(getKeyHeader(bucket, bucket->n - 1).isUsed());

    FullKey kn = getFullKey(bucket, bucket->n - 1);
    *recordLocOut = kn.recordLoc;
    keyDataOut->assign(kn.data);
    int keysize = kn.data.dataSize();

    // The left/prev child of the node we are popping now goes in to the nextChild slot as all
    // of its keys are greater than all remaining keys in this node.
    bucket->nextChild = kn.prevChildBucket;
    bucket->n--;

    // This is risky because the keyDataOut we filled out above will now point to this newly
    // unalloced memory.
    bucket->emptySize += sizeof(KeyHeaderType);
    _unalloc(bucket, keysize);
}

/**
 * Add a key.  Must be > all existing.  Be careful to set next ptr right.
 */
template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::pushBack(BucketType* bucket,
                                       const DiskLoc recordLoc,
                                       const KeyDataType& key,
                                       const DiskLoc prevChild) {
    int bytesNeeded = key.dataSize() + sizeof(KeyHeaderType);
    if (bytesNeeded > bucket->emptySize) {
        return false;
    }
    invariant(bytesNeeded <= bucket->emptySize);

    if (bucket->n) {
        const FullKey klast = getFullKey(bucket, bucket->n - 1);
        if (klast.data.woCompare(key, _ordering) > 0) {
            log() << "btree bucket corrupt? "
                     "consider reindexing or running validate command"
                  << endl;
            log() << "  klast: " << klast.data.toString() << endl;
            log() << "  key:   " << key.toString() << endl;
            invariant(false);
        }
    }

    bucket->emptySize -= sizeof(KeyHeaderType);
    KeyHeaderType& kn = getKeyHeader(bucket, bucket->n++);
    kn.prevChildBucket = prevChild;
    kn.recordLoc = recordLoc;
    kn.setKeyDataOfs((short)_alloc(bucket, key.dataSize()));
    short ofs = kn.keyDataOfs();
    char* p = dataAt(bucket, ofs);
    memcpy(p, key.data(), key.dataSize());
    return true;
}

/**
 * Durability note:
 *
 * We do separate intent declarations herein.  Arguably one could just declare the whole bucket
 * given we do group commits.  This is something we could investigate later as to what is
 * faster.
 **/

/**
 * Insert a key in a bucket with no complexity -- no splits required
 * Returns false if a split is required.
 */
template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::basicInsert(OperationContext* txn,
                                          BucketType* bucket,
                                          const DiskLoc bucketLoc,
                                          int& keypos,
                                          const KeyDataType& key,
                                          const DiskLoc recordLoc) {
    invariant(bucket->n < 1024);
    invariant(keypos >= 0 && keypos <= bucket->n);

    int bytesNeeded = key.dataSize() + sizeof(KeyHeaderType);
    if (bytesNeeded > bucket->emptySize) {
        _pack(txn, bucket, bucketLoc, keypos);
        if (bytesNeeded > bucket->emptySize) {
            return false;
        }
    }

    invariant(getBucket(txn, bucketLoc) == bucket);

    {
        // declare that we will write to [k(keypos),k(n)]
        char* start = reinterpret_cast<char*>(&getKeyHeader(bucket, keypos));
        char* end = reinterpret_cast<char*>(&getKeyHeader(bucket, bucket->n + 1));

        // Declare that we will write to [k(keypos),k(n)]
        txn->recoveryUnit()->writingPtr(start, end - start);
    }

    // e.g. for n==3, keypos==2
    // 1 4 9 -> 1 4 _ 9
    for (int j = bucket->n; j > keypos; j--) {
        getKeyHeader(bucket, j) = getKeyHeader(bucket, j - 1);
    }

    size_t writeLen = sizeof(bucket->emptySize) + sizeof(bucket->topSize) + sizeof(bucket->n);
    txn->recoveryUnit()->writingPtr(&bucket->emptySize, writeLen);
    bucket->emptySize -= sizeof(KeyHeaderType);
    bucket->n++;

    // This _KeyNode was marked for writing above.
    KeyHeaderType& kn = getKeyHeader(bucket, keypos);
    kn.prevChildBucket.Null();
    kn.recordLoc = recordLoc;
    kn.setKeyDataOfs((short)_alloc(bucket, key.dataSize()));
    char* p = dataAt(bucket, kn.keyDataOfs());
    txn->recoveryUnit()->writingPtr(p, key.dataSize());
    memcpy(p, key.data(), key.dataSize());
    return true;
}

/**
 * With this implementation, refPos == 0 disregards effect of refPos.  index > 0 prevents
 * creation of an empty bucket.
 */
template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::mayDropKey(BucketType* bucket, int index, int refPos) {
    return index > 0 && (index != refPos) && getKeyHeader(bucket, index).isUnused() &&
        getKeyHeader(bucket, index).prevChildBucket.isNull();
}

template <class BtreeLayout>
int BtreeLogic<BtreeLayout>::_packedDataSize(BucketType* bucket, int refPos) {
    if (bucket->flags & Packed) {
        return BtreeLayout::BucketSize - bucket->emptySize - BucketType::HeaderSize;
    }

    int size = 0;
    for (int j = 0; j < bucket->n; ++j) {
        if (mayDropKey(bucket, j, refPos)) {
            continue;
        }
        size += getFullKey(bucket, j).data.dataSize() + sizeof(KeyHeaderType);
    }

    return size;
}

/**
 * When we delete things, we just leave empty space until the node is full and then we repack
 * it.
 */
template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::_pack(OperationContext* txn,
                                    BucketType* bucket,
                                    const DiskLoc thisLoc,
                                    int& refPos) {
    invariant(getBucket(txn, thisLoc) == bucket);

    if (bucket->flags & Packed) {
        return;
    }

    _packReadyForMod(btreemod(txn, bucket), refPos);
}

/**
 * Version when write intent already declared.
 */
template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::_packReadyForMod(BucketType* bucket, int& refPos) {
    if (bucket->flags & Packed) {
        return;
    }

    int tdz = totalDataSize(bucket);
    char temp[BtreeLayout::BucketSize];
    int ofs = tdz;
    bucket->topSize = 0;

    int i = 0;
    for (int j = 0; j < bucket->n; j++) {
        if (mayDropKey(bucket, j, refPos)) {
            // key is unused and has no children - drop it
            continue;
        }

        if (i != j) {
            if (refPos == j) {
                // i < j so j will never be refPos again
                refPos = i;
            }
            getKeyHeader(bucket, i) = getKeyHeader(bucket, j);
        }

        short ofsold = getKeyHeader(bucket, i).keyDataOfs();
        int sz = getFullKey(bucket, i).data.dataSize();
        ofs -= sz;
        bucket->topSize += sz;
        memcpy(temp + ofs, dataAt(bucket, ofsold), sz);
        getKeyHeader(bucket, i).setKeyDataOfsSavingUse(ofs);
        ++i;
    }

    if (refPos == bucket->n) {
        refPos = i;
    }

    bucket->n = i;
    int dataUsed = tdz - ofs;
    memcpy(bucket->data + ofs, temp + ofs, dataUsed);

    bucket->emptySize = tdz - dataUsed - bucket->n * sizeof(KeyHeaderType);
    int foo = bucket->emptySize;
    invariant(foo >= 0);
    setPacked(bucket);
    assertValid(_indexName, bucket, _ordering);
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::truncateTo(BucketType* bucket, int N, int& refPos) {
    bucket->n = N;
    setNotPacked(bucket);
    _packReadyForMod(bucket, refPos);
}

/**
 * In the standard btree algorithm, we would split based on the
 * existing keys _and_ the new key.  But that's more work to
 * implement, so we split the existing keys and then add the new key.
 *
 * There are several published heuristic algorithms for doing splits, but basically what you
 * want are (1) even balancing between the two sides and (2) a small split key so the parent can
 * have a larger branching factor.
 *
 * We just have a simple algorithm right now: if a key includes the halfway point (or 10% way
 * point) in terms of bytes, split on that key; otherwise split on the key immediately to the
 * left of the halfway point (or 10% point).
 *
 * This function is expected to be called on a packed bucket.
 */
template <class BtreeLayout>
int BtreeLogic<BtreeLayout>::splitPos(BucketType* bucket, int keypos) {
    invariant(bucket->n > 2);
    int split = 0;
    int rightSize = 0;

    // When splitting a btree node, if the new key is greater than all the other keys, we should
    // not do an even split, but a 90/10 split.  see SERVER-983.  TODO I think we only want to
    // do the 90% split on the rhs node of the tree.
    int rightSizeLimit =
        (bucket->topSize + sizeof(KeyHeaderType) * bucket->n) / (keypos == bucket->n ? 10 : 2);

    for (int i = bucket->n - 1; i > -1; --i) {
        rightSize += getFullKey(bucket, i).data.dataSize() + sizeof(KeyHeaderType);
        if (rightSize > rightSizeLimit) {
            split = i;
            break;
        }
    }

    // safeguards - we must not create an empty bucket
    if (split < 1) {
        split = 1;
    } else if (split > bucket->n - 2) {
        split = bucket->n - 2;
    }

    return split;
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::reserveKeysFront(BucketType* bucket, int nAdd) {
    invariant(bucket->emptySize >= int(sizeof(KeyHeaderType) * nAdd));
    bucket->emptySize -= sizeof(KeyHeaderType) * nAdd;
    for (int i = bucket->n - 1; i > -1; --i) {
        getKeyHeader(bucket, i + nAdd) = getKeyHeader(bucket, i);
    }
    bucket->n += nAdd;
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::setKey(BucketType* bucket,
                                     int i,
                                     const DiskLoc recordLoc,
                                     const KeyDataType& key,
                                     const DiskLoc prevChildBucket) {
    KeyHeaderType& kn = getKeyHeader(bucket, i);
    kn.recordLoc = recordLoc;
    kn.prevChildBucket = prevChildBucket;
    short ofs = (short)_alloc(bucket, key.dataSize());
    kn.setKeyDataOfs(ofs);
    char* p = dataAt(bucket, ofs);
    memcpy(p, key.data(), key.dataSize());
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::dropFront(BucketType* bucket, int nDrop, int& refpos) {
    for (int i = nDrop; i < bucket->n; ++i) {
        getKeyHeader(bucket, i - nDrop) = getKeyHeader(bucket, i);
    }
    bucket->n -= nDrop;
    setNotPacked(bucket);
    _packReadyForMod(bucket, refpos);
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::customLocate(OperationContext* txn,
                                           DiskLoc* locInOut,
                                           int* keyOfsInOut,
                                           const IndexSeekPoint& seekPoint,
                                           int direction) const {
    pair<DiskLoc, int> unused;

    customLocate(txn, locInOut, keyOfsInOut, seekPoint, direction, unused);
    skipUnusedKeys(txn, locInOut, keyOfsInOut, direction);
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::advance(OperationContext* txn,
                                      DiskLoc* bucketLocInOut,
                                      int* posInOut,
                                      int direction) const {
    *bucketLocInOut = advance(txn, *bucketLocInOut, posInOut, direction);
    skipUnusedKeys(txn, bucketLocInOut, posInOut, direction);
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::skipUnusedKeys(OperationContext* txn,
                                             DiskLoc* loc,
                                             int* pos,
                                             int direction) const {
    while (!loc->isNull() && !keyIsUsed(txn, *loc, *pos)) {
        *loc = advance(txn, *loc, pos, direction);
    }
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::advanceTo(OperationContext* txn,
                                        DiskLoc* thisLocInOut,
                                        int* keyOfsInOut,
                                        const IndexSeekPoint& seekPoint,
                                        int direction) const {
    advanceToImpl(txn, thisLocInOut, keyOfsInOut, seekPoint, direction);
    skipUnusedKeys(txn, thisLocInOut, keyOfsInOut, direction);
}

/**
 * find smallest/biggest value greater-equal/less-equal than specified
 *
 * starting thisLoc + keyOfs will be strictly less than/strictly greater than
 * keyBegin/keyBeginLen/keyEnd
 *
 * All the direction checks below allowed me to refactor the code, but possibly separate forward
 * and reverse implementations would be more efficient
 */
template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::advanceToImpl(OperationContext* txn,
                                            DiskLoc* thisLocInOut,
                                            int* keyOfsInOut,
                                            const IndexSeekPoint& seekPoint,
                                            int direction) const {
    BucketType* bucket = getBucket(txn, *thisLocInOut);

    int l, h;
    bool dontGoUp;

    if (direction > 0) {
        l = *keyOfsInOut;
        h = bucket->n - 1;
        int cmpResult = customBSONCmp(getFullKey(bucket, h).data.toBson(), seekPoint, direction);
        dontGoUp = (cmpResult >= 0);
    } else {
        l = 0;
        h = *keyOfsInOut;
        int cmpResult = customBSONCmp(getFullKey(bucket, l).data.toBson(), seekPoint, direction);
        dontGoUp = (cmpResult <= 0);
    }

    pair<DiskLoc, int> bestParent;

    if (dontGoUp) {
        // this comparison result assures h > l
        if (!customFind(txn, l, h, seekPoint, direction, thisLocInOut, keyOfsInOut, bestParent)) {
            return;
        }
    } else {
        // go up parents until rightmost/leftmost node is >=/<= target or at top
        while (!bucket->parent.isNull()) {
            *thisLocInOut = bucket->parent;
            bucket = getBucket(txn, *thisLocInOut);

            if (direction > 0) {
                if (customBSONCmp(getFullKey(bucket, bucket->n - 1).data.toBson(),
                                  seekPoint,
                                  direction) >= 0) {
                    break;
                }
            } else {
                if (customBSONCmp(getFullKey(bucket, 0).data.toBson(), seekPoint, direction) <= 0) {
                    break;
                }
            }
        }
    }

    customLocate(txn, thisLocInOut, keyOfsInOut, seekPoint, direction, bestParent);
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::customLocate(OperationContext* txn,
                                           DiskLoc* locInOut,
                                           int* keyOfsInOut,
                                           const IndexSeekPoint& seekPoint,
                                           int direction,
                                           pair<DiskLoc, int>& bestParent) const {
    BucketType* bucket = getBucket(txn, *locInOut);

    if (0 == bucket->n) {
        *locInOut = DiskLoc();
        return;
    }

    // go down until find smallest/biggest >=/<= target
    for (;;) {
        int l = 0;
        int h = bucket->n - 1;

        // +direction: 0, -direction: h
        int z = (direction > 0) ? 0 : h;

        // leftmost/rightmost key may possibly be >=/<= search key
        int res = customBSONCmp(getFullKey(bucket, z).data.toBson(), seekPoint, direction);
        if (direction * res >= 0) {
            DiskLoc next;
            *keyOfsInOut = z;

            if (direction > 0) {
                dassert(z == 0);
                next = getKeyHeader(bucket, 0).prevChildBucket;
            } else {
                next = bucket->nextChild;
            }

            if (!next.isNull()) {
                bestParent = pair<DiskLoc, int>(*locInOut, *keyOfsInOut);
                *locInOut = next;
                bucket = getBucket(txn, *locInOut);
                continue;
            } else {
                return;
            }
        }

        res = customBSONCmp(getFullKey(bucket, h - z).data.toBson(), seekPoint, direction);
        if (direction * res < 0) {
            DiskLoc next;
            if (direction > 0) {
                next = bucket->nextChild;
            } else {
                next = getKeyHeader(bucket, 0).prevChildBucket;
            }

            if (next.isNull()) {
                // if bestParent is null, we've hit the end and locInOut gets set to DiskLoc()
                *locInOut = bestParent.first;
                *keyOfsInOut = bestParent.second;
                return;
            } else {
                *locInOut = next;
                bucket = getBucket(txn, *locInOut);
                continue;
            }
        }

        if (!customFind(txn, l, h, seekPoint, direction, locInOut, keyOfsInOut, bestParent)) {
            return;
        }

        bucket = getBucket(txn, *locInOut);
    }
}

template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::customFind(OperationContext* txn,
                                         int low,
                                         int high,
                                         const IndexSeekPoint& seekPoint,
                                         int direction,
                                         DiskLoc* thisLocInOut,
                                         int* keyOfsInOut,
                                         pair<DiskLoc, int>& bestParent) const {
    const BucketType* bucket = getBucket(txn, *thisLocInOut);

    for (;;) {
        if (low + 1 == high) {
            *keyOfsInOut = (direction > 0) ? high : low;
            DiskLoc next = getKeyHeader(bucket, high).prevChildBucket;
            if (!next.isNull()) {
                bestParent = make_pair(*thisLocInOut, *keyOfsInOut);
                *thisLocInOut = next;
                return true;
            } else {
                return false;
            }
        }

        int middle = low + (high - low) / 2;

        int cmp = customBSONCmp(getFullKey(bucket, middle).data.toBson(), seekPoint, direction);
        if (cmp < 0) {
            low = middle;
        } else if (cmp > 0) {
            high = middle;
        } else {
            if (direction < 0) {
                low = middle;
            } else {
                high = middle;
            }
        }
    }
}

/**
 * NOTE: Currently the Ordering implementation assumes a compound index will not have more keys
 * than an unsigned variable has bits.  The same assumption is used in the implementation below
 * with respect to the 'mask' variable.
 *
 * 'l' is a regular bsonobj
 *
 * 'rBegin' is composed partly of an existing bsonobj, and the remaining keys are taken from a
 * vector of elements that frequently changes
 *
 * see https://jira.mongodb.org/browse/SERVER-371
 */
// static
template <class BtreeLayout>
int BtreeLogic<BtreeLayout>::customBSONCmp(const BSONObj& left,
                                           const IndexSeekPoint& right,
                                           int direction) const {
    // XXX: make this readable
    dassert(right.keySuffix.size() == right.suffixInclusive.size());

    BSONObjIterator ll(left);
    BSONObjIterator rr(right.keyPrefix);
    unsigned mask = 1;
    size_t i = 0;
    for (; i < size_t(right.prefixLen); ++i, mask <<= 1) {
        BSONElement lll = ll.next();
        BSONElement rrr = rr.next();

        int x = lll.woCompare(rrr, false);
        if (_ordering.descending(mask))
            x = -x;
        if (x != 0)
            return x;
    }
    if (right.prefixExclusive) {
        return -direction;
    }
    for (; i < right.keySuffix.size(); ++i, mask <<= 1) {
        if (!ll.more())
            return -direction;

        BSONElement lll = ll.next();
        BSONElement rrr = *right.keySuffix[i];
        int x = lll.woCompare(rrr, false);
        if (_ordering.descending(mask))
            x = -x;
        if (x != 0)
            return x;
        if (!right.suffixInclusive[i]) {
            return -direction;
        }
    }
    return ll.more() ? direction : 0;
}

template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::exists(OperationContext* txn, const KeyDataType& key) const {
    int position = 0;

    // Find the DiskLoc
    bool found;

    DiskLoc bucket = _locate(txn, getRootLoc(txn), key, &position, &found, DiskLoc::min(), 1);

    while (!bucket.isNull()) {
        FullKey fullKey = getFullKey(getBucket(txn, bucket), position);
        if (fullKey.header.isUsed()) {
            return fullKey.data.woEqual(key);
        }
        bucket = advance(txn, bucket, &position, 1);
    }

    return false;
}

template <class BtreeLayout>
Status BtreeLogic<BtreeLayout>::dupKeyCheck(OperationContext* txn,
                                            const BSONObj& key,
                                            const DiskLoc& loc) const {
    KeyDataOwnedType theKey(key);
    if (!wouldCreateDup(txn, theKey, loc)) {
        return Status::OK();
    }

    return Status(ErrorCodes::DuplicateKey, dupKeyError(theKey));
}

template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::wouldCreateDup(OperationContext* txn,
                                             const KeyDataType& key,
                                             const DiskLoc self) const {
    int position;
    bool found;

    DiskLoc posLoc = _locate(txn, getRootLoc(txn), key, &position, &found, DiskLoc::min(), 1);

    while (!posLoc.isNull()) {
        FullKey fullKey = getFullKey(getBucket(txn, posLoc), position);
        if (fullKey.header.isUsed()) {
            // TODO: we may not need fullKey.data until we know fullKey.header.isUsed() here
            // and elsewhere.
            if (fullKey.data.woEqual(key)) {
                return fullKey.recordLoc != self;
            }
            break;
        }

        posLoc = advance(txn, posLoc, &position, 1);
    }
    return false;
}

template <class BtreeLayout>
string BtreeLogic<BtreeLayout>::dupKeyError(const KeyDataType& key) const {
    stringstream ss;
    ss << "E11000 duplicate key error ";
    ss << "index: " << _indexName << " ";
    ss << "dup key: " << key.toString();
    return ss.str();
}

/**
 * Find a key within this btree bucket.
 *
 * When duplicate keys are allowed, we use the DiskLoc of the record as if it were part of the
 * key.  That assures that even when there are many duplicates (e.g., 1 million) for a key, our
 * performance is still good.
 *
 * assertIfDup: if the key exists (ignoring the recordLoc), uassert
 *
 * pos: for existing keys k0...kn-1.
 * returns # it goes BEFORE.  so key[pos-1] < key < key[pos]
 * returns n if it goes after the last existing key.
 * note result might be an Unused location!
 */
template <class BtreeLayout>
Status BtreeLogic<BtreeLayout>::_find(OperationContext* txn,
                                      BucketType* bucket,
                                      const KeyDataType& key,
                                      const DiskLoc& recordLoc,
                                      bool errorIfDup,
                                      int* keyPositionOut,
                                      bool* foundOut) const {
    // XXX: fix the ctor for DiskLoc56bit so we can just convert w/o assignment operator
    LocType genericRecordLoc;
    genericRecordLoc = recordLoc;

    bool dupsCheckedYet = false;

    int low = 0;
    int high = bucket->n - 1;
    int middle = (low + high) / 2;

    while (low <= high) {
        FullKey fullKey = getFullKey(bucket, middle);
        int cmp = key.woCompare(fullKey.data, _ordering);

        // The key data is the same.
        if (0 == cmp) {
            // Found the key in this bucket.  If we're checking for dups...
            if (errorIfDup) {
                if (fullKey.header.isUnused()) {
                    // It's ok that the key is there if it is unused.  We need to check that
                    // there aren't other entries for the key then.  as it is very rare that
                    // we get here, we don't put any coding effort in here to make this
                    // particularly fast
                    if (!dupsCheckedYet) {
                        // This is expensive and we only want to do it once(? -- when would
                        // it happen twice).
                        dupsCheckedYet = true;
                        if (exists(txn, key)) {
                            if (wouldCreateDup(txn, key, genericRecordLoc)) {
                                return Status(ErrorCodes::DuplicateKey, dupKeyError(key), 11000);
                            } else {
                                return Status(ErrorCodes::DuplicateKeyValue,
                                              "key/value already in index");
                            }
                        }
                    }
                } else {
                    if (fullKey.recordLoc == recordLoc) {
                        return Status(ErrorCodes::DuplicateKeyValue, "key/value already in index");
                    } else {
                        return Status(ErrorCodes::DuplicateKey, dupKeyError(key), 11000);
                    }
                }
            }

            // If we're here dup keys are allowed, or the key is a dup but unused.
            LocType recordLocCopy = fullKey.recordLoc;

            // We clear this bit so we can test equality without the used bit messing us up.
            // XXX: document this
            // XXX: kill this GETOFS stuff
            recordLocCopy.GETOFS() &= ~1;

            // Set 'cmp' to the comparison w/the DiskLoc and fall through below.
            cmp = recordLoc.compare(recordLocCopy);
        }

        if (cmp < 0) {
            high = middle - 1;
        } else if (cmp > 0) {
            low = middle + 1;
        } else {
            // Found it!
            *keyPositionOut = middle;
            *foundOut = true;
            return Status::OK();
        }

        middle = (low + high) / 2;
    }

    // Not found.
    *keyPositionOut = low;

    // Some debugging checks.
    if (low != bucket->n) {
        wassert(key.woCompare(getFullKey(bucket, low).data, _ordering) <= 0);

        if (low > 0) {
            if (getFullKey(bucket, low - 1).data.woCompare(key, _ordering) > 0) {
                DEV {
                    log() << key.toString() << endl;
                    log() << getFullKey(bucket, low - 1).data.toString() << endl;
                }
                wassert(false);
            }
        }
    }

    *foundOut = false;
    return Status::OK();
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::delBucket(OperationContext* txn,
                                        BucketType* bucket,
                                        const DiskLoc bucketLoc) {
    invariant(bucketLoc != getRootLoc(txn));

    _cursorRegistry->invalidateCursorsForBucket(bucketLoc);

    BucketType* p = getBucket(txn, bucket->parent);
    int parentIdx = indexInParent(txn, bucket, bucketLoc);
    *txn->recoveryUnit()->writing(&childLocForPos(p, parentIdx)) = DiskLoc();
    deallocBucket(txn, bucket, bucketLoc);
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::deallocBucket(OperationContext* txn,
                                            BucketType* bucket,
                                            const DiskLoc bucketLoc) {
    bucket->n = BtreeLayout::INVALID_N_SENTINEL;
    bucket->parent.Null();
    _recordStore->deleteRecord(txn, bucketLoc.toRecordId());
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::restorePosition(OperationContext* txn,
                                              const BSONObj& savedKey,
                                              const DiskLoc& savedLoc,
                                              int direction,
                                              DiskLoc* bucketLocInOut,
                                              int* keyOffsetInOut) const {
    // The caller has to ensure validity of the saved cursor using the SavedCursorRegistry
    BucketType* bucket = getBucket(txn, *bucketLocInOut);
    invariant(bucket);
    invariant(BtreeLayout::INVALID_N_SENTINEL != bucket->n);

    if (_keyIsAt(savedKey, savedLoc, bucket, *keyOffsetInOut)) {
        skipUnusedKeys(txn, bucketLocInOut, keyOffsetInOut, direction);
        return;
    }

    if (*keyOffsetInOut > 0) {
        (*keyOffsetInOut)--;
        if (_keyIsAt(savedKey, savedLoc, bucket, *keyOffsetInOut)) {
            skipUnusedKeys(txn, bucketLocInOut, keyOffsetInOut, direction);
            return;
        }
    }

    locate(txn, savedKey, savedLoc, direction, keyOffsetInOut, bucketLocInOut);
}

template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::_keyIsAt(const BSONObj& savedKey,
                                       const DiskLoc& savedLoc,
                                       BucketType* bucket,
                                       int keyPos) const {
    if (keyPos >= bucket->n) {
        return false;
    }

    FullKey key = getFullKey(bucket, keyPos);
    if (!key.data.toBson().binaryEqual(savedKey)) {
        return false;
    }
    return key.header.recordLoc == savedLoc;
}

/**
 * May delete the bucket 'bucket' rendering 'bucketLoc' invalid.
 */
template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::delKeyAtPos(OperationContext* txn,
                                          BucketType* bucket,
                                          const DiskLoc bucketLoc,
                                          int p) {
    invariant(bucket->n > 0);
    DiskLoc left = childLocForPos(bucket, p);
    if (bucket->n == 1) {
        if (left.isNull() && bucket->nextChild.isNull()) {
            _delKeyAtPos(bucket, p);
            if (isHead(bucket)) {
                // we don't delete the top bucket ever
            } else {
                if (!mayBalanceWithNeighbors(txn, bucket, bucketLoc)) {
                    // An empty bucket is only allowed as a txnient state.  If
                    // there are no neighbors to balance with, we delete ourself.
                    // This condition is only expected in legacy btrees.
                    delBucket(txn, bucket, bucketLoc);
                }
            }
            return;
        }
        deleteInternalKey(txn, bucket, bucketLoc, p);
        return;
    }

    if (left.isNull()) {
        _delKeyAtPos(bucket, p);
        mayBalanceWithNeighbors(txn, bucket, bucketLoc);
    } else {
        deleteInternalKey(txn, bucket, bucketLoc, p);
    }
}

/**
 * This function replaces the specified key (k) by either the prev or next key in the btree
 * (k').  We require that k have either a left or right child.  If k has a left child, we set k'
 * to the prev key of k, which must be a leaf present in the left child.  If k does not have a
 * left child, we set k' to the next key of k, which must be a leaf present in the right child.
 * When we replace k with k', we copy k' over k (which may cause a split) and then remove k'
 * from its original location.  Because k' is stored in a descendent of k, replacing k by k'
 * will not modify the storage location of the original k', and we can easily remove k' from its
 * original location.
 *
 * This function is only needed in cases where k has a left or right child; in other cases a
 * simpler key removal implementation is possible.
 *
 * NOTE on noncompliant BtreeBuilder btrees: It is possible (though likely rare) for btrees
 * created by BtreeBuilder to have k' that is not a leaf, see SERVER-2732.  These cases are
 * handled in the same manner as described in the "legacy btree structures" note below.
 *
 * NOTE on legacy btree structures: In legacy btrees, k' can be a nonleaf.  In such a case we
 * 'delete' k by marking it as an unused node rather than replacing it with k'.  Also, k' may be
 * a leaf but marked as an unused node.  In such a case we replace k by k', preserving the key's
 * unused marking.  This function is only expected to mark a key as unused when handling a
 * legacy btree.
 */
template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::deleteInternalKey(OperationContext* txn,
                                                BucketType* bucket,
                                                const DiskLoc bucketLoc,
                                                int keypos) {
    DiskLoc lchild = childLocForPos(bucket, keypos);
    DiskLoc rchild = childLocForPos(bucket, keypos + 1);
    invariant(!lchild.isNull() || !rchild.isNull());
    int advanceDirection = lchild.isNull() ? 1 : -1;
    int advanceKeyOfs = keypos;
    DiskLoc advanceLoc = advance(txn, bucketLoc, &advanceKeyOfs, advanceDirection);
    // advanceLoc must be a descentant of thisLoc, because thisLoc has a
    // child in the proper direction and all descendants of thisLoc must be
    // nonempty because they are not the root.
    BucketType* advanceBucket = getBucket(txn, advanceLoc);

    if (!childLocForPos(advanceBucket, advanceKeyOfs).isNull() ||
        !childLocForPos(advanceBucket, advanceKeyOfs + 1).isNull()) {
        markUnused(bucket, keypos);
        return;
    }

    FullKey kn = getFullKey(advanceBucket, advanceKeyOfs);
    // Because advanceLoc is a descendant of thisLoc, updating thisLoc will
    // not affect packing or keys of advanceLoc and kn will be stable
    // during the following setInternalKey()
    setInternalKey(txn,
                   bucket,
                   bucketLoc,
                   keypos,
                   kn.recordLoc,
                   kn.data,
                   childLocForPos(bucket, keypos),
                   childLocForPos(bucket, keypos + 1));
    delKeyAtPos(txn, btreemod(txn, advanceBucket), advanceLoc, advanceKeyOfs);
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::replaceWithNextChild(OperationContext* txn,
                                                   BucketType* bucket,
                                                   const DiskLoc bucketLoc) {
    invariant(bucket->n == 0 && !bucket->nextChild.isNull());
    if (bucket->parent.isNull()) {
        invariant(getRootLoc(txn) == bucketLoc);
        _headManager->setHead(txn, bucket->nextChild.toRecordId());
    } else {
        BucketType* parentBucket = getBucket(txn, bucket->parent);
        int bucketIndexInParent = indexInParent(txn, bucket, bucketLoc);
        *txn->recoveryUnit()->writing(&childLocForPos(parentBucket, bucketIndexInParent)) =
            bucket->nextChild;
    }

    *txn->recoveryUnit()->writing(&getBucket(txn, bucket->nextChild)->parent) = bucket->parent;
    _cursorRegistry->invalidateCursorsForBucket(bucketLoc);
    deallocBucket(txn, bucket, bucketLoc);
}

template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::canMergeChildren(OperationContext* txn,
                                               BucketType* bucket,
                                               const DiskLoc bucketLoc,
                                               const int leftIndex) {
    invariant(leftIndex >= 0 && leftIndex < bucket->n);

    DiskLoc leftNodeLoc = childLocForPos(bucket, leftIndex);
    DiskLoc rightNodeLoc = childLocForPos(bucket, leftIndex + 1);

    if (leftNodeLoc.isNull() || rightNodeLoc.isNull()) {
        return false;
    }

    int pos = 0;

    BucketType* leftBucket = getBucket(txn, leftNodeLoc);
    BucketType* rightBucket = getBucket(txn, rightNodeLoc);

    int sum = BucketType::HeaderSize + _packedDataSize(leftBucket, pos) +
        _packedDataSize(rightBucket, pos) + getFullKey(bucket, leftIndex).data.dataSize() +
        sizeof(KeyHeaderType);

    return sum <= BtreeLayout::BucketSize;
}

/**
 * This implementation must respect the meaning and value of lowWaterMark.  Also see comments in
 * splitPos().
 */
template <class BtreeLayout>
int BtreeLogic<BtreeLayout>::_rebalancedSeparatorPos(OperationContext* txn,
                                                     BucketType* bucket,
                                                     int leftIndex) {
    int split = -1;
    int rightSize = 0;

    const BucketType* l = childForPos(txn, bucket, leftIndex);
    const BucketType* r = childForPos(txn, bucket, leftIndex + 1);

    int KNS = sizeof(KeyHeaderType);
    int rightSizeLimit = (l->topSize + l->n * KNS + getFullKey(bucket, leftIndex).data.dataSize() +
                          KNS + r->topSize + r->n * KNS) /
        2;

    // This constraint should be ensured by only calling this function
    // if we go below the low water mark.
    invariant(rightSizeLimit < BtreeLayout::BucketBodySize);

    for (int i = r->n - 1; i > -1; --i) {
        rightSize += getFullKey(r, i).data.dataSize() + KNS;
        if (rightSize > rightSizeLimit) {
            split = l->n + 1 + i;
            break;
        }
    }

    if (split == -1) {
        rightSize += getFullKey(bucket, leftIndex).data.dataSize() + KNS;
        if (rightSize > rightSizeLimit) {
            split = l->n;
        }
    }

    if (split == -1) {
        for (int i = l->n - 1; i > -1; --i) {
            rightSize += getFullKey(l, i).data.dataSize() + KNS;
            if (rightSize > rightSizeLimit) {
                split = i;
                break;
            }
        }
    }

    // safeguards - we must not create an empty bucket
    if (split < 1) {
        split = 1;
    } else if (split > l->n + 1 + r->n - 2) {
        split = l->n + 1 + r->n - 2;
    }

    return split;
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::doMergeChildren(OperationContext* txn,
                                              BucketType* bucket,
                                              const DiskLoc bucketLoc,
                                              int leftIndex) {
    DiskLoc leftNodeLoc = childLocForPos(bucket, leftIndex);
    DiskLoc rightNodeLoc = childLocForPos(bucket, leftIndex + 1);

    BucketType* l = btreemod(txn, getBucket(txn, leftNodeLoc));
    BucketType* r = btreemod(txn, getBucket(txn, rightNodeLoc));

    int pos = 0;
    _packReadyForMod(l, pos);
    _packReadyForMod(r, pos);

    // We know the additional keys below will fit in l because canMergeChildren() must be true.
    int oldLNum = l->n;
    // left child's right child becomes old parent key's left child
    FullKey knLeft = getFullKey(bucket, leftIndex);
    invariant(pushBack(l, knLeft.recordLoc, knLeft.data, l->nextChild));

    for (int i = 0; i < r->n; ++i) {
        FullKey kn = getFullKey(r, i);
        invariant(pushBack(l, kn.recordLoc, kn.data, kn.prevChildBucket));
    }

    l->nextChild = r->nextChild;
    fixParentPtrs(txn, l, leftNodeLoc, oldLNum);
    delBucket(txn, r, rightNodeLoc);

    childLocForPos(bucket, leftIndex + 1) = leftNodeLoc;
    childLocForPos(bucket, leftIndex) = DiskLoc();
    _delKeyAtPos(bucket, leftIndex, true);

    if (bucket->n == 0) {
        // Will trash bucket and bucketLoc.
        //
        // TODO To ensure all leaves are of equal height, we should ensure this is only called
        // on the root.
        replaceWithNextChild(txn, bucket, bucketLoc);
    } else {
        mayBalanceWithNeighbors(txn, bucket, bucketLoc);
    }
}

template <class BtreeLayout>
int BtreeLogic<BtreeLayout>::indexInParent(OperationContext* txn,
                                           BucketType* bucket,
                                           const DiskLoc bucketLoc) const {
    invariant(!bucket->parent.isNull());
    const BucketType* p = getBucket(txn, bucket->parent);
    if (p->nextChild == bucketLoc) {
        return p->n;
    }

    for (int i = 0; i < p->n; ++i) {
        if (getKeyHeader(p, i).prevChildBucket == bucketLoc) {
            return i;
        }
    }

    log() << "ERROR: can't find ref to child bucket.\n";
    log() << "child: " << bucketLoc << "\n";
    // dump();
    log() << "Parent: " << bucket->parent << "\n";
    // p->dump();
    invariant(false);
    return -1;  // just to compile
}

template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::tryBalanceChildren(OperationContext* txn,
                                                 BucketType* bucket,
                                                 const DiskLoc bucketLoc,
                                                 int leftIndex) {
    // If we can merge, then we must merge rather than balance to preserve bucket utilization
    // constraints.
    if (canMergeChildren(txn, bucket, bucketLoc, leftIndex)) {
        return false;
    }

    doBalanceChildren(txn, btreemod(txn, bucket), bucketLoc, leftIndex);
    return true;
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::doBalanceLeftToRight(OperationContext* txn,
                                                   BucketType* bucket,
                                                   const DiskLoc bucketLoc,
                                                   int leftIndex,
                                                   int split,
                                                   BucketType* l,
                                                   const DiskLoc lchild,
                                                   BucketType* r,
                                                   const DiskLoc rchild) {
    // TODO maybe do some audits the same way pushBack() does?  As a precondition, rchild + the
    // old separator are <= half a body size, and lchild is at most completely full.  Based on
    // the value of split, rchild will get <= half of the total bytes which is at most 75% of a
    // full body.  So rchild will have room for the following keys:
    int rAdd = l->n - split;
    reserveKeysFront(r, rAdd);

    for (int i = split + 1, j = 0; i < l->n; ++i, ++j) {
        FullKey kn = getFullKey(l, i);
        setKey(r, j, kn.recordLoc, kn.data, kn.prevChildBucket);
    }

    FullKey leftIndexKN = getFullKey(bucket, leftIndex);
    setKey(r, rAdd - 1, leftIndexKN.recordLoc, leftIndexKN.data, l->nextChild);

    fixParentPtrs(txn, r, rchild, 0, rAdd - 1);

    FullKey kn = getFullKey(l, split);
    l->nextChild = kn.prevChildBucket;

    // Because lchild is a descendant of thisLoc, updating thisLoc will not affect packing or
    // keys of lchild and kn will be stable during the following setInternalKey()
    setInternalKey(txn, bucket, bucketLoc, leftIndex, kn.recordLoc, kn.data, lchild, rchild);

    // lchild and rchild cannot be merged, so there must be >0 (actually more) keys to the left
    // of split.
    int zeropos = 0;
    truncateTo(l, split, zeropos);
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::doBalanceRightToLeft(OperationContext* txn,
                                                   BucketType* bucket,
                                                   const DiskLoc bucketLoc,
                                                   int leftIndex,
                                                   int split,
                                                   BucketType* l,
                                                   const DiskLoc lchild,
                                                   BucketType* r,
                                                   const DiskLoc rchild) {
    // As a precondition, lchild + the old separator are <= half a body size,
    // and rchild is at most completely full.  Based on the value of split,
    // lchild will get less than half of the total bytes which is at most 75%
    // of a full body.  So lchild will have room for the following keys:
    int lN = l->n;

    {
        // left child's right child becomes old parent key's left child
        FullKey kn = getFullKey(bucket, leftIndex);
        invariant(pushBack(l, kn.recordLoc, kn.data, l->nextChild));
    }

    for (int i = 0; i < split - lN - 1; ++i) {
        FullKey kn = getFullKey(r, i);
        invariant(pushBack(l, kn.recordLoc, kn.data, kn.prevChildBucket));
    }

    {
        FullKey kn = getFullKey(r, split - lN - 1);
        l->nextChild = kn.prevChildBucket;
        // Child lN was lchild's old nextChild, and don't need to fix that one.
        fixParentPtrs(txn, l, lchild, lN + 1, l->n);
        // Because rchild is a descendant of thisLoc, updating thisLoc will
        // not affect packing or keys of rchild and kn will be stable
        // during the following setInternalKey()
        setInternalKey(txn, bucket, bucketLoc, leftIndex, kn.recordLoc, kn.data, lchild, rchild);
    }

    // lchild and rchild cannot be merged, so there must be >0 (actually more)
    // keys to the right of split.
    int zeropos = 0;
    dropFront(r, split - lN, zeropos);
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::doBalanceChildren(OperationContext* txn,
                                                BucketType* bucket,
                                                const DiskLoc bucketLoc,
                                                int leftIndex) {
    DiskLoc lchild = childLocForPos(bucket, leftIndex);
    DiskLoc rchild = childLocForPos(bucket, leftIndex + 1);

    int zeropos = 0;
    BucketType* l = btreemod(txn, getBucket(txn, lchild));
    _packReadyForMod(l, zeropos);

    BucketType* r = btreemod(txn, getBucket(txn, rchild));
    _packReadyForMod(r, zeropos);

    int split = _rebalancedSeparatorPos(txn, bucket, leftIndex);

    // By definition, if we are below the low water mark and cannot merge
    // then we must actively balance.
    invariant(split != l->n);
    if (split < l->n) {
        doBalanceLeftToRight(txn, bucket, bucketLoc, leftIndex, split, l, lchild, r, rchild);
    } else {
        doBalanceRightToLeft(txn, bucket, bucketLoc, leftIndex, split, l, lchild, r, rchild);
    }
}

template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::mayBalanceWithNeighbors(OperationContext* txn,
                                                      BucketType* bucket,
                                                      const DiskLoc bucketLoc) {
    if (bucket->parent.isNull()) {
        return false;
    }

    if (_packedDataSize(bucket, 0) >= lowWaterMark()) {
        return false;
    }

    BucketType* p = getBucket(txn, bucket->parent);
    int parentIdx = indexInParent(txn, bucket, bucketLoc);

    // TODO will missing neighbor case be possible long term?  Should we try to merge/balance
    // somehow in that case if so?
    bool mayBalanceRight = (parentIdx < p->n) && !childLocForPos(p, parentIdx + 1).isNull();
    bool mayBalanceLeft = (parentIdx > 0) && !childLocForPos(p, parentIdx - 1).isNull();

    // Balance if possible on one side - we merge only if absolutely necessary to preserve btree
    // bucket utilization constraints since that's a more heavy duty operation (especially if we
    // must re-split later).
    if (mayBalanceRight && tryBalanceChildren(txn, p, bucket->parent, parentIdx)) {
        return true;
    }

    if (mayBalanceLeft && tryBalanceChildren(txn, p, bucket->parent, parentIdx - 1)) {
        return true;
    }

    BucketType* pm = btreemod(txn, getBucket(txn, bucket->parent));
    if (mayBalanceRight) {
        doMergeChildren(txn, pm, bucket->parent, parentIdx);
        return true;
    } else if (mayBalanceLeft) {
        doMergeChildren(txn, pm, bucket->parent, parentIdx - 1);
        return true;
    }

    return false;
}

template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::unindex(OperationContext* txn,
                                      const BSONObj& key,
                                      const DiskLoc& recordLoc) {
    int pos;
    bool found = false;
    KeyDataOwnedType ownedKey(key);

    DiskLoc loc = _locate(txn, getRootLoc(txn), ownedKey, &pos, &found, recordLoc, 1);
    if (found) {
        BucketType* bucket = btreemod(txn, getBucket(txn, loc));
        delKeyAtPos(txn, bucket, loc, pos);
        assertValid(_indexName, getRoot(txn), _ordering);
    }
    return found;
}

template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::isEmpty(OperationContext* txn) const {
    return getRoot(txn)->n == 0;
}

/**
 * This can cause a lot of additional page writes when we assign buckets to different parents.
 * Maybe get rid of parent ptrs?
 */
template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::fixParentPtrs(OperationContext* txn,
                                            BucketType* bucket,
                                            const DiskLoc bucketLoc,
                                            int firstIndex,
                                            int lastIndex) {
    invariant(getBucket(txn, bucketLoc) == bucket);

    if (lastIndex == -1) {
        lastIndex = bucket->n;
    }

    for (int i = firstIndex; i <= lastIndex; i++) {
        const DiskLoc childLoc = childLocForPos(bucket, i);
        if (!childLoc.isNull()) {
            *txn->recoveryUnit()->writing(&getBucket(txn, childLoc)->parent) = bucketLoc;
        }
    }
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::setInternalKey(OperationContext* txn,
                                             BucketType* bucket,
                                             const DiskLoc bucketLoc,
                                             int keypos,
                                             const DiskLoc recordLoc,
                                             const KeyDataType& key,
                                             const DiskLoc lchild,
                                             const DiskLoc rchild) {
    childLocForPos(bucket, keypos).Null();
    // This may leave the bucket empty (n == 0) which is ok only as a txnient state.  In the
    // instant case, the implementation of insertHere behaves correctly when n == 0 and as a
    // side effect increments n.
    _delKeyAtPos(bucket, keypos, true);

    // Ensure we do not orphan neighbor's old child.
    invariant(childLocForPos(bucket, keypos) == rchild);

    // Just set temporarily - required to pass validation in insertHere()
    childLocForPos(bucket, keypos) = lchild;

    insertHere(txn, bucketLoc, keypos, key, recordLoc, lchild, rchild);
}

/**
 * insert a key in this bucket, splitting if necessary.
 *
 * @keypos - where to insert the key in range 0..n.  0=make leftmost, n=make rightmost.  NOTE
 * this function may free some data, and as a result the value passed for keypos may be invalid
 * after calling insertHere()
 *
 * Some of the write intent signaling below relies on the implementation of the optimized write
 * intent code in basicInsert().
 */
template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::insertHere(OperationContext* txn,
                                         const DiskLoc bucketLoc,
                                         int pos,
                                         const KeyDataType& key,
                                         const DiskLoc recordLoc,
                                         const DiskLoc leftChildLoc,
                                         const DiskLoc rightChildLoc) {
    BucketType* bucket = getBucket(txn, bucketLoc);

    if (!basicInsert(txn, bucket, bucketLoc, pos, key, recordLoc)) {
        // If basicInsert() fails, the bucket will be packed as required by split().
        split(txn,
              btreemod(txn, bucket),
              bucketLoc,
              pos,
              recordLoc,
              key,
              leftChildLoc,
              rightChildLoc);
        return;
    }

    KeyHeaderType* kn = &getKeyHeader(bucket, pos);
    if (pos + 1 == bucket->n) {
        // It's the last key.
        if (bucket->nextChild != leftChildLoc) {
            // XXX log more
            invariant(false);
        }
        kn->prevChildBucket = bucket->nextChild;
        invariant(kn->prevChildBucket == leftChildLoc);
        *txn->recoveryUnit()->writing(&bucket->nextChild) = rightChildLoc;
        if (!rightChildLoc.isNull()) {
            *txn->recoveryUnit()->writing(&getBucket(txn, rightChildLoc)->parent) = bucketLoc;
        }
    } else {
        kn->prevChildBucket = leftChildLoc;
        if (getKeyHeader(bucket, pos + 1).prevChildBucket != leftChildLoc) {
            // XXX: log more
            invariant(false);
        }
        const LocType* pc = &getKeyHeader(bucket, pos + 1).prevChildBucket;
        // Intent declared in basicInsert()
        *const_cast<LocType*>(pc) = rightChildLoc;
        if (!rightChildLoc.isNull()) {
            *txn->recoveryUnit()->writing(&getBucket(txn, rightChildLoc)->parent) = bucketLoc;
        }
    }
}

template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::split(OperationContext* txn,
                                    BucketType* bucket,
                                    const DiskLoc bucketLoc,
                                    int keypos,
                                    const DiskLoc recordLoc,
                                    const KeyDataType& key,
                                    const DiskLoc lchild,
                                    const DiskLoc rchild) {
    int split = splitPos(bucket, keypos);
    DiskLoc rLoc = _addBucket(txn);
    BucketType* r = btreemod(txn, getBucket(txn, rLoc));

    for (int i = split + 1; i < bucket->n; i++) {
        FullKey kn = getFullKey(bucket, i);
        invariant(pushBack(r, kn.recordLoc, kn.data, kn.prevChildBucket));
    }
    r->nextChild = bucket->nextChild;
    assertValid(_indexName, r, _ordering);

    r = NULL;
    fixParentPtrs(txn, getBucket(txn, rLoc), rLoc);

    FullKey splitkey = getFullKey(bucket, split);
    // splitkey key gets promoted, its children will be thisLoc (l) and rLoc (r)
    bucket->nextChild = splitkey.prevChildBucket;

    // Because thisLoc is a descendant of parent, updating parent will not affect packing or
    // keys of thisLoc and splitkey will be stable during the following:

    if (bucket->parent.isNull()) {
        // promote splitkey to a parent this->node make a new parent if we were the root
        DiskLoc L = _addBucket(txn);
        BucketType* p = btreemod(txn, getBucket(txn, L));
        invariant(pushBack(p, splitkey.recordLoc, splitkey.data, bucketLoc));
        p->nextChild = rLoc;
        assertValid(_indexName, p, _ordering);
        bucket->parent = L;
        _headManager->setHead(txn, L.toRecordId());
        *txn->recoveryUnit()->writing(&getBucket(txn, rLoc)->parent) = bucket->parent;
    } else {
        // set this before calling _insert - if it splits it will do fixParent() logic and
        // change the value.
        *txn->recoveryUnit()->writing(&getBucket(txn, rLoc)->parent) = bucket->parent;
        _insert(txn,
                getBucket(txn, bucket->parent),
                bucket->parent,
                splitkey.data,
                splitkey.recordLoc,
                true,  // dupsallowed
                bucketLoc,
                rLoc);
    }

    int newpos = keypos;
    // note this may trash splitkey.key.  thus we had to promote it before finishing up here.
    truncateTo(bucket, split, newpos);

    // add our this->new key, there is room this->now
    if (keypos <= split) {
        insertHere(txn, bucketLoc, newpos, key, recordLoc, lchild, rchild);
    } else {
        int kp = keypos - split - 1;
        invariant(kp >= 0);
        insertHere(txn, rLoc, kp, key, recordLoc, lchild, rchild);
    }
}

class DummyDocWriter final : public DocWriter {
public:
    DummyDocWriter(size_t sz) : _sz(sz) {}
    virtual void writeDocument(char* buf) const { /* no-op */
    }
    virtual size_t documentSize() const {
        return _sz;
    }

private:
    size_t _sz;
};

template <class BtreeLayout>
Status BtreeLogic<BtreeLayout>::initAsEmpty(OperationContext* txn) {
    if (!_headManager->getHead(txn).isNull()) {
        return Status(ErrorCodes::InternalError, "index already initialized");
    }

    _headManager->setHead(txn, _addBucket(txn).toRecordId());
    return Status::OK();
}

template <class BtreeLayout>
DiskLoc BtreeLogic<BtreeLayout>::_addBucket(OperationContext* txn) {
    DummyDocWriter docWriter(BtreeLayout::BucketSize);
    StatusWith<RecordId> loc = _recordStore->insertRecordWithDocWriter(txn, &docWriter);
    // XXX: remove this(?) or turn into massert or sanely bubble it back up.
    uassertStatusOK(loc.getStatus());

    // this is a new bucket, not referenced by anyone, probably don't need this lock
    BucketType* b = btreemod(txn, getBucket(txn, loc.getValue()));
    init(b);
    return DiskLoc::fromRecordId(loc.getValue());
}

// static
template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::dumpBucket(const BucketType* bucket, int indentLength) {
    log() << "BUCKET n:" << bucket->n << ", parent:" << hex << bucket->parent.getOfs() << dec;

    const string indent = string(indentLength, ' ');

    for (int i = 0; i < bucket->n; i++) {
        log() << '\n' << indent;
        FullKey k = getFullKey(bucket, i);
        string ks = k.data.toString();
        log() << "  " << hex << k.prevChildBucket.getOfs() << "<-- prevChildBucket for " << i
              << '\n';
        log() << indent << "    " << i << ' ' << ks.substr(0, 30)
              << " Loc:" << k.recordLoc.toString() << dec;
        if (getKeyHeader(bucket, i).isUnused()) {
            log() << " UNUSED";
        }
    }

    log() << "\n" << indent << "  " << hex << bucket->nextChild.getOfs() << dec << endl;
}

template <class BtreeLayout>
DiskLoc BtreeLogic<BtreeLayout>::getDiskLoc(OperationContext* txn,
                                            const DiskLoc& bucketLoc,
                                            const int keyOffset) const {
    invariant(!bucketLoc.isNull());
    BucketType* bucket = getBucket(txn, bucketLoc);
    return getKeyHeader(bucket, keyOffset).recordLoc;
}

template <class BtreeLayout>
BSONObj BtreeLogic<BtreeLayout>::getKey(OperationContext* txn,
                                        const DiskLoc& bucketLoc,
                                        const int keyOffset) const {
    invariant(!bucketLoc.isNull());
    BucketType* bucket = getBucket(txn, bucketLoc);
    int n = bucket->n;
    invariant(n != BtreeLayout::INVALID_N_SENTINEL);
    invariant(n >= 0);
    invariant(n < 10000);
    invariant(n != 0xffff);

    invariant(keyOffset >= 0);
    invariant(keyOffset < n);

    // XXX: should we really return an empty obj if keyOffset>=n?
    if (keyOffset >= n) {
        return BSONObj();
    } else {
        return getFullKey(bucket, keyOffset).data.toBson();
    }
}

template <class BtreeLayout>
IndexKeyEntry BtreeLogic<BtreeLayout>::getRandomEntry(OperationContext* txn) const {
    // To ensure a uniform distribution, all keys must have an equal probability of being selected.
    // Specifically, a key from the root should have the same probability of being selected as a key
    // from a leaf.
    //
    // Here we do a random walk until we get to a leaf, storing a random key from each bucket along
    // the way down. Because the root is always present in the random walk, but any given leaf would
    // seldom be seen, we assign weights to each key such that the key from the leaf is much more
    // likely to be selected than the key from the root. These weights attempt to ensure each entry
    // is equally likely to be selected and avoid bias towards the entries closer to the root.
    //
    // As a simplification, we treat all buckets in a given level as having the same number of
    // children. While this is inaccurate if the tree isn't perfectly balanced or if key-size
    // greatly varies, it is assumed to be good enough for this purpose.
    invariant(!isEmpty(txn));
    BucketType* root = getRoot(txn);

    vector<int64_t> nKeysInLevel;
    vector<FullKey> selectedKeys;

    auto& prng = txn->getClient()->getPrng();

    int nRetries = 0;
    const int kMaxRetries = 5;
    do {
        // See documentation below for description of parameters.
        recordRandomWalk(txn, &prng, root, 1, &nKeysInLevel, &selectedKeys);
    } while (selectedKeys.empty() && nRetries++ < kMaxRetries);
    massert(28826,
            str::stream() << "index " << _indexName << " may be corrupt, please repair",
            !selectedKeys.empty());

    invariant(nKeysInLevel.size() == selectedKeys.size());
    // Select a key from the random walk such that each key from the B-tree has an equal probability
    // of being selected.
    //
    // Let N be the sum of 'nKeysInLevel'. That is, the total number of keys in the B-tree.
    //
    // On our walk down the tree, we selected exactly one key from each level of the B-tree, where
    // 'selectedKeys[i]' came from the ith level of the tree. On any given level, each key has an
    // equal probability of being selected. Specifically, a key on level i has a probability of
    // 1/'nKeysInLevel[i]' of being selected as 'selectedKeys[i]'. Then if, given our selected keys,
    // we choose to return 'selectedKeys[i]' with a probability of 'nKeysInLevel[i]'/N, that key
    // will be returned with a probability of 1/'nKeysInLevel[i]' * 'nKeysInLevel[i]'/N = 1/N.
    //
    // So 'selectedKeys[i]' should have a probability of 'nKeysInLevel[i]'/N of being returned. We
    // will do so by picking a random number X in the range [0, N). Then, if X is in the first
    // 'nKeysInLevel[0]' numbers, we will return 'selectedKeys[0]'. If X is in the next
    // 'nKeysInLevel[1]' numbers, we will return 'selectedKeys[1]', and so on.
    int64_t choice = prng.nextInt64(std::accumulate(nKeysInLevel.begin(), nKeysInLevel.end(), 0));
    for (size_t i = 0; i < nKeysInLevel.size(); i++) {
        if (choice < nKeysInLevel[i]) {
            return {selectedKeys[i].data.toBson(), selectedKeys[i].header.recordLoc.toRecordId()};
        }
        choice -= nKeysInLevel[i];
    }
    MONGO_UNREACHABLE;
}

/**
 * Does a random walk through the tree, recording information about the walk along the way.
 *
 * 'nKeysInLevel' will be filled in such that 'nKeysInLevel[i]' is an approximation of the number of
 * keys in the ith level of the B-tree.
 *
 * 'selectedKeys' will be filled in such that 'selectedKeys[i]' will be a pseudo-random key selected
 * from the bucket we went through on the ith level of the B-tree.
 */
template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::recordRandomWalk(OperationContext* txn,
                                               PseudoRandom* prng,
                                               BucketType* curBucket,
                                               int64_t nBucketsInCurrentLevel,
                                               vector<int64_t>* nKeysInLevel,
                                               vector<FullKey>* selectedKeys) const {
    // Select a random key from this bucket, and record it.
    int nKeys = curBucket->n;
    int keyToReturn = prng->nextInt32(nKeys);
    auto fullKey = getFullKey(curBucket, keyToReturn);
    // If the key is not used, just skip this level.
    if (fullKey.header.isUsed()) {
        selectedKeys->push_back(std::move(fullKey));
        nKeysInLevel->push_back(nBucketsInCurrentLevel * nKeys);
    }

    // Select a random child and descend (if there are any).
    int nChildren = nKeys + 1;
    int nextChild = prng->nextInt32(nChildren);
    if (auto child = childForPos(txn, curBucket, nextChild)) {
        recordRandomWalk(
            txn, prng, child, nBucketsInCurrentLevel * nChildren, nKeysInLevel, selectedKeys);
    }
}

template <class BtreeLayout>
Status BtreeLogic<BtreeLayout>::touch(OperationContext* txn) const {
    return _recordStore->touch(txn, NULL);
}

template <class BtreeLayout>
long long BtreeLogic<BtreeLayout>::fullValidate(OperationContext* txn,
                                                long long* unusedCount,
                                                bool strict,
                                                bool dumpBuckets,
                                                unsigned depth) const {
    return _fullValidate(txn, getRootLoc(txn), unusedCount, strict, dumpBuckets, depth);
}

template <class BtreeLayout>
long long BtreeLogic<BtreeLayout>::_fullValidate(OperationContext* txn,
                                                 const DiskLoc bucketLoc,
                                                 long long* unusedCount,
                                                 bool strict,
                                                 bool dumpBuckets,
                                                 unsigned depth) const {
    BucketType* bucket = getBucket(txn, bucketLoc);
    assertValid(_indexName, bucket, _ordering, true);

    if (dumpBuckets) {
        log() << bucketLoc.toString() << ' ';
        dumpBucket(bucket, depth);
    }

    long long keyCount = 0;

    for (int i = 0; i < bucket->n; i++) {
        KeyHeaderType& kn = getKeyHeader(bucket, i);

        if (kn.isUsed()) {
            keyCount++;
        } else if (NULL != unusedCount) {
            ++(*unusedCount);
        }

        if (!kn.prevChildBucket.isNull()) {
            DiskLoc left = kn.prevChildBucket;
            BucketType* b = getBucket(txn, left);

            if (strict) {
                invariant(b->parent == bucketLoc);
            } else {
                wassert(b->parent == bucketLoc);
            }

            keyCount += _fullValidate(txn, left, unusedCount, strict, dumpBuckets, depth + 1);
        }
    }

    if (!bucket->nextChild.isNull()) {
        BucketType* b = getBucket(txn, bucket->nextChild);
        if (strict) {
            invariant(b->parent == bucketLoc);
        } else {
            wassert(b->parent == bucketLoc);
        }

        keyCount +=
            _fullValidate(txn, bucket->nextChild, unusedCount, strict, dumpBuckets, depth + 1);
    }

    return keyCount;
}

// XXX: remove this(?)  used to not dump every key in assertValid.
int nDumped = 0;

// static
template <class BtreeLayout>
void BtreeLogic<BtreeLayout>::assertValid(const std::string& ns,
                                          BucketType* bucket,
                                          const Ordering& ordering,
                                          bool force) {
    if (!force) {
        return;
    }

    // this is very slow so don't do often
    {
        static int _k;
        if (++_k % 128) {
            return;
        }
    }

    DEV {
        // slow:
        for (int i = 0; i < bucket->n - 1; i++) {
            FullKey firstKey = getFullKey(bucket, i);
            FullKey secondKey = getFullKey(bucket, i + 1);
            int z = firstKey.data.woCompare(secondKey.data, ordering);
            if (z > 0) {
                log() << "ERROR: btree key order corrupt.  Keys:" << endl;
                if (++nDumped < 5) {
                    for (int j = 0; j < bucket->n; j++) {
                        log() << "  " << getFullKey(bucket, j).data.toString() << endl;
                    }
                    dumpBucket(bucket);
                }
                wassert(false);
                break;
            } else if (z == 0) {
                if (!(firstKey.header.recordLoc < secondKey.header.recordLoc)) {
                    log() << "ERROR: btree key order corrupt (recordlocs wrong):" << endl;
                    log() << " k(" << i << ")" << firstKey.data.toString()
                          << " RL:" << firstKey.header.recordLoc.toString() << endl;
                    log() << " k(" << i + 1 << ")" << secondKey.data.toString()
                          << " RL:" << secondKey.header.recordLoc.toString() << endl;
                    wassert(firstKey.header.recordLoc < secondKey.header.recordLoc);
                }
            }
        }
    }
    else {
        // faster:
        if (bucket->n > 1) {
            FullKey k1 = getFullKey(bucket, 0);
            FullKey k2 = getFullKey(bucket, bucket->n - 1);
            int z = k1.data.woCompare(k2.data, ordering);
            // wassert( z <= 0 );
            if (z > 0) {
                log() << "Btree keys out of order in collection " << ns;
                ONCE {
                    dumpBucket(bucket);
                }
                invariant(false);
            }
        }
    }
}

template <class BtreeLayout>
Status BtreeLogic<BtreeLayout>::insert(OperationContext* txn,
                                       const BSONObj& rawKey,
                                       const DiskLoc& value,
                                       bool dupsAllowed) {
    KeyDataOwnedType key(rawKey);

    if (key.dataSize() > BtreeLayout::KeyMax) {
        string msg = str::stream() << "Btree::insert: key too large to index, failing "
                                   << _indexName << ' ' << key.dataSize() << ' ' << key.toString();
        return Status(ErrorCodes::KeyTooLong, msg);
    }

    Status status =
        _insert(txn, getRoot(txn), getRootLoc(txn), key, value, dupsAllowed, DiskLoc(), DiskLoc());

    assertValid(_indexName, getRoot(txn), _ordering);
    return status;
}

template <class BtreeLayout>
Status BtreeLogic<BtreeLayout>::_insert(OperationContext* txn,
                                        BucketType* bucket,
                                        const DiskLoc bucketLoc,
                                        const KeyDataType& key,
                                        const DiskLoc recordLoc,
                                        bool dupsAllowed,
                                        const DiskLoc leftChild,
                                        const DiskLoc rightChild) {
    invariant(key.dataSize() > 0);

    int pos;
    bool found;
    Status findStatus = _find(txn, bucket, key, recordLoc, !dupsAllowed, &pos, &found);
    if (!findStatus.isOK()) {
        return findStatus;
    }

    if (found) {
        KeyHeaderType& header = getKeyHeader(bucket, pos);
        if (header.isUnused()) {
            LOG(4) << "btree _insert: reusing unused key" << endl;
            massert(17433, "_insert: reuse key but lchild is not null", leftChild.isNull());
            massert(17434, "_insert: reuse key but rchild is not null", rightChild.isNull());
            txn->recoveryUnit()->writing(&header)->setUsed();
            return Status::OK();
        }
        // The logic in _find() prohibits finding and returning a position if the 'used' bit
        // in the header is set and dups are disallowed.
        invariant(dupsAllowed);

        // The key and value are already in the index. Not an error because documents that have
        // already been indexed may be seen again due to updates during a background index scan.
        return Status::OK();
    }

    DiskLoc childLoc = childLocForPos(bucket, pos);

    // In current usage, rightChild is NULL for a new key and is not NULL when we are
    // promoting a split key.  These are the only two cases where _insert() is called
    // currently.
    if (childLoc.isNull() || !rightChild.isNull()) {
        insertHere(txn, bucketLoc, pos, key, recordLoc, leftChild, rightChild);
        return Status::OK();
    } else {
        return _insert(txn,
                       getBucket(txn, childLoc),
                       childLoc,
                       key,
                       recordLoc,
                       dupsAllowed,
                       DiskLoc(),
                       DiskLoc());
    }
}

template <class BtreeLayout>
DiskLoc BtreeLogic<BtreeLayout>::advance(OperationContext* txn,
                                         const DiskLoc& bucketLoc,
                                         int* posInOut,
                                         int direction) const {
    BucketType* bucket = getBucket(txn, bucketLoc);

    if (*posInOut < 0 || *posInOut >= bucket->n) {
        log() << "ASSERT failure advancing btree bucket" << endl;
        log() << "  thisLoc: " << bucketLoc.toString() << endl;
        log() << "  keyOfs: " << *posInOut << " n:" << bucket->n << " direction: " << direction
              << endl;
        // log() << bucketSummary() << endl;
        invariant(false);
    }

    // XXX document
    int adj = direction < 0 ? 1 : 0;
    int ko = *posInOut + direction;

    // Look down if we need to.
    DiskLoc nextDownLoc = childLocForPos(bucket, ko + adj);
    BucketType* nextDown = getBucket(txn, nextDownLoc);
    if (NULL != nextDown) {
        for (;;) {
            if (direction > 0) {
                *posInOut = 0;
            } else {
                *posInOut = nextDown->n - 1;
            }
            DiskLoc newNextDownLoc = childLocForPos(nextDown, *posInOut + adj);
            BucketType* newNextDownBucket = getBucket(txn, newNextDownLoc);
            if (NULL == newNextDownBucket) {
                break;
            }
            nextDownLoc = newNextDownLoc;
            nextDown = newNextDownBucket;
        }
        return nextDownLoc;
    }

    // Looking down isn't the right choice, move forward.
    if (ko < bucket->n && ko >= 0) {
        *posInOut = ko;
        return bucketLoc;
    }

    // Hit the end of the bucket, move up and over.
    DiskLoc childLoc = bucketLoc;
    DiskLoc ancestor = getBucket(txn, bucketLoc)->parent;
    for (;;) {
        if (ancestor.isNull()) {
            break;
        }
        BucketType* an = getBucket(txn, ancestor);
        for (int i = 0; i < an->n; i++) {
            if (childLocForPos(an, i + adj) == childLoc) {
                *posInOut = i;
                return ancestor;
            }
        }
        invariant(direction < 0 || an->nextChild == childLoc);
        // parent exhausted also, keep going up
        childLoc = ancestor;
        ancestor = an->parent;
    }

    return DiskLoc();
}

template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::keyIsUsed(OperationContext* txn,
                                        const DiskLoc& loc,
                                        const int& pos) const {
    return getKeyHeader(getBucket(txn, loc), pos).isUsed();
}

template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::locate(OperationContext* txn,
                                     const BSONObj& key,
                                     const DiskLoc& recordLoc,
                                     const int direction,
                                     int* posOut,
                                     DiskLoc* bucketLocOut) const {
    // Clear out any data.
    *posOut = 0;
    *bucketLocOut = DiskLoc();

    bool found = false;
    KeyDataOwnedType owned(key);

    *bucketLocOut = _locate(txn, getRootLoc(txn), owned, posOut, &found, recordLoc, direction);

    if (!found) {
        return false;
    }

    skipUnusedKeys(txn, bucketLocOut, posOut, direction);

    return found;
}

/**
 * Recursively walk down the btree, looking for a match of key and recordLoc.
 * Caller should have acquired lock on bucketLoc.
 */
template <class BtreeLayout>
DiskLoc BtreeLogic<BtreeLayout>::_locate(OperationContext* txn,
                                         const DiskLoc& bucketLoc,
                                         const KeyDataType& key,
                                         int* posOut,
                                         bool* foundOut,
                                         const DiskLoc& recordLoc,
                                         const int direction) const {
    int position;
    BucketType* bucket = getBucket(txn, bucketLoc);
    // XXX: owned to not owned conversion(?)
    _find(txn, bucket, key, recordLoc, false, &position, foundOut);

    // Look in our current bucket.
    if (*foundOut) {
        *posOut = position;
        return bucketLoc;
    }

    // Not in our current bucket.  'position' tells us where there may be a child.
    DiskLoc childLoc = childLocForPos(bucket, position);

    if (!childLoc.isNull()) {
        DiskLoc inChild = _locate(txn, childLoc, key, posOut, foundOut, recordLoc, direction);
        if (!inChild.isNull()) {
            return inChild;
        }
    }

    *posOut = position;

    if (direction < 0) {
        // The key *would* go to our left.
        (*posOut)--;
        if (-1 == *posOut) {
            // But there's no space for that in our bucket.
            return DiskLoc();
        } else {
            return bucketLoc;
        }
    } else {
        // The key would go to our right...
        if (bucket->n == *posOut) {
            return DiskLoc();
        } else {
            // But only if there is space.
            return bucketLoc;
        }
    }
}

// TODO relcoate
template <class BtreeLayout>
bool BtreeLogic<BtreeLayout>::isHead(BucketType* bucket) {
    return bucket->parent.isNull();
}

template <class BtreeLayout>
typename BtreeLogic<BtreeLayout>::BucketType* BtreeLogic<BtreeLayout>::getBucket(
    OperationContext* txn, const RecordId id) const {
    if (id.isNull()) {
        return NULL;
    }

    RecordData recordData = _recordStore->dataFor(txn, id);

    // we need to be working on the raw bytes, not a transient copy
    invariant(!recordData.isOwned());

    return reinterpret_cast<BucketType*>(const_cast<char*>(recordData.data()));
}

template <class BtreeLayout>
typename BtreeLogic<BtreeLayout>::BucketType* BtreeLogic<BtreeLayout>::getRoot(
    OperationContext* txn) const {
    return getBucket(txn, _headManager->getHead(txn));
}

template <class BtreeLayout>
DiskLoc BtreeLogic<BtreeLayout>::getRootLoc(OperationContext* txn) const {
    return DiskLoc::fromRecordId(_headManager->getHead(txn));
}

template <class BtreeLayout>
typename BtreeLogic<BtreeLayout>::BucketType* BtreeLogic<BtreeLayout>::childForPos(
    OperationContext* txn, BucketType* bucket, int pos) const {
    DiskLoc loc = childLocForPos(bucket, pos);
    return getBucket(txn, loc);
}

template <class BtreeLayout>
typename BtreeLogic<BtreeLayout>::LocType& BtreeLogic<BtreeLayout>::childLocForPos(
    BucketType* bucket, int pos) {
    if (bucket->n == pos) {
        return bucket->nextChild;
    } else {
        return getKeyHeader(bucket, pos).prevChildBucket;
    }
}

//
// And, template stuff.
//

// V0 format.
template struct FixedWidthKey<DiskLoc>;
template class BtreeLogic<BtreeLayoutV0>;

// V1 format.
template struct FixedWidthKey<DiskLoc56Bit>;
template class BtreeLogic<BtreeLayoutV1>;

}  // namespace mongo
