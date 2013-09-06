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

#include "mongo/db/index/btree_index_cursor.h"

#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    unordered_set<BtreeIndexCursor*> BtreeIndexCursor::_activeCursors;
    SimpleMutex BtreeIndexCursor::_activeCursorsMutex("active_btree_index_cursors");

    // Go forward by default.
    BtreeIndexCursor::BtreeIndexCursor(IndexDescriptor *descriptor, Ordering ordering,
                                       BtreeInterface *interface)
        : _direction(1), _descriptor(descriptor), _ordering(ordering), _interface(interface),
          _bucket(descriptor->getHead()), _keyOffset(0) {

        SimpleMutex::scoped_lock lock(_activeCursorsMutex);
        _activeCursors.insert(this);
    }

    BtreeIndexCursor::~BtreeIndexCursor() {
        SimpleMutex::scoped_lock lock(_activeCursorsMutex);
        _activeCursors.erase(this);
    }

    bool BtreeIndexCursor::isEOF() const { return _bucket.isNull(); }

    // XXX TWO SHORT TERM HACKS THAT MUST DIE USED BY 2D INDEX:
    DiskLoc BtreeIndexCursor::getBucket() const { return _bucket; }
    int BtreeIndexCursor::getKeyOfs() const { return _keyOffset; }

    void BtreeIndexCursor::aboutToDeleteBucket(const DiskLoc& bucket) {
        SimpleMutex::scoped_lock lock(_activeCursorsMutex);
        for (unordered_set<BtreeIndexCursor*>::iterator i = _activeCursors.begin();
             i != _activeCursors.end(); ++i) {

            BtreeIndexCursor* ic = *i;
            if (bucket == ic->_bucket) {
                ic->_keyOffset = -1;
            }
        }
    }

    Status BtreeIndexCursor::setOptions(const CursorOptions& options) {
        if (CursorOptions::DECREASING == options.direction) {
            _direction = -1;
        } else {
            _direction = 1;
        }
        return Status::OK();
    }

    Status BtreeIndexCursor::seek(const BSONObj& position) {
        _keyOffset = 0;

        // Unused out parameter.
        bool found;

        _bucket = _interface->locate(
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

    Status BtreeIndexCursor::seek(const vector<const BSONElement*>& position,
                                  const vector<bool>& inclusive) {
        pair<DiskLoc, int> ignored;

        // Bucket is modified by customLocate.  Seeks start @ the root, so we set _bucket to the
        // root here.
        _bucket = _descriptor->getHead();
        _keyOffset = 0;

        _interface->customLocate(
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

    Status BtreeIndexCursor::skip(const BSONObj &keyBegin, int keyBeginLen, bool afterKey,
                                  const vector<const BSONElement*>& keyEnd,
                                  const vector<bool>& keyEndInclusive) {
        _interface->advanceTo(
            _bucket,
            _keyOffset,
            keyBegin,
            keyBeginLen,
            afterKey,
            keyEnd,
            keyEndInclusive,
            _ordering,
            (int)_direction);

        skipUnusedKeys();
        return Status::OK();
    }

    BSONObj BtreeIndexCursor::getKey() const {
        verify(!_bucket.isNull());
        return _interface->keyAt(_bucket, _keyOffset);
    }

    DiskLoc BtreeIndexCursor::getValue() const {
        verify(!_bucket.isNull());
        return _interface->recordAt(_bucket, _keyOffset);
    }

    void BtreeIndexCursor::next() { advance("BtreeIndexCursor::next"); skipUnusedKeys(); }

    Status BtreeIndexCursor::savePosition() {
        if (!isEOF()) {
            _savedKey = getKey().getOwned();
            _savedLoc = getValue();
            return Status::OK();
        } else {
            return Status(ErrorCodes::IllegalOperation, "Can't save position when EOF");
        }
    }

    Status BtreeIndexCursor::restorePosition() {
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
                if (BtreeInterface::deletedBucketCode != e.getCode()) {
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
        _bucket = _interface->locate(
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

    string BtreeIndexCursor::toString() { return "I AM A BTREE INDEX CURSOR!\n"; }

    void BtreeIndexCursor::skipUnusedKeys() {
        int skipped = 0;

        while (!isEOF() && !_interface->keyIsUsed(_bucket, _keyOffset)) {
            advance("BtreeIndexCursor::skipUnusedKeys");
            ++skipped;
        }

        if (skipped > 10) {
            OCCASIONALLY log() << "btree unused skipped: " << skipped << endl;
        }
    }

    bool BtreeIndexCursor::isSavedPositionValid() {
        // We saved the key.  If it's in the same position we saved it from...
        if (_interface->keyAt(_bucket, _keyOffset).binaryEqual(_savedKey)) {
            // And the record it points to is the same record...
            if (_interface->recordAt(_bucket, _keyOffset) == _savedLoc) {
                // Success!  We found it.  However!
                if (!_interface->keyIsUsed(_bucket, _keyOffset)) {
                    // We could have been deleted but still exist as a "vacant" key, so skip
                    // over any unused keys.
                    skipUnusedKeys();
                }
                return true;
            }
        }

        return false;
    }

    // Move to the next/prev. key.  Used by normal getNext and also skipping unused keys.
    void BtreeIndexCursor::advance(const char* caller) {
        _bucket = _interface->advance(_bucket, _keyOffset, _direction, caller);
    }

}  // namespace mongo
