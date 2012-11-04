/**
 *    Copyright (C) 2008 10gen Inc.
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
 */

#pragma once

#include "mongo/db/cursor.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_details.h"

namespace mongo {

    class FieldRangeVector;
    class FieldRangeVectorIterator;

    /**
     * A Cursor class for btree iteration.
     *
     * A BtreeCursor iterates over all keys of a specified btree that are within the provided
     * btree bounds, using the specified direction of traversal.
     *
     * Notes on specifying btree bounds:
     *
     * A BtreeCursor may be initialized with the 'startKey' and 'endKey' bounds of an interval of
     * keys within a btree.  A BtreeCursor may alternatively be initialized with a FieldRangeVector
     * describing a list of intervals for every field of the btree index.
     *
     * Notes on inheritance:
     *
     * BtreeCursor is an abstract class.  Btrees come in different versions, with different storage
     * formats.  The versions are v0 and v1 as of this writing.  Member functions implemented in
     * BtreeCursor are storage format agnostic.  Classes derived from BtreeCursor implement the
     * format specific bits.
     *
     * Notes on the yielding implementation:
     *
     * When an operation using a BtreeCursor yields the database mutex that locks the btree data
     * structure, the btree may be modified.  When the operation regains the database mutex, the
     * BtreeCursor can relocate its position in the modified btree and continue iteration from that
     * point.
     *
     * Before the database mutex is yielded, a BtreeCursor records its position (noteLoc()).  A
     * recorded btree position consists of a btree bucket, bucket key offset, and unique btree key.
     *
     * After the database mutex is regained, a BtreeCursor relocates its position (checkLoc()).  To
     * relocate a unique btree key, a BtreeCursor first checks the btree key at its recorded btree
     * bucket and bucket key offset.  If the key at that location does not match the recorded btree
     * key, and a preceding offset also fails to match, the recorded key (or the next existing key
     * following it) is located in the btree using binary search.  If the recorded btree bucket is
     * invalidated, the initial recorded bucket check is not attempted (see SERVER-4575).
     */
    class BtreeCursor : public Cursor {
    public:
        virtual ~BtreeCursor();

        /** Makes an appropriate subclass depending on the index version. */

        static BtreeCursor* make( NamespaceDetails* namespaceDetails,
                                  const IndexDetails& id,
                                  const BSONObj& startKey,
                                  const BSONObj& endKey,
                                  bool endKeyInclusive,
                                  int direction );

        static BtreeCursor* make( NamespaceDetails* namespaceDetails,
                                  const IndexDetails& id,
                                  const shared_ptr<FieldRangeVector>& bounds,
                                  int singleIntervalLimit,
                                  int direction );

        virtual bool ok() { return !bucket.isNull(); }
        virtual bool advance();
        virtual void noteLocation(); // updates keyAtKeyOfs...
        virtual void checkLocation() = 0;
        virtual bool supportGetMore() { return true; }
        virtual bool supportYields() { return true; }

        /**
         * used for multikey index traversal to avoid sending back dups. see Matcher::matches().
         * if a multikey index traversal:
         *   if loc has already been sent, returns true.
         *   otherwise, marks loc as sent.
         * @return false if the loc has not been seen
         */
        virtual bool getsetdup(DiskLoc loc) {
            if( _multikey ) {
                pair<set<DiskLoc>::iterator, bool> p = _dups.insert(loc);
                return !p.second;
            }
            return false;
        }

        virtual bool modifiedKeys() const { return _multikey; }
        virtual bool isMultiKey() const { return _multikey; }

        /** returns BSONObj() if ofs is out of range */
        virtual BSONObj keyAt(int ofs) const = 0;

        virtual BSONObj currKey() const = 0;
        virtual BSONObj indexKeyPattern() { return _order; }

        virtual void aboutToDeleteBucket(const DiskLoc& b) {
            if ( bucket == b )
                keyOfs = -1;
        }

        virtual DiskLoc currLoc() = 0;
        virtual DiskLoc refLoc()   { return currLoc(); }
        virtual Record* _current() { return currLoc().rec(); }
        virtual BSONObj current()  { return BSONObj::make(_current()); }
        virtual string toString();

        BSONObj prettyKey( const BSONObj& key ) const {
            return key.replaceFieldNames( indexDetails.keyPattern() ).clientReadable();
        }

        virtual BSONObj prettyIndexBounds() const;

