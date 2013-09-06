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

#include "mongo/db/index/s2_index_cursor.h"

#include "mongo/db/btreecursor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/s2_near_cursor.h"
#include "mongo/db/index/s2_simple_cursor.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/queryutil.h"
#include "third_party/s2/s2regionintersection.h"

namespace mongo {

    S2IndexCursor::S2IndexCursor(const S2IndexingParams& params, IndexDescriptor* descriptor)
        : _params(params), _descriptor(descriptor) { }

    Status S2IndexCursor::seek(const BSONObj &position) {
        vector<GeoQuery> regions;
        bool isNearQuery = false;
        NearQuery nearQuery;

        // Go through the fields that we index, and for each geo one, make
        // a GeoQuery object for the S2*Cursor class to do intersection
        // testing/cover generating with.
        BSONObjIterator keyIt(_descriptor->keyPattern());
        while (keyIt.more()) {
            BSONElement keyElt = keyIt.next();

            if (keyElt.type() != String || IndexNames::GEO_2DSPHERE != keyElt.valuestr()) {
                continue;
            }

            BSONElement e = position.getFieldDotted(keyElt.fieldName());
            if (e.eoo()) { continue; }
            if (!e.isABSONObj()) { continue; }
            BSONObj obj = e.Obj();

            if (nearQuery.parseFrom(obj)) {
                if (isNearQuery) {
                    return Status(ErrorCodes::BadValue, "Only one $near clause allowed: " +
                                  position.toString(), 16685);
                }
                isNearQuery = true;

                if (nearQuery.fromRadians) {
                    nearQuery.minDistance *= _params.radius;
                    nearQuery.maxDistance *= _params.radius;
                }

                nearQuery.field = keyElt.fieldName();
                continue;
            }

            GeoQuery geoQueryField(keyElt.fieldName());
            if (!geoQueryField.parseFrom(obj)) {
                return Status(ErrorCodes::BadValue, "can't parse query (2dsphere): "
                                                   + obj.toString(), 16535);
            }
            if (!geoQueryField.hasS2Region()) {
                return Status(ErrorCodes::BadValue, "Geometry unsupported: " + obj.toString(),
                              16684);
            }
            regions.push_back(geoQueryField);
        }

        // Remove all the indexed geo regions from the query.  The s2*cursor will
        // instead create a covering for that key to speed up the search.
        //
        // One thing to note is that we create coverings for indexed geo keys during
        // a near search to speed it up further.
        BSONObjBuilder geoFieldsToNuke;

        if (isNearQuery) {
            geoFieldsToNuke.append(nearQuery.field, "");
        }

        for (size_t i = 0; i < regions.size(); ++i) {
            geoFieldsToNuke.append(regions[i].getField(), "");
        }

        // false means we want to filter OUT geoFieldsToNuke, not filter to include only that.
        BSONObj filteredQuery = position.filterFieldsUndotted(geoFieldsToNuke.obj(), false);

        if (isNearQuery) {
            S2NearIndexCursor* nearCursor = new S2NearIndexCursor(_descriptor, _params);
            _underlyingCursor.reset(nearCursor);
            nearCursor->seek(filteredQuery, nearQuery, regions);
        } else {
            S2SimpleCursor* simpleCursor = new S2SimpleCursor(_descriptor, _params);
            _underlyingCursor.reset(simpleCursor);
            simpleCursor->seek(filteredQuery, regions);
        }

        return Status::OK();
    }

    Status S2IndexCursor::setOptions(const CursorOptions& options) { return Status::OK(); }

    bool S2IndexCursor::isEOF() const { return _underlyingCursor->isEOF(); }
    BSONObj S2IndexCursor::getKey() const { return _underlyingCursor->getKey(); }
    DiskLoc S2IndexCursor::getValue() const { return _underlyingCursor->getValue(); }
    void S2IndexCursor::next() { _underlyingCursor->next(); }
    string S2IndexCursor::toString() { return _underlyingCursor->toString(); }
    Status S2IndexCursor::savePosition() { return _underlyingCursor->savePosition(); }
    Status S2IndexCursor::restorePosition() { return _underlyingCursor->restorePosition(); }

}  // namespace mongo
