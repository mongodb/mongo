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

namespace mongo {

    /**
     * This class is a crutch to help migrate from the old everything-is-a-Cursor world to the new
     * index API world.  It wraps a new IndexCursor in the old Cursor.  We only use this for special
     * (2d, 2dsphere, text, haystack, hash) indices.
     */
    class EmulatedCursor : public Cursor {
    public:
        /**
         * Takes ownership of the provided indexAccessMethod indexCursor.
         * Takes ownership of the IndexDescriptor inside the indexAccessMethod.
         */
        EmulatedCursor(IndexDescriptor* descriptor, IndexAccessMethod* indexAccessMethod,
                       const BSONObj& query, const BSONObj& order, int numWanted)
            : _descriptor(descriptor), _indexAccessMethod(indexAccessMethod) {

            IndexCursor *cursor;
            indexAccessMethod->newCursor(&cursor);
            _indexCursor.reset(cursor);
            _indexCursor->seek(query);

            if (!_indexCursor->isEOF()) {
                _nscanned = 1;
            } else {
                _nscanned = 0;
            }

            if ("hashed" == KeyPattern::findPluginName(descriptor->keyPattern())) {
                // Quoted from hashindex.cpp:
                // Force a match of the query against the actual document by giving
                // the cursor a matcher with an empty indexKeyPattern.  This ensures the
                // index is not used as a covered index.
                // NOTE: this forcing is necessary due to potential hash collisions
                _matcher = shared_ptr<CoveredIndexMatcher>(
                    new CoveredIndexMatcher(query, BSONObj()));
                _supportYields = true;
                _supportGetMore = true;
                _modifiedKeys = true;
            } else {
                verify(0);
            }

            checkMultiKeyProperties();
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
            verify(_supportYields);
            _indexCursor->savePosition();
        }

        virtual void checkLocation() {
            verify(_supportYields);
            _indexCursor->restorePosition();
            checkMultiKeyProperties();
        }

        // Below this is where the Cursor <---> IndexCursor mapping breaks down.
        virtual CoveredIndexMatcher* matcher() const {
            return _matcher.get();
        }

        // XXX: this is true for everything but '2d'.
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
                pair<set<DiskLoc>::iterator, bool> p = _dups.insert(loc);
                return !p.second;
            } else {
                return false;
            }
        }

    private:
        void checkMultiKeyProperties() {
            if ("hashed" == KeyPattern::findPluginName(_descriptor->keyPattern())) {
                _shouldGetSetDup = _descriptor->isMultikey();
            }
        }

        set<DiskLoc> _dups;

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
    };

}  // namespace mongo
