#pragma once

#include "btree.h"

namespace mongo {

    /**
     * build btree from the bottom up
     */
    class BtreeBuilder {
        bool dupsAllowed;
        IndexDetails& idx;
        /** Number of keys added to btree. */
        unsigned long long n;
        /** Last key passed to addKey(). */
        Key keyLast;
        BSONObj order;
        Ordering ordering;
        /** true iff commit() completed successfully. */
        bool committed;

        DiskLoc cur, first;
        BtreeBucket *b;

        void newBucket();
        void buildNextLevel(DiskLoc);
        void mayCommitProgressDurably();

    public:
        ~BtreeBuilder();

        BtreeBuilder(bool _dupsAllowed, IndexDetails& _idx);

        /**
         * Preconditions: 'key' is > or >= last key passed to this function (depends on _dupsAllowed)
         * Postconditions: 'key' is added to intermediate storage.
         */
        void addKey(BSONObj& key, DiskLoc loc);

        /**
         * commit work.  if not called, destructor will clean up partially completed work
         *  (in case exception has happened).
         */
        void commit();

        unsigned long long getn() { return n; }
    };

}
