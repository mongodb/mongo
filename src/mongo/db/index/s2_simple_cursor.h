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

#include <vector>

#include "mongo/db/btreecursor.h"
#include "mongo/db/geo/geoquery.h"
#include "mongo/db/geo/s2common.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"
#include "mongo/platform/unordered_set.h"
#include "third_party/s2/s2cap.h"
#include "third_party/s2/s2regionintersection.h"

namespace mongo {

    class S2SimpleCursor : public IndexCursor {
    public:
        S2SimpleCursor(IndexDescriptor* descriptor, const S2IndexingParams& params);

        virtual ~S2SimpleCursor() { }

        // Not implemented
        virtual Status seek(const BSONObj& position);
        Status setOptions(const CursorOptions& options);

        // Implemented:
        // Not part of the IndexCursor spec.
        void seek(const BSONObj& query, const vector<GeoQuery>& regions);

        bool isEOF() const;
        BSONObj getKey() const;
        DiskLoc getValue() const;
        void next();

        virtual string toString();

        virtual Status savePosition();
        virtual Status restorePosition();

    private:
        IndexDescriptor* _descriptor;

        // The query with the geo stuff taken out.  We use this with a matcher.
        BSONObj _filteredQuery;

        // What geo regions are we looking for?
        vector<GeoQuery> _fields;

        // How were the keys created?  We need this to search for the right stuff.
        S2IndexingParams _params;

        // What have we checked so we don't repeat it and waste time?
        unordered_set<DiskLoc, DiskLoc::Hasher> _seen;

        // This really does all the work/points into the btree.
        scoped_ptr<BtreeCursor> _btreeCursor;

        // Stat counters/debug information goes below:
        // How many items did we look at in the btree?
        long long _nscanned;

        // How many did we try to match?
        long long _matchTested;

        // How many did we geo-test?
        long long _geoTested;

        // How many cells were in our cover?
        long long _cellsInCover;
    };

}  // namespace mongo