        virtual CoveredIndexMatcher* matcher() const { return _matcher.get(); }

        virtual bool currentMatches( MatchDetails* details = 0 );

        virtual void setMatcher( shared_ptr<CoveredIndexMatcher> matcher ) { _matcher = matcher;  }

        virtual const Projection::KeyOnly* keyFieldsOnly() const { return _keyFieldsOnly.get(); }

        virtual void setKeyFieldsOnly( const shared_ptr<Projection::KeyOnly>& keyFieldsOnly ) {
            _keyFieldsOnly = keyFieldsOnly;
        }

        virtual long long nscanned() { return _nscanned; }

        /** for debugging only */
        const DiskLoc getBucket() const { return bucket; }
        int getKeyOfs() const { return keyOfs; }

        // just for unit tests
        virtual bool curKeyHasChild() = 0;

    protected:
        BtreeCursor( NamespaceDetails* nsd, int theIndexNo, const IndexDetails& idxDetails );

        virtual void init( const BSONObj& startKey,
                           const BSONObj& endKey,
                           bool endKeyInclusive,
                           int direction );

        virtual void init( const shared_ptr<FieldRangeVector>& bounds,
                           int singleIntervalLimit,
                           int direction );

        /**
         * Our btrees may (rarely) have "unused" keys when items are deleted.
         * Skip past them.
         */
        virtual bool skipUnusedKeys() = 0;

        bool skipOutOfRangeKeysAndCheckEnd();

        /**
         * Attempt to locate the next btree key matching _bounds.  This may mean advancing to the
         * next successive key in the btree, or skipping to a new position in the btree.  If an
         * internal iteration cutoff is reached before a matching key is found, then the search for
         * a matching key will be aborted, leaving the cursor pointing at a key that is not within
         * bounds.
         */
        void skipAndCheck();

        void checkEnd();

        /** selective audits on construction */
        void audit();

        virtual void _audit() = 0;

        virtual DiskLoc _locate(const BSONObj& key, const DiskLoc& loc) = 0;

        virtual DiskLoc _advance(const DiskLoc& thisLoc,
                                 int& keyOfs,
                                 int direction,
                                 const char* caller) = 0;

        virtual void _advanceTo(DiskLoc& thisLoc,
                                int& keyOfs,
                                const BSONObj& keyBegin,
                                int keyBeginLen,
                                bool afterKey,
                                const vector<const BSONElement*>& keyEnd,
                                const vector<bool>& keyEndInclusive,
                                const Ordering& order,
                                int direction ) = 0;

        /** set initial bucket */
        void initWithoutIndependentFieldRanges();

        /** if afterKey is true, we want the first key with values of the keyBegin fields greater than keyBegin */
        void advanceTo( const BSONObj& keyBegin,
                        int keyBeginLen,
                        bool afterKey,
                        const vector<const BSONElement*>& keyEnd,
                        const vector<bool>& keyEndInclusive );

        // these are set in the construtor
        NamespaceDetails* const d;
        const int idxNo;
        const IndexDetails& indexDetails;

        // these are all set in init()
        set<DiskLoc> _dups;
        BSONObj startKey;
        BSONObj endKey;
        bool _endKeyInclusive;
        bool _multikey; // this must be updated every getmore batch in case someone added a multikey
        BSONObj _order; // this is the same as indexDetails.keyPattern()
        Ordering _ordering;
        DiskLoc bucket;
        int keyOfs;
        int _direction; // 1=fwd,-1=reverse
        BSONObj keyAtKeyOfs; // so we can tell if things moved around on us between the query and the getMore call
        DiskLoc locAtKeyOfs;
        shared_ptr<FieldRangeVector> _bounds;
        auto_ptr<FieldRangeVectorIterator> _boundsIterator;
        bool _boundsMustMatch; // If iteration is aborted before a key matching _bounds is
                               // identified, the cursor may be left pointing at a key that is not
                               // within bounds (_bounds->matchesKey( currKey() ) may be false).
                               // _boundsMustMatch will be set to false accordingly.
        shared_ptr<CoveredIndexMatcher> _matcher;
        shared_ptr<Projection::KeyOnly> _keyFieldsOnly;
        bool _independentFieldRanges;
        long long _nscanned;

    private:
        void _finishConstructorInit();
        static BtreeCursor* make( NamespaceDetails* nsd,
                                  int idxNo,
                                  const IndexDetails& indexDetails );
    };

} // namespace mongo
