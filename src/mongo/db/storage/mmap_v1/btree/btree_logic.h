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

#include <string>

#include "mongo/db/catalog/head_manager.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/mmap_v1/btree/btree_ondisk.h"
#include "mongo/db/storage/mmap_v1/btree/key.h"
#include "mongo/db/storage/mmap_v1/diskloc.h"

namespace mongo {

class PseudoRandom;
class RecordStore;
class SavedCursorRegistry;

// Used for unit-testing only
template <class BtreeLayout>
class BtreeLogicTestBase;
template <class BtreeLayout>
class ArtificialTreeBuilder;

/**
 * This is the logic for manipulating the Btree.  It is (mostly) independent of the on-disk
 * format.
 */
template <class BtreeLayout>
class BtreeLogic {
public:
    // AKA _keyNode
    typedef typename BtreeLayout::FixedWidthKeyType KeyHeaderType;

    // AKA Key
    typedef typename BtreeLayout::KeyType KeyDataType;

    // AKA KeyOwned
    typedef typename BtreeLayout::KeyOwnedType KeyDataOwnedType;

    // AKA Loc
    typedef typename BtreeLayout::LocType LocType;

    // AKA BucketBasics or BtreeBucket, either one.
    typedef typename BtreeLayout::BucketType BucketType;

    /**
     * 'head' manages the catalog information.
     * 'store' allocates and frees buckets.
     * 'ordering' is meta-information we store in the catalog.
     * 'indexName' is a string identifying the index that we use to print errors with.
     */
    BtreeLogic(HeadManager* head,
               RecordStore* store,
               SavedCursorRegistry* cursors,
               const Ordering& ordering,
               const std::string& indexName,
               bool isUnique)
        : _headManager(head),
          _recordStore(store),
          _cursorRegistry(cursors),
          _ordering(ordering),
          _indexName(indexName),
          _isUnique(isUnique) {}

    //
    // Public-facing
    //

    class Builder {
    public:
        typedef typename BtreeLayout::KeyOwnedType KeyDataOwnedType;
        typedef typename BtreeLayout::KeyType KeyDataType;

        Status addKey(const BSONObj& key, const DiskLoc& loc);

    private:
        friend class BtreeLogic;

        class SetRightLeafLocChange;

        Builder(BtreeLogic* logic, OperationContext* txn, bool dupsAllowed);

        /**
         * Creates and returns a new empty bucket to the right of leftSib, maintaining the
         * internal consistency of the tree. leftSib must be the right-most child of its parent
         * or it must be the root.
         */
        DiskLoc newBucket(BucketType* leftSib, DiskLoc leftSibLoc);

        BucketType* _getModifiableBucket(DiskLoc loc);
        BucketType* _getBucket(DiskLoc loc);

        // Not owned.
        BtreeLogic* _logic;

        DiskLoc _rightLeafLoc;  // DiskLoc of right-most (highest) leaf bucket.
        bool _dupsAllowed;
        std::unique_ptr<KeyDataOwnedType> _keyLast;

        // Not owned.
        OperationContext* _txn;
    };

    /**
     * Caller owns the returned pointer.
     * 'this' must outlive the returned pointer.
     */
    Builder* newBuilder(OperationContext* txn, bool dupsAllowed);

    Status dupKeyCheck(OperationContext* txn, const BSONObj& key, const DiskLoc& loc) const;

    Status insert(OperationContext* txn,
                  const BSONObj& rawKey,
                  const DiskLoc& value,
                  bool dupsAllowed);

    /**
     * Navigates down the tree and locates the bucket and position containing a record with
     * the specified <key, recordLoc> combination.
     *
     * @return true if the exact <key, recordLoc> was found. Otherwise, false and the
     *      bucketLocOut would contain the bucket containing key which is before or after the
     *      searched one (dependent on the direction).
     */
    bool locate(OperationContext* txn,
                const BSONObj& key,
                const DiskLoc& recordLoc,
                const int direction,
                int* posOut,
                DiskLoc* bucketLocOut) const;

    void advance(OperationContext* txn,
                 DiskLoc* bucketLocInOut,
                 int* posInOut,
                 int direction) const;

    bool exists(OperationContext* txn, const KeyDataType& key) const;

    bool unindex(OperationContext* txn, const BSONObj& key, const DiskLoc& recordLoc);

