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


#include "mongo/db/diskloc.h"
#include "mongo/db/index/btree_index_cursor.h"  // for aboutToDeleteBucket
#include "mongo/db/jsobj.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/dur_commitjob.h"
#include "mongo/db/storage/mmap_v1/dur_transaction.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/storage/transaction.h"
#include "mongo/db/structure/btree/btree_logic.h"
#include "mongo/db/structure/btree/key.h"
#include "mongo/db/structure/record_store.h"

namespace mongo {

    //
    // Public Builder logic
    //

    template <class BtreeLayout>
    typename BtreeLogic<BtreeLayout>::Builder*
    BtreeLogic<BtreeLayout>::newBuilder(TransactionExperiment* trans, bool dupsAllowed) {
        return new Builder(this, trans, dupsAllowed);
    }

    template <class BtreeLayout>
    BtreeLogic<BtreeLayout>::Builder::Builder(BtreeLogic* logic,
                                              TransactionExperiment* trans,
                                              bool dupsAllowed)
        : _logic(logic),
          _dupsAllowed(dupsAllowed),
          _numAdded(0),
          _trans(trans) {

        _first = _cur = _logic->addBucket(trans);
        _b = _getModifiableBucket(_cur);
        _committed = false;
    }

    template <class BtreeLayout>
    Status BtreeLogic<BtreeLayout>::Builder::addKey(const BSONObj& keyObj, const DiskLoc& loc) {
        auto_ptr<KeyDataOwnedType> key(new KeyDataOwnedType(keyObj));

        if (key->dataSize() > BtreeLayout::KeyMax) {
            string msg = str::stream() << "Btree::insert: key too large to index, failing "
                                       << _logic->_indexName
                                       << ' ' << key->dataSize() << ' ' << key->toString();
            problem() << msg << endl;
            return Status(ErrorCodes::KeyTooLong, msg);
        }

        // If we have a previous key to compare to...
        if (_numAdded > 0) {
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

        if (!_logic->_pushBack(_b, loc, *key, DiskLoc())) {
            // bucket was full
            newBucket();
            _logic->pushBack(_b, loc, *key, DiskLoc());
        }

        _keyLast = key;
        _numAdded++;
        mayCommitProgressDurably();
        return Status::OK();
    }

    template <class BtreeLayout>
    unsigned long long BtreeLogic<BtreeLayout>::Builder::commit(bool mayInterrupt) {
        buildNextLevel(_first, mayInterrupt);
        _committed = true;
        return _numAdded;
    }

    //
    // Private Builder logic
    //

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::Builder::newBucket() {
        DiskLoc newBucketLoc = _logic->addBucket(_trans);
        _b->parent = newBucketLoc;
        _cur = newBucketLoc;
        _b = _getModifiableBucket(_cur);
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::Builder::buildNextLevel(DiskLoc loc, bool mayInterrupt) {
        for (;;) {
            if (_getBucket(loc)->parent.isNull()) {
                // only 1 bucket at this level. we are done.
                _logic->_headManager->setHead(loc);
                break;
            }

            DiskLoc upLoc = _logic->addBucket(_trans);
            DiskLoc upStart = upLoc;
            BucketType* up = _getModifiableBucket(upLoc);

            DiskLoc xloc = loc;
            while (!xloc.isNull()) {
                if (_trans->commitIfNeeded()) {
                    _b = _getModifiableBucket(_cur);
                    up = _getModifiableBucket(upLoc);
                }

                if (mayInterrupt) {
                    killCurrentOp.checkForInterrupt();
                }

                BucketType* x = _getModifiableBucket(xloc);
                KeyDataType k;
                DiskLoc r;
                _logic->popBack(x, &r, &k);
                bool keepX = (x->n != 0);
                DiskLoc keepLoc = keepX ? xloc : x->nextChild;

                if (!_logic->_pushBack(up, r, k, keepLoc)) {
                    // current bucket full
                    DiskLoc n = _logic->addBucket(_trans);
                    up->parent = n;
                    upLoc = n;
                    up = _getModifiableBucket(upLoc);
                    _logic->pushBack(up, r, k, keepLoc);
                }

                DiskLoc nextLoc = x->parent;
                if (keepX) {
                    x->parent = upLoc;
                }
                else {
                    if (!x->nextChild.isNull()) {
                        DiskLoc ll = x->nextChild;
                        _getModifiableBucket(ll)->parent = upLoc;
                    }
                    _logic->deallocBucket(x, xloc);
                }
                xloc = nextLoc;
            }

            loc = upStart;
            mayCommitProgressDurably();
        }
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::Builder::mayCommitProgressDurably() {
        if (_trans->commitIfNeeded()) {
            _b = _getModifiableBucket(_cur);
        }
    }

    template <class BtreeLayout>
    typename BtreeLogic<BtreeLayout>::BucketType*
    BtreeLogic<BtreeLayout>::Builder::_getModifiableBucket(DiskLoc loc) {
        return _logic->btreemod(_trans, _logic->getBucket(loc));
    }

    template <class BtreeLayout>
    typename BtreeLogic<BtreeLayout>::BucketType*
    BtreeLogic<BtreeLayout>::Builder::_getBucket(DiskLoc loc) {
        return _logic->getBucket(loc);
    }

    //
    // BtreeLogic logic
    //

    // static
    template <class BtreeLayout>
    typename BtreeLogic<BtreeLayout>::FullKey
    BtreeLogic<BtreeLayout>::getFullKey(const BucketType* bucket, int i) {
        if (i >= bucket->n) {
            int code = 13000;
            massert(code,
                    (string)"invalid keyNode: " + BSON( "i" << i << "n" << bucket->n ).jsonString(),
                    i < bucket->n );
        }
        return FullKey(bucket, i);
    }

    // static
    template <class BtreeLayout>
    typename BtreeLogic<BtreeLayout>::KeyHeaderType&
    BtreeLogic<BtreeLayout>::getKeyHeader(BucketType* bucket, int i) {
        return ((KeyHeaderType*)bucket->data)[i];
    }

    // static
    template <class BtreeLayout>
    const typename BtreeLogic<BtreeLayout>::KeyHeaderType&
    BtreeLogic<BtreeLayout>::getKeyHeader(const BucketType* bucket, int i) {
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
    typename BtreeLogic<BtreeLayout>::BucketType*
    BtreeLogic<BtreeLayout>::btreemod(TransactionExperiment* trans, BucketType* bucket) {
        trans->writingPtr(bucket, BtreeLayout::BucketSize);
        return bucket;
    }

    template <class BtreeLayout>
    int BtreeLogic<BtreeLayout>::totalDataSize(BucketType* bucket) {
        return (int) (BtreeLayout::BucketSize - (bucket->data - (char*)bucket));
    }

    template <class BtreeLayout>
    int BtreeLogic<BtreeLayout>::headerSize() {
        const BucketType* b = NULL;
        return (char*)&(b->data) - (char*)&(b->parent);
    }

    template <class BtreeLayout>
    int BtreeLogic<BtreeLayout>::bodySize() {
        return BtreeLayout::BucketSize - headerSize();
    }

    // We define this value as the maximum number of bytes such that, if we have
    // fewer than this many bytes, we must be able to either merge with or receive
    // keys from any neighboring node.  If our utilization goes below this value we
    // know we can bring up the utilization with a simple operation.  Ignoring the
    // 90/10 split policy which is sometimes employed and our 'unused' nodes, this
    // is a lower bound on bucket utilization for non root buckets.
    //
    // Note that the exact value here depends on the implementation of
    // rebalancedSeparatorPos().  The conditions for lowWaterMark - 1 are as
    // follows:  We know we cannot merge with the neighbor, so the total data size
    // for us, the neighbor, and the separator must be at least
    // BucketType::bodySize() + 1.  We must be able to accept one key of any
    // allowed size, so our size plus storage for that additional key must be
    // <= BucketType::bodySize() / 2.  This way, with the extra key we'll have a
    // new bucket data size < half the total data size and by the implementation
    // of rebalancedSeparatorPos() the key must be added.
    template <class BtreeLayout>
    int BtreeLogic<BtreeLayout>::lowWaterMark() {
        return bodySize() / 2 - BtreeLayout::KeyMax - sizeof(KeyHeaderType) + 1;
    }

    // XXX

    enum Flags {
        Packed = 1
    };

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
     * Pull rightmost key from the bucket.  This version requires its right child to be null so it
     * does not bother returning that value.
     */
    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::popBack(BucketType* bucket,
                                          DiskLoc* recordLocOut,
                                          KeyDataType *keyDataOut) {

        massert(17435,  "n==0 in btree popBack()", bucket->n > 0 );

        invariant(getKeyHeader(bucket, bucket->n - 1).isUsed());

        FullKey kn = getFullKey(bucket, bucket->n - 1);
        *recordLocOut = kn.recordLoc;
        keyDataOut->assign(kn.data);
        int keysize = kn.data.dataSize();

        massert(17436, "rchild not null in btree popBack()", bucket->nextChild.isNull());

        // Weirdly, we also put the rightmost down pointer in nextchild, even when bucket isn't
        // full.
        bucket->nextChild = kn.prevChildBucket;
        bucket->n--;
        // This is risky because the key we are returning points to this unalloc'ed memory,
        // and we are assuming that the last key points to the last allocated
        // bson region.
        bucket->emptySize += sizeof(KeyHeaderType);
        _unalloc(bucket, keysize);
    }

    /**
     * Add a key.  Must be > all existing.  Be careful to set next ptr right.
     */
    template <class BtreeLayout>
    bool BtreeLogic<BtreeLayout>::_pushBack(BucketType* bucket,
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
                         "consider reindexing or running validate command" << endl;
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
        char *p = dataAt(bucket, ofs);
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
    bool BtreeLogic<BtreeLayout>::basicInsert(TransactionExperiment* trans,
                                              BucketType* bucket,
                                              const DiskLoc bucketLoc,
                                              int& keypos,
                                              const KeyDataType& key,
                                              const DiskLoc recordLoc) {
        invariant(bucket->n < 1024);
        invariant(keypos >= 0 && keypos <= bucket->n);

        int bytesNeeded = key.dataSize() + sizeof(KeyHeaderType);
        if (bytesNeeded > bucket->emptySize) {
            _pack(trans, bucket, bucketLoc, keypos);
            if (bytesNeeded > bucket->emptySize) {
                return false;
            }
        }

        invariant(getBucket(bucketLoc) == bucket);

        {
            // declare that we will write to [k(keypos),k(n)]
            const char *p = (const char *) &getKeyHeader(bucket, keypos);
            const char *q = (const char *) &getKeyHeader(bucket, bucket->n+1);

            // Declare that we will write to [k(keypos),k(n)]
            trans->writingPtr((void*)( (char*)bucket + (p - (char*)bucket)), q - p);
        }

        // e.g. for n==3, keypos==2
        // 1 4 9 -> 1 4 _ 9
        for (int j = bucket->n; j > keypos; j--) {
            getKeyHeader(bucket, j) = getKeyHeader(bucket, j - 1);
        }

        size_t writeLen = sizeof(bucket->emptySize) + sizeof(bucket->topSize) + sizeof(bucket->n);
        trans->writingPtr(&bucket->emptySize, writeLen);
        bucket->emptySize -= sizeof(KeyHeaderType);
        bucket->n++;

        // This _KeyNode was marked for writing above.
        KeyHeaderType& kn = getKeyHeader(bucket, keypos);
        kn.prevChildBucket.Null();
        kn.recordLoc = recordLoc;
        kn.setKeyDataOfs((short) _alloc(bucket, key.dataSize()));
        char *p = dataAt(bucket, kn.keyDataOfs());
        trans->writingPtr(p, key.dataSize());
        memcpy(p, key.data(), key.dataSize());
        return true;
    }

    /**
     * With this implementation, refPos == 0 disregards effect of refPos.  index > 0 prevents
     * creation of an empty bucket.
     */
    template <class BtreeLayout>
    bool BtreeLogic<BtreeLayout>::mayDropKey(BucketType* bucket, int index, int refPos) {
        return index > 0
            && (index != refPos)
            && getKeyHeader(bucket, index).isUnused()
            && getKeyHeader(bucket, index).prevChildBucket.isNull();
    }

    template <class BtreeLayout>
    int BtreeLogic<BtreeLayout>::packedDataSize(BucketType* bucket, int refPos) {
        if (bucket->flags & Packed) {
            return BtreeLayout::BucketSize - bucket->emptySize - headerSize();
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
    void BtreeLogic<BtreeLayout>::_pack(TransactionExperiment* trans,
                                        BucketType* bucket,
                                        const DiskLoc thisLoc,
                                        int &refPos) {

        invariant(getBucket(thisLoc) == bucket);

        if (bucket->flags & Packed) {
            return;
        }

        _packReadyForMod(btreemod(trans, bucket), refPos);
    }

    /**
     * Version when write intent already declared.
     */
    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::_packReadyForMod(BucketType* bucket, int &refPos) {
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
        {
            int foo = bucket->emptySize;
            invariant( foo >= 0 );
        }
        setPacked(bucket);
        assertValid(bucket, _ordering);
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::truncateTo(BucketType* bucket,
                                              int N,
                                              int &refPos) {
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
        int rightSizeLimit = (bucket->topSize + sizeof(KeyHeaderType) * bucket->n)
                           / (keypos == bucket->n ? 10 : 2);

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
        }
        else if (split > bucket->n - 2) {
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
        KeyHeaderType &kn = getKeyHeader(bucket, i);
        kn.recordLoc = recordLoc;
        kn.prevChildBucket = prevChildBucket;
        short ofs = (short) _alloc(bucket, key.dataSize());
        kn.setKeyDataOfs(ofs);
        char *p = dataAt(bucket, ofs);
        memcpy(p, key.data(), key.dataSize());
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::dropFront(BucketType* bucket,
                                             int nDrop,
                                             int &refpos) {
        for (int i = nDrop; i < bucket->n; ++i) {
            getKeyHeader(bucket, i - nDrop) = getKeyHeader(bucket, i);
        }
        bucket->n -= nDrop;
        setNotPacked(bucket);
        _packReadyForMod(bucket, refpos );
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::customLocate(DiskLoc* locInOut,
                                               int* keyOfsInOut,
                                               const BSONObj& keyBegin,
                                               int keyBeginLen,
                                               bool afterKey,
                                               const vector<const BSONElement*>& keyEnd,
                                               const vector<bool>& keyEndInclusive,
                                               int direction) const {
        pair<DiskLoc, int> unused;

        customLocate(locInOut,
                     keyOfsInOut,
                     keyBegin,
                     keyBeginLen,
                     afterKey, 
                     keyEnd,
                     keyEndInclusive,
                     direction,
                     unused);

        skipUnusedKeys(locInOut, keyOfsInOut, direction);
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::advance(DiskLoc* bucketLocInOut,
                                          int* posInOut,
                                          int direction) const {

        *bucketLocInOut = advance(*bucketLocInOut, posInOut, direction);
        skipUnusedKeys(bucketLocInOut, posInOut, direction);
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::skipUnusedKeys(DiskLoc* loc, int* pos, int direction) const {
        while (!loc->isNull() && !keyIsUsed(*loc, *pos)) {
            *loc = advance(*loc, pos, direction);
        }
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::advanceTo(DiskLoc* thisLocInOut,
                                            int* keyOfsInOut,
                                            const BSONObj &keyBegin,
                                            int keyBeginLen,
                                            bool afterKey,
                                            const vector<const BSONElement*>& keyEnd,
                                            const vector<bool>& keyEndInclusive,
                                            int direction) const {

        advanceToImpl(thisLocInOut,
                      keyOfsInOut,
                      keyBegin,
                      keyBeginLen,
                      afterKey,
                      keyEnd,
                      keyEndInclusive,
                      direction);

        skipUnusedKeys(thisLocInOut, keyOfsInOut, direction);
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
    void BtreeLogic<BtreeLayout>::advanceToImpl(DiskLoc* thisLocInOut,
                                                int* keyOfsInOut,
                                                const BSONObj &keyBegin,
                                                int keyBeginLen,
                                                bool afterKey,
                                                const vector<const BSONElement*>& keyEnd,
                                                const vector<bool>& keyEndInclusive,
                                                int direction) const {

        BucketType* bucket = getBucket(*thisLocInOut);

        int l, h;
        bool dontGoUp;

        if (direction > 0) {
            l = *keyOfsInOut;
            h = bucket->n - 1;
            int cmpResult = customBSONCmp(getFullKey(bucket, h).data.toBson(),
                                          keyBegin,
                                          keyBeginLen,
                                          afterKey,
                                          keyEnd,
                                          keyEndInclusive,
                                          _ordering,
                                          direction);
            dontGoUp = (cmpResult >= 0);
        }
        else {
            l = 0;
            h = *keyOfsInOut;
            int cmpResult = customBSONCmp(getFullKey(bucket, l).data.toBson(),
                                          keyBegin,
                                          keyBeginLen,
                                          afterKey,
                                          keyEnd,
                                          keyEndInclusive,
                                          _ordering,
                                          direction);
            dontGoUp = (cmpResult <= 0);
        }

        pair<DiskLoc, int> bestParent;

        if (dontGoUp) {
            // this comparison result assures h > l
            if (!customFind(l,
                            h,
                            keyBegin,
                            keyBeginLen,
                            afterKey,
                            keyEnd,
                            keyEndInclusive,
                            _ordering,
                            direction,
                            thisLocInOut,
                            keyOfsInOut,
                            bestParent)) {
                return;
            }
        }
        else {
            // go up parents until rightmost/leftmost node is >=/<= target or at top
            while (!bucket->parent.isNull()) {
                *thisLocInOut = bucket->parent;
                bucket = getBucket(*thisLocInOut);

                if (direction > 0) {
                    if (customBSONCmp(getFullKey(bucket, bucket->n - 1).data.toBson(),
                                      keyBegin,
                                      keyBeginLen,
                                      afterKey,
                                      keyEnd,
                                      keyEndInclusive,
                                      _ordering,
                                      direction) >= 0 ) {
                        break;
                    }
                }
                else {
                    if (customBSONCmp(getFullKey(bucket, 0).data.toBson(),
                                      keyBegin,
                                      keyBeginLen,
                                      afterKey,
                                      keyEnd,
                                      keyEndInclusive,
                                      _ordering,
                                      direction) <= 0) {
                        break;
                    }
                }
            }
        }

        customLocate(thisLocInOut,
                     keyOfsInOut,
                     keyBegin,
                     keyBeginLen,
                     afterKey,
                     keyEnd,
                     keyEndInclusive,
                     direction,
                     bestParent);
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::customLocate(DiskLoc* locInOut,
                                               int* keyOfsInOut,
                                               const BSONObj& keyBegin,
                                               int keyBeginLen,
                                               bool afterKey,
                                               const vector<const BSONElement*>& keyEnd,
                                               const vector<bool>& keyEndInclusive,
                                               int direction,
                                               pair<DiskLoc, int>& bestParent) const {

        BucketType* bucket = getBucket(*locInOut);

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
            int res = customBSONCmp(getFullKey(bucket, z).data.toBson(),
                                    keyBegin,
                                    keyBeginLen,
                                    afterKey,
                                    keyEnd,
                                    keyEndInclusive,
                                    _ordering,
                                    direction);


            if (direction * res >= 0) {
                DiskLoc next;
                *keyOfsInOut = z;

                if (direction > 0) {
                    dassert(z == 0);
                    next = getKeyHeader(bucket, 0).prevChildBucket;
                }
                else {
                    next = bucket->nextChild;
                }

                if (!next.isNull()) {
                    bestParent = pair<DiskLoc, int>(*locInOut, *keyOfsInOut);
                    *locInOut = next;
                    bucket = getBucket(*locInOut);
                    continue;
                }
                else {
                    return;
                }
            }

            res = customBSONCmp(getFullKey(bucket, h - z).data.toBson(),
                                keyBegin,
                                keyBeginLen,
                                afterKey,
                                keyEnd,
                                keyEndInclusive,
                                _ordering,
                                direction);

            if (direction * res < 0) {
                DiskLoc next;
                if (direction > 0) {
                    next = bucket->nextChild;
                }
                else {
                    next = getKeyHeader(bucket, 0).prevChildBucket;
                }

                if (next.isNull()) {
                    // if bestParent is null, we've hit the end and locInOut gets set to DiskLoc()
                    *locInOut = bestParent.first;
                    *keyOfsInOut = bestParent.second;
                    return;
                }
                else {
                    *locInOut = next;
                    bucket = getBucket(*locInOut);
                    continue;
                }
            }

            if (!customFind(l,
                            h,
                            keyBegin,
                            keyBeginLen,
                            afterKey,
                            keyEnd,
                            keyEndInclusive,
                            _ordering,
                            direction,
                            locInOut,
                            keyOfsInOut,
                            bestParent)) {
                return;
            }

            bucket = getBucket(*locInOut);
        }
    }

    template <class BtreeLayout>
    bool BtreeLogic<BtreeLayout>::customFind(int low,
                                             int high,
                                             const BSONObj& keyBegin,
                                             int keyBeginLen,
                                             bool afterKey,
                                             const vector<const BSONElement*>& keyEnd,
                                             const vector<bool>& keyEndInclusive,
                                             const Ordering& order,
                                             int direction,
                                             DiskLoc* thisLocInOut,
                                             int* keyOfsInOut,
                                             pair<DiskLoc, int>& bestParent) const {

        const BucketType* bucket = getBucket(*thisLocInOut);

        for (;;) {
            if (low + 1 == high) {
                *keyOfsInOut = (direction > 0) ? high : low;
                DiskLoc next = getKeyHeader(bucket, high).prevChildBucket;
                if (!next.isNull()) {
                    bestParent = make_pair(*thisLocInOut, *keyOfsInOut);
                    *thisLocInOut = next;
                    return true;
                }
                else {
                    return false;
                }
            }

            int middle = low + (high - low) / 2;

            int cmp = customBSONCmp(getFullKey(bucket, middle).data.toBson(),
                                    keyBegin,
                                    keyBeginLen,
                                    afterKey,
                                    keyEnd,
                                    keyEndInclusive,
                                    order,
                                    direction);

            if (cmp < 0) {
                low = middle;
            }
            else if (cmp > 0) {
                high = middle;
            }
            else {
                if (direction < 0) {
                    low = middle;
                }
                else {
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
    int BtreeLogic<BtreeLayout>::customBSONCmp(const BSONObj& l,
                                               const BSONObj& rBegin,
                                               int rBeginLen,
                                               bool rSup,
                                               const vector<const BSONElement*>& rEnd,
                                               const vector<bool>& rEndInclusive,
                                               const Ordering& o,
                                               int direction) const {
        // XXX: make this readable
        BSONObjIterator ll( l );
        BSONObjIterator rr( rBegin );
        vector< const BSONElement * >::const_iterator rr2 = rEnd.begin();
        vector< bool >::const_iterator inc = rEndInclusive.begin();
        unsigned mask = 1;
        for( int i = 0; i < rBeginLen; ++i, mask <<= 1 ) {
            BSONElement lll = ll.next();
            BSONElement rrr = rr.next();
            ++rr2;
            ++inc;

            int x = lll.woCompare( rrr, false );
            if ( o.descending( mask ) )
                x = -x;
            if ( x != 0 )
                return x;
        }
        if ( rSup ) {
            return -direction;
        }
        for( ; ll.more(); mask <<= 1 ) {
            BSONElement lll = ll.next();
            BSONElement rrr = **rr2;
            ++rr2;
            int x = lll.woCompare( rrr, false );
            if ( o.descending( mask ) )
                x = -x;
            if ( x != 0 )
                return x;
            if ( !*inc ) {
                return -direction;
            }
            ++inc;
        }
        return 0;
    }

    template <class BtreeLayout>
    bool BtreeLogic<BtreeLayout>::exists(const KeyDataType& key) const {
        int position = 0;

        // Find the DiskLoc 
        bool found;
        DiskLoc bucket = locate(getRootLoc(), key, &position, &found, minDiskLoc, 1);

        while (!bucket.isNull()) {
            FullKey fullKey = getFullKey(getBucket(bucket), position);
            if (fullKey.header.isUsed()) {
                return fullKey.data.woEqual(key);
            }
            bucket = advance(bucket, &position, 1);
        }

        return false;
    }

    template <class BtreeLayout>
    Status BtreeLogic<BtreeLayout>::dupKeyCheck(const BSONObj& key, const DiskLoc& loc) const {
        KeyDataOwnedType theKey(key);
        if (!wouldCreateDup(theKey, loc)) {
            return Status::OK();
        }

        return Status(ErrorCodes::DuplicateKey, dupKeyError(theKey));
    }

    template <class BtreeLayout>
    bool BtreeLogic<BtreeLayout>::wouldCreateDup(const KeyDataType& key,
                                                 const DiskLoc self) const {
        int position;
        bool found;
        DiskLoc posLoc = locate(getRootLoc(), key, &position, &found, minDiskLoc, 1);

        while (!posLoc.isNull()) {
            FullKey fullKey = getFullKey(getBucket(posLoc), position);
            if (fullKey.header.isUsed()) {
                // TODO: we may not need fullKey.data until we know fullKey.header.isUsed() here
                // and elsewhere.
                if (fullKey.data.woEqual(key)) {
                    return fullKey.recordLoc != self;
                }
                break;
            }

            posLoc = advance(posLoc, &position, 1);
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
    Status BtreeLogic<BtreeLayout>::find(BucketType* bucket,
                                          const KeyDataType& key,
                                          const DiskLoc& recordLoc,
                                          bool errorIfDup,
                                          int* keyPositionOut,
                                          bool* foundOut) const {

        // XXX: fix the ctor for DiskLoc56bit so we can just convert w/o assignment operator
        LocType genericRecordLoc;
        genericRecordLoc = recordLoc;

        bool dupsChecked = false;
        // globalIndexCounters->btree(reinterpret_cast<const char*>(bucket));

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
                        if (!dupsChecked) {
                            // This is expensive and we only want to do it once(? -- when would
                            // it happen twice).
                            dupsChecked = true;
                            if (exists(key)) {
                                if (wouldCreateDup(key, genericRecordLoc)) {
                                    return Status(ErrorCodes::DuplicateKey, dupKeyError(key), 11000);
                                }
                                else {
                                    return Status(ErrorCodes::UniqueIndexViolation, "FIXME");
                                }
                            }
                        }
                    }
                    else {
                        if (fullKey.recordLoc == recordLoc) {
                            return Status(ErrorCodes::UniqueIndexViolation, "FIXME");
                        }
                        else {
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
            }
            else if (cmp > 0) {
                low = middle + 1;
            }
            else {
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
    void BtreeLogic<BtreeLayout>::delBucket(TransactionExperiment* trans,
                                            BucketType* bucket,
                                            const DiskLoc bucketLoc) {
        invariant(bucketLoc != getRootLoc());

        BtreeIndexCursor::aboutToDeleteBucket(bucketLoc);

        BucketType* p = getBucket(bucket->parent);
        int parentIdx = indexInParent(bucket, bucketLoc);
        *trans->writing(&childLocForPos(p, parentIdx)) = DiskLoc();
        deallocBucket(bucket, bucketLoc);
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::deallocBucket(BucketType* bucket, const DiskLoc bucketLoc) {
        DurTransaction txn; // XXX
        bucket->n = BtreeLayout::INVALID_N_SENTINEL;
        bucket->parent.Null();
        _recordStore->deleteRecord(&txn, bucketLoc);
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::restorePosition(const BSONObj& savedKey,
                                                  const DiskLoc& savedLoc,
                                                  int direction,
                                                  DiskLoc* bucketLocInOut,
                                                  int* keyOffsetInOut) const {

        // _keyOffset is -1 if the bucket was deleted.  When buckets are deleted the Btree calls
        // a clientcursor function that calls down to all BTree buckets.  Really, this deletion
        // thing should be kept BTree-internal.  This'll go away with finer grained locking: we
        // can hold on to a bucket for as long as we need it.
        if (-1 == *keyOffsetInOut) {
            locate(savedKey, savedLoc, direction, keyOffsetInOut, bucketLocInOut);
            return;
        }

        invariant(*keyOffsetInOut >= 0);

        BucketType* bucket = getBucket(*bucketLocInOut);
        invariant(bucket);
        invariant(BtreeLayout::INVALID_N_SENTINEL != bucket->n);

        if (keyIsAt(savedKey, savedLoc, bucket, *keyOffsetInOut)) {
            skipUnusedKeys(bucketLocInOut, keyOffsetInOut, direction);
            return;
        }

        if (*keyOffsetInOut > 0) {
            (*keyOffsetInOut)--;
            if (keyIsAt(savedKey, savedLoc, bucket, *keyOffsetInOut)) {
                skipUnusedKeys(bucketLocInOut, keyOffsetInOut, direction);
                return;
            }
        }

        locate(savedKey, savedLoc, direction, keyOffsetInOut, bucketLocInOut);
    }

    template <class BtreeLayout>
    bool BtreeLogic<BtreeLayout>::keyIsAt(const BSONObj& savedKey,
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
    void BtreeLogic<BtreeLayout>::delKeyAtPos(TransactionExperiment* trans,
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
                }
                else {
                    if (!mayBalanceWithNeighbors(trans, bucket, bucketLoc)) {
                        // An empty bucket is only allowed as a transient state.  If
                        // there are no neighbors to balance with, we delete ourself.
                        // This condition is only expected in legacy btrees.
                        delBucket(trans, bucket, bucketLoc);
                    }
                }
                return;
            }
            deleteInternalKey(trans, bucket, bucketLoc, p);
            return;
        }

        if (left.isNull()) {
            _delKeyAtPos(bucket, p);
            mayBalanceWithNeighbors(trans, bucket, bucketLoc);
        }
        else {
            deleteInternalKey(trans, bucket, bucketLoc, p);
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
    void BtreeLogic<BtreeLayout>::deleteInternalKey(TransactionExperiment* trans,
                                                    BucketType* bucket,
                                                    const DiskLoc bucketLoc,
                                                    int keypos) {
        DiskLoc lchild = childLocForPos(bucket, keypos);
        DiskLoc rchild = childLocForPos(bucket, keypos + 1);
        invariant(!lchild.isNull() || !rchild.isNull());
        int advanceDirection = lchild.isNull() ? 1 : -1;
        int advanceKeyOfs = keypos;
        DiskLoc advanceLoc = advance(bucketLoc, &advanceKeyOfs, advanceDirection);
        // advanceLoc must be a descentant of thisLoc, because thisLoc has a
        // child in the proper direction and all descendants of thisLoc must be
        // nonempty because they are not the root.
        BucketType* advanceBucket = getBucket(advanceLoc);
         
        if (!childLocForPos(advanceBucket, advanceKeyOfs).isNull()
            || !childLocForPos(advanceBucket, advanceKeyOfs + 1).isNull()) {

            markUnused(bucket, keypos);
            return;
        }

        FullKey kn = getFullKey(advanceBucket, advanceKeyOfs);
        // Because advanceLoc is a descendant of thisLoc, updating thisLoc will
        // not affect packing or keys of advanceLoc and kn will be stable
        // during the following setInternalKey()
        setInternalKey(trans, bucket, bucketLoc, keypos, kn.recordLoc, kn.data,
                       childLocForPos(bucket, keypos),
                       childLocForPos(bucket, keypos + 1));
        delKeyAtPos(trans, btreemod(trans, advanceBucket), advanceLoc, advanceKeyOfs);
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::replaceWithNextChild(TransactionExperiment* trans,
                                                       BucketType* bucket,
                                                       const DiskLoc bucketLoc) {

        invariant(bucket->n == 0 && !bucket->nextChild.isNull() );
        if (bucket->parent.isNull()) {
            invariant(getRootLoc() == bucketLoc);
            _headManager->setHead(bucket->nextChild);
        }
        else {
            BucketType* parentBucket = getBucket(bucket->parent);
            int bucketIndexInParent = indexInParent(bucket, bucketLoc);
            *trans->writing(&childLocForPos(parentBucket, bucketIndexInParent)) =
                bucket->nextChild;
        }

        *trans->writing(&getBucket(bucket->nextChild)->parent) = bucket->parent;
        BtreeIndexCursor::aboutToDeleteBucket(bucketLoc);
        deallocBucket(bucket, bucketLoc);
    }

    template <class BtreeLayout>
    bool BtreeLogic<BtreeLayout>::canMergeChildren(BucketType* bucket,
                                                    const DiskLoc bucketLoc,
                                                    const int leftIndex) {
        invariant(leftIndex >= 0 && leftIndex < bucket->n);

        DiskLoc leftNodeLoc = childLocForPos(bucket, leftIndex);
        DiskLoc rightNodeLoc = childLocForPos(bucket, leftIndex + 1);

        if (leftNodeLoc.isNull() || rightNodeLoc.isNull()) {
            return false;
        }

        int pos = 0;

        BucketType* leftBucket = getBucket(leftNodeLoc);
        BucketType* rightBucket = getBucket(rightNodeLoc);

        int sum = headerSize()
                + packedDataSize(leftBucket, pos)
                + packedDataSize(rightBucket, pos)
                + getFullKey(bucket, leftIndex).data.dataSize()
                + sizeof(KeyHeaderType);

        return sum <= BtreeLayout::BucketSize;
    }

    /**
     * This implementation must respect the meaning and value of lowWaterMark.  Also see comments in
     * splitPos().
     */
    template <class BtreeLayout>
    int BtreeLogic<BtreeLayout>::rebalancedSeparatorPos(BucketType* bucket,
                                                         const DiskLoc bucketLoc,
                                                         int leftIndex) {
        int split = -1;
        int rightSize = 0;
        const BucketType* l = childForPos(bucket, leftIndex);
        const BucketType* r = childForPos(bucket, leftIndex + 1);

        int KNS = sizeof(KeyHeaderType);
        int rightSizeLimit = ( l->topSize
                             + l->n * KNS
                             + getFullKey(bucket, leftIndex).data.dataSize()
                             + KNS
                             + r->topSize
                             + r->n * KNS ) / 2;

        // This constraint should be ensured by only calling this function
        // if we go below the low water mark.
        invariant(rightSizeLimit < bodySize());

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
        }
        else if (split > l->n + 1 + r->n - 2) {
            split = l->n + 1 + r->n - 2;
        }

        return split;
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::doMergeChildren(TransactionExperiment* trans,
                                                  BucketType* bucket,
                                                  const DiskLoc bucketLoc,
                                                  int leftIndex) {

        DiskLoc leftNodeLoc = childLocForPos(bucket, leftIndex);
        DiskLoc rightNodeLoc = childLocForPos(bucket, leftIndex + 1);
        BucketType* l = btreemod(trans, getBucket(leftNodeLoc));
        BucketType* r = btreemod(trans, getBucket(rightNodeLoc));

        int pos = 0;
        _packReadyForMod(l, pos);
        _packReadyForMod(r, pos);

        // We know the additional keys below will fit in l because canMergeChildren() must be true.
        int oldLNum = l->n;
        // left child's right child becomes old parent key's left child
        FullKey knLeft = getFullKey(bucket, leftIndex);
        pushBack(l, knLeft.recordLoc, knLeft.data, l->nextChild);

        for (int i = 0; i < r->n; ++i) {
            FullKey kn = getFullKey(r, i);
            pushBack(l, kn.recordLoc, kn.data, kn.prevChildBucket);
        }

        l->nextChild = r->nextChild;
        fixParentPtrs(trans, l, leftNodeLoc, oldLNum);
        delBucket(trans, r, rightNodeLoc);

        childLocForPos(bucket, leftIndex + 1) = leftNodeLoc;
        childLocForPos(bucket, leftIndex) = DiskLoc();
        _delKeyAtPos(bucket, leftIndex, true);

        if (bucket->n == 0) {
            // Will trash bucket and bucketLoc.
            //
            // TODO To ensure all leaves are of equal height, we should ensure this is only called
            // on the root.
            replaceWithNextChild(trans, bucket, bucketLoc);
        }
        else {
            mayBalanceWithNeighbors(trans, bucket, bucketLoc);
        }
    }

    template <class BtreeLayout>
    int BtreeLogic<BtreeLayout>::indexInParent(BucketType* bucket,
                                                const DiskLoc bucketLoc) const {
        invariant(!bucket->parent.isNull());
        const BucketType* p = getBucket(bucket->parent);
        if (p->nextChild == bucketLoc) {
            return p->n;
        }

        for (int i = 0; i < p->n; ++i) {
            if (getKeyHeader(p, i).prevChildBucket == bucketLoc) {
                return i;
            }
        }

        out() << "ERROR: can't find ref to child bucket.\n";
        out() << "child: " << bucketLoc << "\n";
        //dump();
        out() << "Parent: " << bucket->parent << "\n";
        //p->dump();
        invariant(false);
        return -1; // just to compile
    }

    template <class BtreeLayout>
    bool BtreeLogic<BtreeLayout>::tryBalanceChildren(TransactionExperiment* trans,
                                                     BucketType* bucket,
                                                     const DiskLoc bucketLoc,
                                                     int leftIndex) {

        // If we can merge, then we must merge rather than balance to preserve bucket utilization
        // constraints.
        if (canMergeChildren(bucket, bucketLoc, leftIndex)) {
            return false;
        }

        doBalanceChildren(trans, btreemod(trans, bucket), bucketLoc, leftIndex);
        return true;
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::doBalanceLeftToRight(TransactionExperiment* trans,
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

        fixParentPtrs(trans, r, rchild, 0, rAdd - 1);

        FullKey kn = getFullKey(l, split);
        l->nextChild = kn.prevChildBucket;

        // Because lchild is a descendant of thisLoc, updating thisLoc will not affect packing or
        // keys of lchild and kn will be stable during the following setInternalKey()            
        setInternalKey(trans, bucket, bucketLoc, leftIndex, kn.recordLoc, kn.data, lchild, rchild);

        // lchild and rchild cannot be merged, so there must be >0 (actually more) keys to the left
        // of split.
        int zeropos = 0;
        truncateTo(l, split, zeropos);
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::doBalanceRightToLeft(TransactionExperiment* trans,
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
            pushBack(l, kn.recordLoc, kn.data, l->nextChild);
        }

        for (int i = 0; i < split - lN - 1; ++i) {
            FullKey kn = getFullKey(r, i);
            pushBack(l, kn.recordLoc, kn.data, kn.prevChildBucket);
        }

        {
            FullKey kn = getFullKey(r, split - lN - 1);
            l->nextChild = kn.prevChildBucket;
            // Child lN was lchild's old nextChild, and don't need to fix that one.
            fixParentPtrs(trans, l, lchild, lN + 1, l->n);
            // Because rchild is a descendant of thisLoc, updating thisLoc will
            // not affect packing or keys of rchild and kn will be stable
            // during the following setInternalKey()
            setInternalKey(trans, bucket, bucketLoc, leftIndex, kn.recordLoc, kn.data, lchild, rchild);
        }

        // lchild and rchild cannot be merged, so there must be >0 (actually more)
        // keys to the right of split.
        int zeropos = 0;
        dropFront(r, split - lN, zeropos);
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::doBalanceChildren(TransactionExperiment* trans,
                                                    BucketType* bucket,
                                                    const DiskLoc bucketLoc,
                                                    int leftIndex) {

        DiskLoc lchild = childLocForPos(bucket, leftIndex);
        DiskLoc rchild = childLocForPos(bucket, leftIndex + 1);

        int zeropos = 0;
        BucketType* l = btreemod(trans, getBucket(lchild));
        _packReadyForMod(l, zeropos);

        BucketType* r = btreemod(trans, getBucket(rchild));
        _packReadyForMod(r, zeropos);

        int split = rebalancedSeparatorPos(bucket, bucketLoc, leftIndex);

        // By definition, if we are below the low water mark and cannot merge
        // then we must actively balance.
        invariant(split != l->n);
        if (split < l->n) {
            doBalanceLeftToRight(trans, bucket, bucketLoc, leftIndex, split, l, lchild, r, rchild);
        }
        else {
            doBalanceRightToLeft(trans, bucket, bucketLoc, leftIndex, split, l, lchild, r, rchild);
        }
    }

    template <class BtreeLayout>
    bool BtreeLogic<BtreeLayout>::mayBalanceWithNeighbors(TransactionExperiment* trans,
                                                          BucketType* bucket,
                                                          const DiskLoc bucketLoc) {
        if (bucket->parent.isNull()) {
            return false;
        }

        if (packedDataSize(bucket, 0) >= lowWaterMark()) {
            return false;
        }

        BucketType* p = getBucket(bucket->parent);
        int parentIdx = indexInParent(bucket, bucketLoc);

        // TODO will missing neighbor case be possible long term?  Should we try to merge/balance
        // somehow in that case if so?
        bool mayBalanceRight = (parentIdx < p->n) && !childLocForPos(p, parentIdx + 1).isNull();
        bool mayBalanceLeft = ( parentIdx > 0 ) && !childLocForPos(p, parentIdx - 1).isNull();

        // Balance if possible on one side - we merge only if absolutely necessary to preserve btree
        // bucket utilization constraints since that's a more heavy duty operation (especially if we
        // must re-split later).
        if (mayBalanceRight && tryBalanceChildren(trans, p, bucket->parent, parentIdx)) {
            return true;
        }

        if (mayBalanceLeft && tryBalanceChildren(trans, p, bucket->parent, parentIdx - 1)) {
            return true;
        }

        BucketType* pm = btreemod(trans, getBucket(bucket->parent));
        if (mayBalanceRight) {
            doMergeChildren(trans, pm, bucket->parent, parentIdx);
            return true;
        }
        else if (mayBalanceLeft) {
            doMergeChildren(trans, pm, bucket->parent, parentIdx - 1);
            return true;
        }

        return false;
    }

    template <class BtreeLayout>
    bool BtreeLogic<BtreeLayout>::unindex(TransactionExperiment* trans,
                                          const BSONObj& key,
                                          const DiskLoc& recordLoc) {
        int pos;
        bool found = false;
        KeyDataOwnedType ownedKey(key);
        DiskLoc loc = locate(getRootLoc(), ownedKey, &pos, &found, recordLoc, 1);
        if (found) {
            BucketType* bucket = btreemod(trans, getBucket(loc));
            delKeyAtPos(trans, bucket, loc, pos);
            assertValid(getRoot(), _ordering);
        }
        return found;
    }

    template <class BtreeLayout>
    bool BtreeLogic<BtreeLayout>::isEmpty() const {
        return getRoot()->n == 0;
    }

    template <class BtreeLayout>
    inline void BtreeLogic<BtreeLayout>::fix(TransactionExperiment* trans,
                                             const DiskLoc bucketLoc,
                                             const DiskLoc child) {
        if (!child.isNull()) {
            *trans->writing(&getBucket(child)->parent) = bucketLoc;
        }
    }

    /**
     * This can cause a lot of additional page writes when we assign buckets to different parents.
     * Maybe get rid of parent ptrs?
     */
    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::fixParentPtrs(TransactionExperiment* trans,
                                                BucketType* bucket,
                                                const DiskLoc bucketLoc,
                                                int firstIndex,
                                                int lastIndex) {

        invariant(getBucket(bucketLoc) == bucket);

        if (lastIndex == -1) {
            lastIndex = bucket->n;
        }

        for (int i = firstIndex; i <= lastIndex; i++) {
            fix(trans, bucketLoc, childLocForPos(bucket, i));
        }
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::setInternalKey(TransactionExperiment* trans,
                                                 BucketType* bucket,
                                                 const DiskLoc bucketLoc,
                                                 int keypos,
                                                 const DiskLoc recordLoc,
                                                 const KeyDataType& key,
                                                 const DiskLoc lchild,
                                                 const DiskLoc rchild) {
        childLocForPos(bucket, keypos).Null();
        // This may leave the bucket empty (n == 0) which is ok only as a transient state.  In the
        // instant case, the implementation of insertHere behaves correctly when n == 0 and as a
        // side effect increments n.
        _delKeyAtPos(bucket, keypos, true);

        // Ensure we do not orphan neighbor's old child.
        invariant(childLocForPos(bucket, keypos ) == rchild);

        // Just set temporarily - required to pass validation in insertHere()
        childLocForPos(bucket, keypos) = lchild;

        insertHere(trans, bucketLoc, keypos, key, recordLoc, lchild, rchild);
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
    void BtreeLogic<BtreeLayout>::insertHere(TransactionExperiment* trans,
                                             const DiskLoc bucketLoc,
                                             int pos,
                                             const KeyDataType& key,
                                             const DiskLoc recordLoc,
                                             const DiskLoc leftChildLoc,
                                             const DiskLoc rightChildLoc) {

        BucketType* bucket = getBucket(bucketLoc);

        if (!basicInsert(trans, bucket, bucketLoc, pos, key, recordLoc)) {
            // If basicInsert() fails, the bucket will be packed as required by split().
            split(trans, btreemod(trans, bucket), bucketLoc, pos, recordLoc, key, leftChildLoc, rightChildLoc);
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
            *trans->writing(&bucket->nextChild) = rightChildLoc;
            if (!rightChildLoc.isNull()) {
                *trans->writing(&getBucket(rightChildLoc)->parent) = bucketLoc;
            }
        }
        else {
            kn->prevChildBucket = leftChildLoc;
            if (getKeyHeader(bucket, pos + 1).prevChildBucket != leftChildLoc) {
                // XXX: log more
                invariant(false);
            }
            const LocType *pc = &getKeyHeader(bucket, pos + 1).prevChildBucket;
            // Intent declared in basicInsert()
            *const_cast<LocType*>(pc) = rightChildLoc;
            if (!rightChildLoc.isNull()) {
                *trans->writing(&getBucket(rightChildLoc)->parent) = bucketLoc;
            }
        }
    }

    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::split(TransactionExperiment* trans,
                                        BucketType* bucket,
                                        const DiskLoc bucketLoc,
                                        int keypos,
                                        const DiskLoc recordLoc,
                                        const KeyDataType& key,
                                        const DiskLoc lchild,
                                        const DiskLoc rchild) {

        int split = splitPos(bucket, keypos);
        DiskLoc rLoc = addBucket(trans);
        BucketType* r = btreemod(trans, getBucket(rLoc));

        for (int i = split + 1; i < bucket->n; i++) {
            FullKey kn = getFullKey(bucket, i);
            pushBack(r, kn.recordLoc, kn.data, kn.prevChildBucket);
        }
        r->nextChild = bucket->nextChild;
        assertValid(r, _ordering);

        r = NULL;
        fixParentPtrs(trans, getBucket(rLoc), rLoc);

        FullKey splitkey = getFullKey(bucket, split);
        // splitkey key gets promoted, its children will be thisLoc (l) and rLoc (r)
        bucket->nextChild = splitkey.prevChildBucket;

        // Because thisLoc is a descendant of parent, updating parent will not affect packing or
        // keys of thisLoc and splitkey will be stable during the following:

        if (bucket->parent.isNull()) {
            // promote splitkey to a parent this->node make a new parent if we were the root
            DiskLoc L = addBucket(trans);
            BucketType* p = btreemod(trans, getBucket(L));
            pushBack(p, splitkey.recordLoc, splitkey.data, bucketLoc);
            p->nextChild = rLoc;
            assertValid(p, _ordering);
            bucket->parent = L;
            _headManager->setHead(L);
            *trans->writing(&getBucket(rLoc)->parent) = bucket->parent;
        }
        else {
            // set this before calling _insert - if it splits it will do fixParent() logic and
            // change the value.
            *trans->writing(&getBucket(rLoc)->parent) = bucket->parent;
            _insert(trans,
                    getBucket(bucket->parent),
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
            insertHere(trans, bucketLoc, newpos, key, recordLoc, lchild, rchild);
        }
        else {
            int kp = keypos - split - 1;
            invariant(kp >= 0);
            insertHere(trans, rLoc, kp, key, recordLoc, lchild, rchild);
        }
    }

    class DummyDocWriter : public DocWriter {
    public:
        DummyDocWriter(size_t sz) : _sz(sz) { }
        virtual void writeDocument(char* buf) const { /* no-op */ }
        virtual size_t documentSize() const { return _sz; }
    private:
        size_t _sz;
    };

    template <class BtreeLayout>
    Status BtreeLogic<BtreeLayout>::initAsEmpty(TransactionExperiment* trans) {
        if (!_headManager->getHead().isNull()) {
            return Status(ErrorCodes::InternalError, "index already initialized");
        }

        _headManager->setHead(addBucket(trans));
        return Status::OK();
    }

    template <class BtreeLayout>
    DiskLoc BtreeLogic<BtreeLayout>::addBucket(TransactionExperiment* trans) {
        DummyDocWriter docWriter(BtreeLayout::BucketSize);
        StatusWith<DiskLoc> loc = _recordStore->insertRecord(trans, &docWriter, 0);
        // XXX: remove this(?) or turn into massert or sanely bubble it back up.
        uassertStatusOK(loc.getStatus());
        BucketType* b = btreemod(trans, getBucket(loc.getValue()));
        init(b);
        return loc.getValue();
    }

    // static
    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::dump(BucketType* bucket, int depth) {
        log() << "BUCKET n:" << bucket->n;
        log() << " parent:" << hex << bucket->parent.getOfs() << dec;

        string indent = string(depth, ' ');

        for (int i = 0; i < bucket->n; i++) {
            log() << '\n' << indent;
            FullKey k = getFullKey(bucket, i);
            string ks = k.data.toString();
            log() << "  " << hex << k.prevChildBucket.getOfs() << "<-- prevChildBucket for " << i << '\n';
            log() << indent << "    " << i << ' ' << ks.substr(0, 30)
              << " Loc:" << k.recordLoc.toString() << dec;
            if (getKeyHeader(bucket, i).isUnused()) {
                log() << " UNUSED";
            }
        }

        log() << "\n" << indent << "  " << hex << bucket->nextChild.getOfs() << dec << endl;
    }

    template <class BtreeLayout>
    DiskLoc BtreeLogic<BtreeLayout>::getDiskLoc(const DiskLoc& bucketLoc, const int keyOffset) {
        invariant(!bucketLoc.isNull());
        BucketType* bucket = getBucket(bucketLoc);
        return getKeyHeader(bucket, keyOffset).recordLoc;
    }

    template <class BtreeLayout>
    BSONObj BtreeLogic<BtreeLayout>::getKey(const DiskLoc& bucketLoc, const int keyOffset) {
        invariant(!bucketLoc.isNull());
        BucketType* bucket = getBucket(bucketLoc);
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
        }
        else {
            return getFullKey(bucket, keyOffset).data.toBson();
        }
    }

    template <class BtreeLayout>
    long long BtreeLogic<BtreeLayout>::fullValidate(long long *unusedCount,
                                                     bool strict,
                                                     bool dumpBuckets,
                                                     unsigned depth) {
        return fullValidate(getRootLoc(), unusedCount, strict, dumpBuckets, depth);
    }

    template <class BtreeLayout>
    long long BtreeLogic<BtreeLayout>::fullValidate(const DiskLoc bucketLoc,
                                                     long long *unusedCount,
                                                     bool strict,
                                                     bool dumpBuckets,
                                                     unsigned depth) {
        BucketType* bucket = getBucket(bucketLoc);
        assertValid(bucket, _ordering, true);

        if (dumpBuckets) {
            log() << bucketLoc.toString() << ' ';
            dump(bucket, depth);
        }

        long long keyCount = 0;

        for (int i = 0; i < bucket->n; i++) {
            KeyHeaderType& kn = getKeyHeader(bucket, i);

            if (kn.isUsed()) {
                keyCount++;
            }
            else if (NULL != unusedCount) {
                ++(*unusedCount);
            }

            if (!kn.prevChildBucket.isNull()) {
                DiskLoc left = kn.prevChildBucket;
                BucketType* b = getBucket(left);

                if (strict) {
                    invariant(b->parent == bucketLoc);
                }
                else {
                    wassert(b->parent == bucketLoc);
                }

                keyCount += fullValidate(left, unusedCount, strict, dumpBuckets, depth + 1);
            }
        }

        if (!bucket->nextChild.isNull()) {
            BucketType* b = getBucket(bucket->nextChild);
            if (strict) {
                invariant(b->parent == bucketLoc);
            }
            else {
                wassert(b->parent == bucketLoc);
            }

            keyCount += fullValidate(bucket->nextChild, unusedCount, strict, dumpBuckets, depth + 1);
        }

        return keyCount;
    }

    // XXX: remove this(?)  used to not dump every key in assertValid.
    int nDumped = 0;

    // static
    template <class BtreeLayout>
    void BtreeLogic<BtreeLayout>::assertValid(BucketType* bucket,
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
                    out() << "ERROR: btree key order corrupt.  Keys:" << endl;
                    if (++nDumped < 5) {
                        for (int j = 0; j < bucket->n; j++) {
                            out() << "  " << getFullKey(bucket, j).data.toString() << endl;
                        }
                        dump(bucket);
                    }
                    wassert(false);
                    break;
                }
                else if (z == 0) {
                    if (!(firstKey.header.recordLoc < secondKey.header.recordLoc)) {
                        out() << "ERROR: btree key order corrupt (recordlocs wrong):" << endl;
                        out() << " k(" << i << ")" << firstKey.data.toString()
                              << " RL:" << firstKey.header.recordLoc.toString() << endl;
                        out() << " k(" << i + 1 << ")" << secondKey.data.toString()
                              << " RL:" << secondKey.header.recordLoc.toString() << endl;
                        wassert(firstKey.header.recordLoc < secondKey.header.recordLoc);
                    }
                }
            }
        }
        else {
            //faster:
            if (bucket->n > 1) {
                FullKey k1 = getFullKey(bucket, 0);
                FullKey k2 = getFullKey(bucket, bucket->n - 1);
                int z = k1.data.woCompare(k2.data, ordering);
                //wassert( z <= 0 );
                if (z > 0) {
                    problem() << "btree keys out of order" << '\n';
                    ONCE {
                        dump(bucket);
                    }
                    invariant(false);
                }
            }
        }
    }

    template <class BtreeLayout>
    Status BtreeLogic<BtreeLayout>::insert(TransactionExperiment* trans,
                                           const BSONObj& rawKey,
                                           const DiskLoc& value,
                                           bool dupsAllowed) {
        KeyDataOwnedType key(rawKey);

        if (key.dataSize() > BtreeLayout::KeyMax) {
            string msg = str::stream() << "Btree::insert: key too large to index, failing "
                                       << _indexName << ' '
                                       << key.dataSize() << ' ' << key.toString();
            return Status(ErrorCodes::KeyTooLong, msg);
        }

        Status status = _insert(trans,
                                getRoot(),
                                getRootLoc(),
                                key,
                                value,
                                dupsAllowed,
                                DiskLoc(),
                                DiskLoc());

        assertValid(getRoot(), _ordering);
        return status;
    }

    template <class BtreeLayout>
    Status BtreeLogic<BtreeLayout>::_insert(TransactionExperiment* trans,
                                            BucketType* bucket,
                                            const DiskLoc bucketLoc,
                                            const KeyDataType& key,
                                            const DiskLoc recordLoc,
                                            bool dupsAllowed,
                                            const DiskLoc leftChild,
                                            const DiskLoc rightChild) {
        invariant( key.dataSize() > 0 );

        int pos;
        bool found;
        Status findStatus = find(bucket, key, recordLoc, !dupsAllowed, &pos, &found);
        if (!findStatus.isOK()) {
            return findStatus;
        }

        if (found) {
            static KeyHeaderType& header = getKeyHeader(bucket, pos);
            if (header.isUnused()) {
                LOG(4) << "btree _insert: reusing unused key" << endl;
                massert(17433, "_insert: reuse key but lchild is not null", leftChild.isNull());
                massert(17434, "_insert: reuse key but rchild is not null", rightChild.isNull());
                trans->writing(&header)->setUsed();
                return Status::OK();
            }
            return Status(ErrorCodes::UniqueIndexViolation, "FIXME");
        }

        DiskLoc childLoc = childLocForPos(bucket, pos);

        // In current usage, rightChild is NULL for a new key and is not NULL when we are
        // promoting a split key.  These are the only two cases where _insert() is called
        // currently.
        if (childLoc.isNull() || !rightChild.isNull()) {
            insertHere(trans, bucketLoc, pos, key, recordLoc, leftChild, rightChild);
            return Status::OK();
        }
        else {
            return _insert(trans,
                           getBucket(childLoc),
                           childLoc,
                           key,
                           recordLoc,
                           dupsAllowed,
                           DiskLoc(),
                           DiskLoc());
        }
    }

    template <class BtreeLayout>
    DiskLoc BtreeLogic<BtreeLayout>::advance(const DiskLoc& bucketLoc,
                                              int* posInOut,
                                              int direction) const {
        BucketType* bucket = getBucket(bucketLoc);

        if (*posInOut < 0 || *posInOut >= bucket->n ) {
            out() << "ASSERT failure advancing btree bucket" << endl;
            out() << "  thisLoc: " << bucketLoc.toString() << endl;
            out() << "  keyOfs: " << *posInOut << " n:" << bucket->n << " direction: " << direction << endl;
            // out() << bucketSummary() << endl;
            invariant(false);
        }

        // XXX document
        int adj = direction < 0 ? 1 : 0;
        int ko = *posInOut + direction;

        // Look down if we need to.
        DiskLoc nextDownLoc = childLocForPos(bucket, ko + adj);
        BucketType* nextDown = getBucket(nextDownLoc);
        if (NULL != nextDown) {
            for (;;) {
                if (direction > 0) {
                    *posInOut = 0;
                }
                else {
                    *posInOut = nextDown->n - 1;
                }
                DiskLoc newNextDownLoc = childLocForPos(nextDown, *posInOut + adj);
                BucketType* newNextDownBucket = getBucket(newNextDownLoc);
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
        DiskLoc ancestor = getBucket(bucketLoc)->parent;
        for (;;) {
            if (ancestor.isNull()) {
                break;
            }
            BucketType* an = getBucket(ancestor);
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
    bool BtreeLogic<BtreeLayout>::keyIsUsed(const DiskLoc& loc, const int& pos) const {
        return getKeyHeader(getBucket(loc), pos).isUsed();
    }

    template <class BtreeLayout>
    bool BtreeLogic<BtreeLayout>::locate(const BSONObj& key,
                                         const DiskLoc& recordLoc,
                                         const int direction,
                                         int* posOut,
                                         DiskLoc* bucketLocOut) const {
        // Clear out any data.
        *posOut = 0;
        *bucketLocOut = DiskLoc();

        bool found = false;
        KeyDataOwnedType owned(key);

        *bucketLocOut = locate(getRootLoc(), owned, posOut, &found, recordLoc, direction);

        if (!found) {
            return false;
        }

        skipUnusedKeys(bucketLocOut, posOut, direction);

        return found;
    }

    template <class BtreeLayout>
    DiskLoc BtreeLogic<BtreeLayout>::locate(const DiskLoc& bucketLoc,
                                            const KeyDataType& key,
                                            int* posOut,
                                            bool* foundOut,
                                            const DiskLoc& recordLoc,
                                            const int direction) const {
        int position;
        BucketType* bucket = getBucket(bucketLoc);
        // XXX: owned to not owned conversion(?)
        find(bucket, key, recordLoc, false, &position, foundOut);

        // Look in our current bucket.
        if (*foundOut) {
            *posOut = position;
            return bucketLoc;
        }

        // Not in our current bucket.  'position' tells us where there may be a child.
        DiskLoc childLoc = childLocForPos(bucket, position);

        if (!childLoc.isNull()) {
            DiskLoc inChild = locate(childLoc, key, posOut, foundOut, recordLoc, direction);
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
            }
            else {
                return bucketLoc;
            }
        }
        else {
            // The key would go to our right...
            if (bucket->n == *posOut) {
                return DiskLoc();
            }
            else {
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
    typename BtreeLogic<BtreeLayout>::BucketType*
    BtreeLogic<BtreeLayout>::getBucket(const DiskLoc dl) const {
        if (dl.isNull()) {
            return NULL;
        }

        Record* record = _recordStore->recordFor(dl);
        return reinterpret_cast<BucketType*>(record->dataNoThrowing());
    }

    template <class BtreeLayout>
    typename BtreeLogic<BtreeLayout>::BucketType*
    BtreeLogic<BtreeLayout>::getRoot() const {
        return getBucket(_headManager->getHead());
    }

    template <class BtreeLayout>
    DiskLoc
    BtreeLogic<BtreeLayout>::getRootLoc() const {
        return _headManager->getHead();
    }

    template <class BtreeLayout>
    typename BtreeLogic<BtreeLayout>::BucketType*
    BtreeLogic<BtreeLayout>::childForPos(BucketType* bucket, int pos) const {
        return getBucket(childLocForPos(bucket, pos));
    }

    template <class BtreeLayout>
    typename BtreeLogic<BtreeLayout>::LocType&
    BtreeLogic<BtreeLayout>::childLocForPos(BucketType* bucket, int pos) {
        if (bucket->n == pos) {
            return bucket->nextChild;
        }
        else {
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
