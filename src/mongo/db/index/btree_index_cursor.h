/**
*    Copyright (C) 2013 10gen Inc.
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

#include <vector>
#include "mongo/base/status.h"
#include "mongo/db/btree.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"

namespace mongo {

    template <class Key> class BtreeIndexCursor : public IndexCursor {
    public:
        // The in-memory wrapper, not part of Btree storage.
        typedef typename BucketBasics<Key>::KeyNode KeyNode;
        // The on-disk data.
        typedef typename Key::_KeyNode OnDiskKeyNode;

        virtual ~BtreeIndexCursor() { }

        bool isEOF() const { return _bucket.isNull(); }

        // XXX SHORT TERM HACKS THAT MUST DIE: 2d index
        virtual DiskLoc getBucket() const { return _bucket; }

        // XXX SHORT TERM HACKS THAT MUST DIE: 2d index
        virtual int getKeyOfs() const { return _keyOffset; }

        // XXX SHORT TERM HACKS THAT MUST DIE: btree deletion
        virtual void aboutToDeleteBucket(const DiskLoc& bucket) {
            if (bucket == _bucket) {
                _keyOffset = -1;
            }
        }

        virtual Status setOptions(const CursorOptions& options) {
            if (CursorOptions::DECREASING == options.direction) {
                _direction = -1;
            } else {
                _direction = 1;
            }
            return Status::OK();
        }

        virtual Status seek(const BSONObj &position) {
            // Unused out parameter.
            bool found;

            _bucket = _descriptor->getHead().btree<Key>()->locate(
                _descriptor->getOnDisk(),
                _descriptor->getHead(),
                position,
                _ordering,
                _keyOffset,
                found, 
                1 == _direction ? minDiskLoc : maxDiskLoc,
                _direction);

            skipUnusedKeys();

            return Status::OK();
        }

        virtual Status seek(const vector<const BSONElement*> &position,
                            const vector<bool> &inclusive) {
            pair<DiskLoc, int> ignored;

            // Bucket is modified by customLocate.  Seeks start @ the root, so we set _bucket to the
            // root here.
            _bucket = _descriptor->getHead();

            _descriptor->getHead().btree<Key>()->customLocate(
                _bucket,
                _keyOffset,
                _emptyObj,
                0,
                false,
                position,
                inclusive,
                _ordering,
                (int)_direction,
                ignored);

            skipUnusedKeys();

            return Status::OK();
        }

        virtual Status skip(const vector<const BSONElement*> &position,
                            const vector<bool> &inclusive) {
            _bucket.btree<Key>()->advanceTo(
                _bucket,
                _keyOffset,
                _emptyObj,
                0,
                false,
                position,
                inclusive,
                _ordering,
                (int)_direction);

            skipUnusedKeys();

            return Status::OK();
        }

        virtual BSONObj getKey() const { return currKey(); }
        virtual DiskLoc getValue() const { return currLoc(); }
        virtual void next() { advance("BtreeIndexCursor::next"); skipUnusedKeys(); }

        virtual Status savePosition() {
            if (!isEOF()) {
                _savedKey = currKey().getOwned();
                _savedLoc = currLoc();
                return Status::OK();
            } else {
                return Status(ErrorCodes::IllegalOperation, "Can't save position when EOF");
            }
        }

        virtual Status restorePosition() {
            // _keyOffset could be -1 if the bucket was deleted.  When buckets are deleted, the
            // Btree calls a clientcursor function that calls down to all BTree buckets.  Really,
            // this deletion thing should be kept BTree-internal.
            if (_keyOffset >= 0) {
                verify(!_savedKey.isEmpty());

                try {
                    if (isSavedPositionValid()) { return Status::OK(); }
                    if (_keyOffset > 0) {
                        --_keyOffset;
                        // "we check one key earlier too, in case a key was just deleted.  this is
                        // important so that multi updates are reasonably fast." -- btreecursor.cpp
                        if (isSavedPositionValid()) { return Status::OK(); }
                    }
                    // Object isn't at the saved position.  Fall through to calling seek.
                } catch (UserException& e) { 
                    // deletedBucketCode is what keyAt throws if the bucket was deleted.  Not a
                    // problem...
                    if (deletedBucketCode != e.getCode()) {
                        return e.toStatus();
                    }
                    // Our bucket was deleted, so we look for the saved key.
                    DEV log() << "debug info: bucket was deleted" << endl;
                }
            }

            // Our old position was invalidated (keyOfs was set to -1) or our saved position
            // is no longer valid, so we must re-find.
            RARELY log() << "key seems to have moved in the index, refinding. "
                         << _bucket.toString() << endl;

            bool found;

            // Why don't we just call seek?  Because we want to pass _savedLoc.
            _bucket = _descriptor->getHead().btree<Key>()->locate(
                _descriptor->getOnDisk(),
                _descriptor->getHead(),
                _savedKey,
                _ordering,
                _keyOffset,
                found, 
                _savedLoc,
                _direction);

            skipUnusedKeys();

            return Status::OK();
        }

        virtual string toString() { return "I AM A BTREE INDEX CURSOR!\n"; }

    private:
        // We keep the constructor private and only allow the AM to create us.
        friend class BtreeAccessMethod<V0>;
        friend class BtreeAccessMethod<V1>;

        // Go forward by default.
        BtreeIndexCursor(IndexDescriptor *descriptor, Ordering ordering)
            : _direction(1), _descriptor(descriptor), _ordering(ordering) {
            _keyOffset = 0;
            _bucket = descriptor->getHead();
        }

        void skipUnusedKeys() {
            int skipped = 0;

            while (!isEOF() && !onDiskKeyNodeAt(_bucket, _keyOffset).isUsed()) {
                advance("BtreeIndexCursor::skipUnusedKeys");
                ++skipped;
            }

            if (skipped > 10) {
                OCCASIONALLY log() << "btree unused skipped: " << skipped << endl;
            }
        }

        bool isSavedPositionValid() {
            // We saved the key.  If it's in the same position we saved it from...
            if (keyAt(_bucket, _keyOffset).binaryEqual(_savedKey)) {
                const OnDiskKeyNode& kn = onDiskKeyNodeAt(_bucket, _keyOffset);
                // And the record it points to is the same record...
                if (kn.recordLoc == _savedLoc) {
                    // Success!  We found it.  However!
                    if (!kn.isUsed()) {
                        // We could have been deleted but still exist as a "vacant" key, so skip
                        // over any unused keys.
                        skipUnusedKeys();
                    }
                    return true;
                }
            }

            return false;
        }

        // Get the on-disk representation of the key at (bucket, keyOffset)
        const OnDiskKeyNode& onDiskKeyNodeAt(DiskLoc bucket, int keyOffset) const { 
            return bucket.btree<Key>()->k(keyOffset);
        }

        static const int deletedBucketCode = 16738;

        // Get the BSON value of the key that's at (bucket, ofs)
        // If there is no such key, return a BSONObj().
        BSONObj keyAt(DiskLoc bucket, int ofs) const { 
            verify(!bucket.isNull());
            const BtreeBucket<Key> *b = bucket.btree<Key>();
            int n = b->getN();
            if (n == b->INVALID_N_SENTINEL) {
                throw UserException(deletedBucketCode, "keyAt bucket deleted");
            }
            dassert( n >= 0 && n < 10000 );
            return ofs >= n ? BSONObj() : b->keyNode(ofs).key.toBson();
        }

        // Move to the next/prev. key.  Used by normal getNext and also skipping unused keys.
        void advance(const char* caller) {
            _bucket = _bucket.btree<Key>()->advance(_bucket, _keyOffset, _direction, caller);
        }

        // Accessors for our current state.
        const KeyNode getKeyNode() const {
            verify(!_bucket.isNull());
            const BtreeBucket<Key> *b = _bucket.btree<Key>();
            return b->keyNode(_keyOffset);
        }

        const BSONObj currKey() const {
            verify(!_bucket.isNull());
            return getKeyNode().key.toBson();
        }

        DiskLoc currLoc() const { 
            verify(!_bucket.isNull());
            return getKeyNode().recordLoc;
        }

        // For saving/restoring position.
        BSONObj _savedKey;
        DiskLoc _savedLoc;

        // What are we looking at RIGHT NOW?  We look at a bucket.
        DiskLoc _bucket;
        // And we look at an offset in the bucket.
        int _keyOffset;

        BSONObj _emptyObj;

        int _direction;
        IndexDescriptor *_descriptor;
        Ordering _ordering;
    };

    template class BtreeIndexCursor<V0>;
    template class BtreeIndexCursor<V1>;

}  // namespace