    bool isEmpty(OperationContext* txn) const;

    long long fullValidate(OperationContext*,
                           long long* unusedCount,
                           bool strict,
                           bool dumpBuckets,
                           unsigned depth) const;

    DiskLoc getDiskLoc(OperationContext* txn, const DiskLoc& bucketLoc, const int keyOffset) const;

    BSONObj getKey(OperationContext* txn, const DiskLoc& bucketLoc, const int keyOffset) const;

    /**
     * Returns a pseudo-random element from the tree. It is an error to call this method if the tree
     * is empty.
     */
    IndexKeyEntry getRandomEntry(OperationContext* txn) const;

    DiskLoc getHead(OperationContext* txn) const {
        return DiskLoc::fromRecordId(_headManager->getHead(txn));
    }

    Status touch(OperationContext* txn) const;

    //
    // Composite key navigation methods
    //

    void customLocate(OperationContext* txn,
                      DiskLoc* locInOut,
                      int* keyOfsInOut,
                      const IndexSeekPoint& seekPoint,
                      int direction) const;

    void advanceTo(OperationContext*,
                   DiskLoc* thisLocInOut,
                   int* keyOfsInOut,
                   const IndexSeekPoint& seekPoint,
                   int direction) const;

    void restorePosition(OperationContext* txn,
                         const BSONObj& savedKey,
                         const DiskLoc& savedLoc,
                         int direction,
                         DiskLoc* bucketInOut,
                         int* keyOffsetInOut) const;

    //
    // Creation and deletion
    //

    /**
     * Returns OK if the index was uninitialized before, error status otherwise.
     */
    Status initAsEmpty(OperationContext* txn);

    //
    // Size constants
    //

    const RecordStore* getRecordStore() const {
        return _recordStore;
    }

    SavedCursorRegistry* savedCursors() const {
        return _cursorRegistry;
    }

    static int lowWaterMark();

    Ordering ordering() const {
        return _ordering;
    }

    int customBSONCmp(const BSONObj& inIndex_left,
                      const IndexSeekPoint& seekPoint_right,
                      int direction) const;

    bool isUnique() const {
        return _isUnique;
    }

private:
    friend class BtreeLogic::Builder;

    // Used for unit-testing only
    friend class BtreeLogicTestBase<BtreeLayout>;
    friend class ArtificialTreeBuilder<BtreeLayout>;

    /**
     * This is an in memory wrapper for the variable length data associated with a
     * KeyHeaderType.  It points to on-disk data but is not itself on-disk data.
     *
     * This object and its BSONObj 'key' will become invalid if the KeyHeaderType data that owns
     * this it is moved within the btree.  In general, a KeyWrapper should not be expected to be
     * valid after a write.
     */
    struct FullKey {
        FullKey(const BucketType* bucket, int i)
            : header(getKeyHeader(bucket, i)),
              prevChildBucket(header.prevChildBucket),
              recordLoc(header.recordLoc),
              data(bucket->data + header.keyDataOfs()) {}

        // This is actually a reference to something on-disk.
        const KeyHeaderType& header;

        // These are actually in 'header'.
        const LocType& prevChildBucket;
        const LocType& recordLoc;

        // This is *not* memory-mapped but its members point to something on-disk.
        KeyDataType data;
    };

    //
    // Functions that depend on the templated type info but nothing in 'this'.
    //

    static LocType& childLocForPos(BucketType* bucket, int pos);

    static FullKey getFullKey(const BucketType* bucket, int i);

    static KeyHeaderType& getKeyHeader(BucketType* bucket, int i);

    static const KeyHeaderType& getKeyHeader(const BucketType* bucket, int i);

    static char* dataAt(BucketType* bucket, short ofs);

    static void markUnused(BucketType* bucket, int keypos);

    static int totalDataSize(BucketType* bucket);

    static void init(BucketType* bucket);

    static int _alloc(BucketType* bucket, int bytes);

    static void _unalloc(BucketType* bucket, int bytes);

    static void _delKeyAtPos(BucketType* bucket, int keypos, bool mayEmpty = false);

    static void popBack(BucketType* bucket, DiskLoc* recordLocOut, KeyDataType* keyDataOut);

    static bool mayDropKey(BucketType* bucket, int index, int refPos);

    static int _packedDataSize(BucketType* bucket, int refPos);

