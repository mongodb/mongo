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

#pragma once

#include <boost/scoped_ptr.hpp>
#include <set>

#include "mongo/db/cursor.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    static const string SPHERE_2D_NAME = "2dsphere";
    static const string HASH_NAME = "hashed";

    /**
     * This class is a crutch to help migrate from the old everything-is-a-Cursor world to the new
     * index API world.  It wraps a new IndexCursor in the old Cursor.  We only use this for special
     * (2d, 2dsphere, text, haystack, hash) indices.
     */
    class EmulatedCursor : public Cursor {
    public:
        /**
         * Create a new EmulatedCursor.
         * Takes ownership of the provided indexAccessMethod indexCursor.
         * Takes ownership of the IndexDescriptor inside the indexAccessMethod.
         */
        static EmulatedCursor* make(IndexDescriptor* descriptor,
                                    IndexAccessMethod* indexAccessMethod,
                                    const BSONObj& query,
                                    const BSONObj& order,
                                    int numWanted,
                                    const BSONObj& keyPattern) {

            EmulatedCursor* ret = new EmulatedCursor(descriptor, indexAccessMethod,
                                                     order, numWanted, keyPattern);
            // Why do we have to do this?  No reading from disk is allowed in constructors,
            // and seeking involves reading from disk.
            ret->seek(query);
            return ret;
        }

        // We defer everything we can to the underlying cursor.
        virtual bool ok() { return !_indexCursor->isEOF(); }
        virtual Record* _current() { return currLoc().rec(); }
        virtual BSONObj current() { return BSONObj::make(_current()); }
        virtual DiskLoc currLoc() { return _indexCursor->getValue(); }
        virtual BSONObj currKey() const { return _indexCursor->getKey(); }
        virtual DiskLoc refLoc() { return currLoc(); }
        virtual long long nscanned() { return _nscanned; }
        virtual string toString() { return _indexCursor->toString(); }

        virtual bool advance() {
            _indexCursor->next();
            if (ok()) {
                ++_nscanned;
            }
            return ok();
        }

        virtual void noteLocation() {
            verify(_supportYields || _supportGetMore);
            _indexCursor->savePosition();
        }

        virtual void checkLocation() {
            verify(_supportYields || _supportGetMore);
            _indexCursor->restorePosition();
            // Somebody might have inserted a multikey during a yield.
            checkMultiKeyProperties();
        }

        // Below this is where the Cursor <---> IndexCursor mapping breaks down.
        virtual CoveredIndexMatcher* matcher() const {
            return _matcher.get();
        }

        virtual BSONObj indexKeyPattern() {
            return _descriptor->keyPattern();
        }

        // XXX: I think this is true for everything.
        virtual bool supportGetMore() { return _supportGetMore; }

        // XXX: this is true for everything but '2d'.
        virtual bool supportYields() { return _supportYields; }

        // XXX: true for 2dsphere
        // XXX: false for 2d, hash
        // XXX: I think it's correct yet slow to leave as 'true'.
        virtual bool isMultiKey() const { return _isMultiKey; }

        // XXX: currently is set to 'true' for 2d and 2dsphere.
        // XXX: default is false, though hash doesn't set it to true (?)
        // XXX: I think it's correct yet slow to leave as 'true'.
        virtual bool modifiedKeys() const { return _modifiedKeys; }

        virtual bool getsetdup(DiskLoc loc) {
            if (_shouldGetSetDup) {
                pair<unordered_set<DiskLoc, DiskLoc::Hasher>::iterator, bool> p = _dups.insert(loc);
                return !p.second;
            } else {
                return false;
            }
        }

        virtual void aboutToDeleteBucket(const DiskLoc& b) {
            _indexCursor->aboutToDeleteBucket(b);
        }

    private:
        EmulatedCursor(IndexDescriptor* descriptor, IndexAccessMethod* indexAccessMethod,
                       const BSONObj& order, int numWanted, const BSONObj& keyPattern)
            : _descriptor(descriptor), _indexAccessMethod(indexAccessMethod),
              _keyPattern(keyPattern), _pluginName(KeyPattern::findPluginName(keyPattern)) {

            IndexCursor *cursor;
            indexAccessMethod->newCursor(&cursor);
            _indexCursor.reset(cursor);

            if (HASH_NAME == _pluginName) {
                _supportYields = true;
                _supportGetMore = true;
                _modifiedKeys = true;
                _shouldGetSetDup = false;
            } else if (SPHERE_2D_NAME == _pluginName) {
                _supportYields = true;
                _supportGetMore = true;
                _modifiedKeys = true;
                // XXX: this duplicates the de-duplication in near cursors.  maybe fix near cursors
                // to not de-dup themselves.
                _shouldGetSetDup = true;
            } else {
                verify(0);
            }

            // _isMultiKey and _shouldGetSetDup are set in this.
            checkMultiKeyProperties();
        }

        void seek(const BSONObj& query) {
            Status seekStatus = _indexCursor->seek(query);

            // Our seek could be malformed.  Code above us expects an exception if so.
            if (Status::OK() != seekStatus) {
                uasserted(seekStatus.location(), seekStatus.reason());
            }

            if (!_indexCursor->isEOF()) {
                _nscanned = 1;
            } else {
                _nscanned = 0;
            }

            if (HASH_NAME == _pluginName) {
                // Quoted from hashindex.cpp:
                // Force a match of the query against the actual document by giving
                // the cursor a matcher with an empty indexKeyPattern.  This ensures the
                // index is not used as a covered index.
                // NOTE: this forcing is necessary due to potential hash collisions
                _matcher = shared_ptr<CoveredIndexMatcher>(
                    new CoveredIndexMatcher(query, BSONObj()));
            } else if (SPHERE_2D_NAME == _pluginName) {
                // Technically, the non-geo indexed fields are in the key, though perhaps not in the
                // exact format the matcher expects (arrays).  So, we match against all non-geo
                // fields.  This could possibly be relaxed in some fashion in the future?  Requires
                // query work.
                BSONObjBuilder fieldsToNuke;
                BSONObjIterator keyIt(_keyPattern);

                while (keyIt.more()) {
                    BSONElement e = keyIt.next();
                    if (e.type() == String && SPHERE_2D_NAME == e.valuestr()) {
                        fieldsToNuke.append(e.fieldName(), "");
                    }
                }

                BSONObj filteredQuery = query.filterFieldsUndotted(fieldsToNuke.obj(), false);

                _matcher = shared_ptr<CoveredIndexMatcher>(
                    new CoveredIndexMatcher(filteredQuery, _keyPattern));
            }
        }

        void checkMultiKeyProperties() {
            _isMultiKey = _shouldGetSetDup = _descriptor->isMultikey();
        }

        unordered_set<DiskLoc, DiskLoc::Hasher> _dups;

        scoped_ptr<IndexDescriptor> _descriptor;
        scoped_ptr<IndexAccessMethod> _indexAccessMethod;
        scoped_ptr<IndexCursor> _indexCursor;

        long long _nscanned;
        shared_ptr<CoveredIndexMatcher> _matcher;

        bool _supportYields;
        bool _supportGetMore;
        bool _isMultiKey;
        bool _modifiedKeys;
        bool _shouldGetSetDup;

        BSONObj _keyPattern;
        string _pluginName;
    };

}  // namespace mongo
