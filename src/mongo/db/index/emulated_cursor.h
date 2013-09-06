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

#pragma once

#include <boost/scoped_ptr.hpp>
#include <set>

#include "mongo/db/cursor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

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
         *
         * Full semantics of numWanted:
         * numWanted == 0 : Return any number of results, but try to return in batches of 101.
         * numWanted == 1 : Return exactly one result.
         * numWanted  > 1 : Return any number of results, but try to return in batches of numWanted.
         *
         * In practice, your cursor can ignore numWanted, as enforcement of limits is done
         * by the caller.
         */
        static EmulatedCursor* make(IndexDescriptor* descriptor,
                                    IndexAccessMethod* indexAccessMethod,
                                    const BSONObj& query,
                                    const BSONObj& order,
                                    int numWanted,
                                    const BSONObj& keyPattern) {

            verify(descriptor);
            verify(indexAccessMethod);

            auto_ptr<EmulatedCursor> ret(new EmulatedCursor(descriptor, indexAccessMethod,
                                                            order, numWanted, keyPattern));
            // Why do we have to do this?  No reading from disk is allowed in constructors,
            // and seeking involves reading from disk.
            ret->seek(query);
            return ret.release();
        }

        // We defer everything we can to the underlying cursor.
        virtual bool ok() { return !_indexCursor->isEOF(); }
        virtual Record* _current() { return currLoc().rec(); }
        virtual BSONObj current() { return BSONObj::make(_current()); }
        virtual DiskLoc currLoc() { return _indexCursor->getValue(); }
        virtual BSONObj currKey() const { return _indexCursor->getKey(); }
        virtual DiskLoc refLoc() {
            // This will sometimes get called even if we're not OK.
            // See ClientCursor::prepareToYield/updateLocation.
            if (!ok()) {
                return DiskLoc();
            } else {
                return currLoc();
            }
        }
        virtual long long nscanned() { return _nscanned; }
        virtual string toString() { return _indexCursor->toString(); }

        virtual void explainDetails(BSONObjBuilder& b) {
            _indexCursor->explainDetails(&b);
            return;
        }

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

        virtual bool supportGetMore() { return _supportGetMore; }
        virtual bool supportYields() { return _supportYields; }
        virtual bool isMultiKey() const { return _isMultiKey; }
        virtual bool modifiedKeys() const { return _modifiedKeys; }
        virtual bool autoDedup() const { return _autoDedup; }

        virtual bool getsetdup(DiskLoc loc) {
            if (_shouldGetSetDup) {
                pair<unordered_set<DiskLoc, DiskLoc::Hasher>::iterator, bool> p = _dups.insert(loc);
                return !p.second;
            } else {
                return false;
            }
        }

    private:
        EmulatedCursor(IndexDescriptor* descriptor, IndexAccessMethod* indexAccessMethod,
                       const BSONObj& order, int numWanted, const BSONObj& keyPattern)
            : _descriptor(descriptor), _indexAccessMethod(indexAccessMethod),
              _keyPattern(keyPattern), _pluginName(IndexNames::findPluginName(keyPattern)) {

            IndexCursor *cursor;
            indexAccessMethod->newCursor(&cursor);
            _indexCursor.reset(cursor);

            CursorOptions options;
            options.numWanted = numWanted;
            cursor->setOptions(options);

            if (IndexNames::HASHED == _pluginName) {
                _supportYields = true;
                _supportGetMore = true;
                _modifiedKeys = true;
                _shouldGetSetDup = false;
                _autoDedup = true;
            } else if (IndexNames::GEO_2DSPHERE == _pluginName) {
                _supportYields = true;
                _supportGetMore = true;
                _modifiedKeys = true;
                // Note: this duplicates the de-duplication in near cursors.  Fix near cursors
                // to not de-dup themselves.
                _shouldGetSetDup = true;
                _autoDedup = false;
            } else if (IndexNames::GEO_2D == _pluginName) {
                _supportYields = false;
                _supportGetMore = true;
                _modifiedKeys = true;
                _isMultiKey = false;
                _shouldGetSetDup = false;
                _autoDedup = false;
            } else {
                verify(0);
            }

            // _isMultiKey and _shouldGetSetDup are set in this unless it's 2d.
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

            if (IndexNames::HASHED == _pluginName) {
                // Quoted from hashindex.cpp:
                // Force a match of the query against the actual document by giving
                // the cursor a matcher with an empty indexKeyPattern.  This ensures the
                // index is not used as a covered index.
                // NOTE: this forcing is necessary due to potential hash collisions
                _matcher = shared_ptr<CoveredIndexMatcher>(
                    new CoveredIndexMatcher(query, BSONObj()));
            } else if (IndexNames::GEO_2DSPHERE == _pluginName) {
                // Technically, the non-geo indexed fields are in the key, though perhaps not in the
                // exact format the matcher expects (arrays).  So, we match against all non-geo
                // fields.  This could possibly be relaxed in some fashion in the future?  Requires
                // query work.
                BSONObjBuilder fieldsToNuke;
                BSONObjIterator keyIt(_keyPattern);

                while (keyIt.more()) {
                    BSONElement e = keyIt.next();
                    if (e.type() == String && IndexNames::GEO_2DSPHERE == e.valuestr()) {
                        fieldsToNuke.append(e.fieldName(), "");
                    }
                }

                BSONObj filteredQuery = query.filterFieldsUndotted(fieldsToNuke.obj(), false);

                _matcher = shared_ptr<CoveredIndexMatcher>(
                    new CoveredIndexMatcher(filteredQuery, _keyPattern));
            } else if (IndexNames::GEO_2D == _pluginName) {
                // No-op matcher.
                _matcher = shared_ptr<CoveredIndexMatcher>(
                    new CoveredIndexMatcher(BSONObj(), BSONObj()));
            }
        }

        void checkMultiKeyProperties() {
            if (IndexNames::GEO_2D != _pluginName) {
                _isMultiKey = _shouldGetSetDup = _descriptor->isMultikey();
            }
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
        bool _autoDedup;

        BSONObj _keyPattern;
        string _pluginName;
    };

}  // namespace mongo