    static void setPacked(BucketType* bucket);

    static void setNotPacked(BucketType* bucket);

    static BucketType* btreemod(OperationContext* txn, BucketType* bucket);

    static int splitPos(BucketType* bucket, int keypos);

    static void reserveKeysFront(BucketType* bucket, int nAdd);

    static void setKey(BucketType* bucket,
                       int i,
                       const DiskLoc recordLoc,
                       const KeyDataType& key,
                       const DiskLoc prevChildBucket);

    static bool isHead(BucketType* bucket);

    static void dumpBucket(const BucketType* bucket, int indentLength = 0);

    static void assertValid(const std::string& ns,
                            BucketType* bucket,
                            const Ordering& ordering,
                            bool force = false);

    //
    // 'this'-specific helpers (require record store, catalog information, or ordering, or type
    // information).
    //

    bool basicInsert(OperationContext* txn,
                     BucketType* bucket,
                     const DiskLoc bucketLoc,
                     int& keypos,
                     const KeyDataType& key,
                     const DiskLoc recordLoc);

    void dropFront(BucketType* bucket, int nDrop, int& refpos);

    void _pack(OperationContext* txn, BucketType* bucket, const DiskLoc thisLoc, int& refPos);

    void customLocate(OperationContext* txn,
                      DiskLoc* locInOut,
                      int* keyOfsInOut,
                      const IndexSeekPoint& seekPoint,
                      int direction,
                      std::pair<DiskLoc, int>& bestParent) const;

    Status _find(OperationContext* txn,
                 BucketType* bucket,
                 const KeyDataType& key,
                 const DiskLoc& recordLoc,
                 bool errorIfDup,
                 int* keyPositionOut,
                 bool* foundOut) const;

    bool customFind(OperationContext* txn,
                    int low,
                    int high,
                    const IndexSeekPoint& seekPoint,
                    int direction,
                    DiskLoc* thisLocInOut,
                    int* keyOfsInOut,
                    std::pair<DiskLoc, int>& bestParent) const;

    void advanceToImpl(OperationContext* txn,
                       DiskLoc* thisLocInOut,
                       int* keyOfsInOut,
                       const IndexSeekPoint& seekPoint,
                       int direction) const;

    bool wouldCreateDup(OperationContext* txn, const KeyDataType& key, const DiskLoc self) const;

    bool keyIsUsed(OperationContext* txn, const DiskLoc& loc, const int& pos) const;

    void skipUnusedKeys(OperationContext* txn, DiskLoc* loc, int* pos, int direction) const;

    DiskLoc advance(OperationContext* txn,
                    const DiskLoc& bucketLoc,
                    int* posInOut,
                    int direction) const;

    DiskLoc _locate(OperationContext* txn,
                    const DiskLoc& bucketLoc,
                    const KeyDataType& key,
                    int* posOut,
                    bool* foundOut,
                    const DiskLoc& recordLoc,
                    const int direction) const;

    long long _fullValidate(OperationContext* txn,
                            const DiskLoc bucketLoc,
                            long long* unusedCount,
                            bool strict,
                            bool dumpBuckets,
                            unsigned depth) const;

    DiskLoc _addBucket(OperationContext* txn);

    bool canMergeChildren(OperationContext* txn,
                          BucketType* bucket,
                          const DiskLoc bucketLoc,
                          const int leftIndex);

    // has to look in children of 'bucket' and requires record store
    int _rebalancedSeparatorPos(OperationContext* txn, BucketType* bucket, int leftIndex);

    void _packReadyForMod(BucketType* bucket, int& refPos);

    void truncateTo(BucketType* bucket, int N, int& refPos);

    void split(OperationContext* txn,
               BucketType* bucket,
               const DiskLoc bucketLoc,
               int keypos,
               const DiskLoc recordLoc,
               const KeyDataType& key,
               const DiskLoc lchild,
               const DiskLoc rchild);

    Status _insert(OperationContext* txn,
                   BucketType* bucket,
                   const DiskLoc bucketLoc,
                   const KeyDataType& key,
                   const DiskLoc recordLoc,
                   bool dupsAllowed,
                   const DiskLoc leftChild,
                   const DiskLoc rightChild);

