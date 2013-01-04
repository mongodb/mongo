/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/geo/s2common.h"

namespace mongo {
    static string myitoa(int d) {
        stringstream ss;
        ss << d;
        return ss.str();
    }

    BSONObj S2SearchUtil::coverAsBSON(S2RegionCoverer *coverer, const S2Region& region,
                                      const string& field) {
        BSONObjBuilder queryBuilder;
        BSONObjBuilder inBuilder(queryBuilder.subobjStart(field));
        // To have an array where elements of that array are regexes, we have to do this.
        BSONObjBuilder inArrayBuilder(inBuilder.subarrayStart("$in"));
        // Sadly we must keep track of this ourselves.  Oh, BSONObjBuilder, you rascsal!
        int arrayPos = 0;

        bool considerCoarser = true;
        vector<S2CellId> cover;
        coverer->GetCovering(region, &cover);
        if (cover.size() > 5000) {
            int oldCoverSize = cover.size();
            int oldMaxLevel = coverer->max_level();
            coverer->set_max_level(coverer->min_level());
            coverer->set_min_level(3 * coverer->min_level() / 4);
            // Our finest level is the coarsest level in the index, so don't look coarser because
            // there's nothing there.
            considerCoarser = false;
            coverer->GetCovering(region, &cover);
            warning() << "Trying to create BSON indexing obj w/too many regions = " << oldCoverSize
                      << endl;
            warning() << "Modifying coverer from (" << coverer->max_level() << "," << oldMaxLevel
                      << ") to (" << coverer->min_level() << "," << coverer->max_level() << ")"
                      << endl;
            warning() << "New #regions = " << cover.size() << endl;
        }

        // Look at the cells we cover and all cells that are within our covering and
        // finer.  Anything with our cover as a strict prefix is contained within the cover and
        // should be intersection tested.
        for (size_t i = 0; i < cover.size(); ++i) {
            // First argument is position in the array as a string.
            // Third argument is options to regex.
            inArrayBuilder.appendRegex(myitoa(arrayPos++), "^" + cover[i].toString(), "");
        }

        if (considerCoarser) {
            // Look at the cells that cover us.  We want to look at every cell that
            // contains the covering we would index on if we were to insert the
            // query geometry.  We generate the would-index-with-this-covering and
            // find all the cells strictly containing the cells in that set, until we hit the
            // coarsest indexed cell.  We use $in, not a prefix match.  Why not prefix?  Because
            // we've already looked at everything finer or as fine as our initial covering.
            //
            // Say we have a fine point with cell id 212121, we go up one, get 21212, we don't
            // want to look at cells 21212[not-1] because we know they're not going to intersect
            // with 212121, but entries inserted with cell value 21212 (no trailing digits) may.
            // And we've already looked at points with the cell id 211111 from the regex search
            // created above, so we only want things where the value of the last digit is not
            // stored (and therefore could be 1).
            set<S2CellId> parents;
            for (size_t i = 0; i < cover.size(); ++i) {
                for (S2CellId id = cover[i].parent(); id.level() >= coverer->min_level();
                        id = id.parent()) {
                    parents.insert(id);
                }
            }

            for (set<S2CellId>::const_iterator it = parents.begin(); it != parents.end(); ++it) {
                inArrayBuilder.append(myitoa(arrayPos++), it->toString());
            }
        }

        inArrayBuilder.done();
        inBuilder.done();
        return queryBuilder.obj();
    }

    string QueryGeometry::toString() const {
        stringstream ss;
        ss << "field = " << field;
        if (NULL != cell.get()) {
            ss << ", cell";
        } else if (NULL != line.get()) {
            ss << ", line = ";
        } else if (NULL != polygon.get()) {
            ss << ", polygon = ";
        }
        return ss.str();
    }

    bool QueryGeometry::parseFrom(const BSONObj& obj) {
        if (GeoParser::isGeoJSONPolygon(obj)) {
            // We can't really pass these things around willy-nilly except by ptr.
            polygon.reset(new S2Polygon());
            GeoParser::parseGeoJSONPolygon(obj, polygon.get());
        } else if (GeoParser::isPoint(obj)) {
            cell.reset(new S2Cell());
            GeoParser::parsePoint(obj, cell.get());
        } else if (GeoParser::isLineString(obj)) {
            line.reset(new S2Polyline());
            GeoParser::parseLineString(obj, line.get());
        } else {
            return false;
        }
        return true;
    }

    // Does this (QueryGeometry) intersect the provided data?
    bool QueryGeometry::intersectsPoint(const S2Cell &otherPoint) {
        if (NULL != cell) {
            return cell->MayIntersect(otherPoint);
        } else if (NULL != line) {
            return line->MayIntersect(otherPoint);
        } else {
            return polygon->MayIntersect(otherPoint);
        }
    }

    bool QueryGeometry::intersectsLine(const S2Polyline& otherLine) {
        if (NULL != cell) {
            return otherLine.MayIntersect(*cell);
        } else if (NULL != line) {
            return otherLine.Intersects(line.get());
        } else {
            // TODO(hk): modify s2 library to just let us know if it intersected
            // rather than returning all this.
            vector<S2Polyline*> clipped;
            polygon->IntersectWithPolyline(&otherLine, &clipped);
            bool ret = clipped.size() > 0;
            for (size_t i = 0; i < clipped.size(); ++i) delete clipped[i];
            return ret;
        }
        return false;
    }

    bool QueryGeometry::intersectsPolygon(const S2Polygon& otherPolygon) {
        if (NULL != cell) {
            return otherPolygon.MayIntersect(*cell);
        } else if (NULL != line) {
            // TODO(hk): modify s2 library to just let us know if it intersected
            // rather than returning all this.
            vector<S2Polyline*> clipped;
            otherPolygon.IntersectWithPolyline(line.get(), &clipped);
            bool ret = clipped.size() > 0;
            for (size_t i = 0; i < clipped.size(); ++i) delete clipped[i];
            return ret;
        } else {
            return otherPolygon.Intersects(polygon.get());
        }
    }

    S2Point QueryGeometry::getCentroid() const {
        // TODO(hk): If the projection is planar this isn't valid.  Fix this
        // when we actually use planar projections, or remove planar projections
        // from the code.
        if (NULL != cell) {
            return cell->GetCenter();
        } else if (NULL != line) {
            return line->GetCentroid();
        } else {
            verify(NULL != polygon);
            return polygon->GetCentroid();
        }
    }

    const S2Region& QueryGeometry::getRegion() const {
        if (NULL != cell) {
            return *cell;
        } else if (NULL != line) {
            return *line;
        } else {
            verify(NULL != polygon);
            return *polygon;
        }
    }
}  // namespace mongo
