#pragma once

#include "btree.h"

namespace mongo {

    /**
     * build btree from the bottom up
     */
    template< class V >
    class BtreeBuilder {
        typedef typename V::KeyOwned KeyOwned;
        typedef typename V::Key Key;
        
        bool dupsAllowed;
        IndexDetails& idx;
        /** Number of keys added to btree. */
        unsigned long long n;
        /** Last key passed to addKey(). */
        auto_ptr< typename V::KeyOwned > keyLast;
        BSONObj order;
        Ordering ordering;
        /** true iff commit() completed successfully. */
        bool committed;

        DiskLoc cur, first;
        BtreeBucket<V> *b;

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