    // TODO take a BucketType*?
    void insertHere(OperationContext* txn,
                    const DiskLoc bucketLoc,
                    int pos,
                    const KeyDataType& key,
                    const DiskLoc recordLoc,
                    const DiskLoc leftChild,
                    const DiskLoc rightChild);

    std::string dupKeyError(const KeyDataType& key) const;

    void setInternalKey(OperationContext* txn,
                        BucketType* bucket,
                        const DiskLoc bucketLoc,
                        int keypos,
                        const DiskLoc recordLoc,
                        const KeyDataType& key,
                        const DiskLoc lchild,
                        const DiskLoc rchild);

    void fixParentPtrs(OperationContext* trans,
                       BucketType* bucket,
                       const DiskLoc bucketLoc,
                       int firstIndex = 0,
                       int lastIndex = -1);

    bool mayBalanceWithNeighbors(OperationContext* txn,
                                 BucketType* bucket,
                                 const DiskLoc bucketLoc);

    void doBalanceChildren(OperationContext* txn,
                           BucketType* bucket,
                           const DiskLoc bucketLoc,
                           int leftIndex);

    void doBalanceLeftToRight(OperationContext* txn,
                              BucketType* bucket,
                              const DiskLoc thisLoc,
                              int leftIndex,
                              int split,
                              BucketType* l,
                              const DiskLoc lchild,
                              BucketType* r,
                              const DiskLoc rchild);

    void doBalanceRightToLeft(OperationContext* txn,
                              BucketType* bucket,
                              const DiskLoc thisLoc,
                              int leftIndex,
                              int split,
                              BucketType* l,
                              const DiskLoc lchild,
                              BucketType* r,
                              const DiskLoc rchild);

    bool tryBalanceChildren(OperationContext* txn,
                            BucketType* bucket,
                            const DiskLoc bucketLoc,
                            int leftIndex);

    int indexInParent(OperationContext* txn, BucketType* bucket, const DiskLoc bucketLoc) const;

    void doMergeChildren(OperationContext* txn,
                         BucketType* bucket,
                         const DiskLoc bucketLoc,
                         int leftIndex);

    void replaceWithNextChild(OperationContext* txn, BucketType* bucket, const DiskLoc bucketLoc);

    void deleteInternalKey(OperationContext* txn,
                           BucketType* bucket,
                           const DiskLoc bucketLoc,
                           int keypos);

    void delKeyAtPos(OperationContext* txn, BucketType* bucket, const DiskLoc bucketLoc, int p);

    void delBucket(OperationContext* txn, BucketType* bucket, const DiskLoc bucketLoc);

    void deallocBucket(OperationContext* txn, BucketType* bucket, const DiskLoc bucketLoc);

    bool _keyIsAt(const BSONObj& savedKey,
                  const DiskLoc& savedLoc,
                  BucketType* bucket,
                  int keyPos) const;

    /**
     * Tries to push key into bucket. Return false if it can't because key doesn't fit.
     *
     * bucket must be declared as writable by the caller.
     * The new key/recordLoc pair must be higher than any others in bucket.
     *
     * TODO needs 'this' for _ordering for sanity check
     */
    bool pushBack(BucketType* bucket,
                  const DiskLoc recordLoc,
                  const KeyDataType& key,
                  const DiskLoc prevChild);


    BucketType* childForPos(OperationContext* txn, BucketType* bucket, int pos) const;

    BucketType* getBucket(OperationContext* txn, const DiskLoc dl) const {
        return getBucket(txn, dl.toRecordId());
    }
    BucketType* getBucket(OperationContext* txn, const RecordId dl) const;

    BucketType* getRoot(OperationContext* txn) const;

    DiskLoc getRootLoc(OperationContext* txn) const;

    void recordRandomWalk(OperationContext* txn,
                          PseudoRandom* prng,
                          BucketType* curBucket,
                          int64_t nBucketsInCurrentLevel,
                          std::vector<int64_t>* nKeysInLevel,
                          std::vector<FullKey>* selectedKeys) const;

    //
    // Data
    //

    // Not owned here.
    HeadManager* _headManager;

    // Not owned here.
    RecordStore* _recordStore;

    // Not owned Here.
    SavedCursorRegistry* _cursorRegistry;

    Ordering _ordering;

    std::string _indexName;

    // True if this is a unique index, i.e. if duplicate key values are disallowed.
    const bool _isUnique;
};

}  // namespace mongo
