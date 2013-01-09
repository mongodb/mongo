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

    BSONObj S2SearchUtil::coverAsBSON(const vector<S2CellId> &cover, const string& field,
                                      const int coarsestIndexedLevel) {
        BSONObjBuilder queryBuilder;
        BSONObjBuilder inBuilder(queryBuilder.subobjStart(field));
        // To have an array where elements of that array are regexes, we have to do this.
        BSONObjBuilder inArrayBuilder(inBuilder.subarrayStart("$in"));
        // Sadly we must keep track of this ourselves.  Oh, BSONObjBuilder, you rascal!
        int arrayPos = 0;

        bool considerCoarser = false;

        // Look at the cells we cover and all cells that are within our covering and
        // finer.  Anything with our cover as a strict prefix is contained within the cover and
        // should be intersection tested.
        for (size_t i = 0; i < cover.size(); ++i) {
            // First argument is position in the array as a string.
            // Third argument is options to regex.
            inArrayBuilder.appendRegex(myitoa(arrayPos++), "^" + cover[i].toString(), "");
            // If any of our covers could be covered by something in the index, we have
            // to look at things coarser.
            considerCoarser = considerCoarser || (cover[i].level() > coarsestIndexedLevel);
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
                for (S2CellId id = cover[i].parent(); id.level() >= coarsestIndexedLevel;
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
        ss << ", predicate = " << ((WITHIN == predicate) ? "within" : "intersect");
        if (NULL != cell.get()) {
            ss << ", cell";
        } else if (NULL != line.get()) {
            ss << ", line";
        } else if (NULL != polygon.get()) {
            ss << ", polygon";
        }
        return ss.str();
    }

    bool QueryGeometry::satisfiesPredicate(const BSONObj &obj) {
        verify(predicate == WITHIN || predicate == INTERSECT);

        if (GeoParser::isPolygon(obj)) {
            S2Polygon shape;
            GeoParser::parsePolygon(obj, &shape);
            if (WITHIN == predicate) {
                return isWithin(shape);
            } else {
                return intersects(shape);
            }
        } else if (GeoParser::isLineString(obj)) {
            S2Polyline shape;
            GeoParser::parseLineString(obj, &shape);
            if (WITHIN == predicate) {
                return isWithin(shape);
            } else {
                return intersects(shape);
            }
        } else if (GeoParser::isPoint(obj)) {
            S2Cell point;
            GeoParser::parsePoint(obj, &point);
            if (WITHIN == predicate) {
                return isWithin(point);
            } else {
                return intersects(point);
            }
        }
        return false;
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

    // Is the geometry provided as an argument within our query geometry?
    bool QueryGeometry::isWithin(const S2Cell &otherPoint) {
        // Intersecting a point is containing a point.  Hooray!
        return intersects(otherPoint);
    }

    bool QueryGeometry::isWithin(const S2Polyline& otherLine) {
        if (NULL != cell) {
            // Points don't contain lines.
            return false;
        } else if (NULL != line) {
            // Doing line-in-line is scary.
            return false;
        } else {
            // Kind of a mess.  We get a function for clipping the line to the
            // polygon.  We do this and make sure the line is the same as the
            // line we're clipping against.
            vector<S2Polyline*> clipped;
            polygon->IntersectWithPolyline(&otherLine, &clipped);
            if (1 != clipped.size()) { return false; }
            // If the line is entirely contained within the polygon, we should be
            // getting it back verbatim, so really there should be no error.
            bool ret = clipped[0]->NearlyCoversPolyline(otherLine, S1Angle::Degrees(1e-10));
            for (size_t i = 0; i < clipped.size(); ++i) delete clipped[i];
            return ret;
        }
    }

    bool QueryGeometry::isWithin(const S2Polygon& otherPolygon) {
        if (NULL != cell) {
            // Points don't contain polygons.
            return false;
        } else if (NULL != line) {
            // Lines don't contain polygons
            return false;
        } else {
            return polygon->Contains(&otherPolygon);
        }
    }

    // Does this (QueryGeometry) intersect the provided data?
    bool QueryGeometry::intersects(const S2Cell &otherPoint) {
        if (NULL != cell) {
            return cell->MayIntersect(otherPoint);
        } else if (NULL != line) {
            return line->MayIntersect(otherPoint);
        } else {
            return polygon->MayIntersect(otherPoint);
        }
    }

    bool QueryGeometry::intersects(const S2Polyline& otherLine) {
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
    }

    bool QueryGeometry::intersects(const S2Polygon& otherPolygon) {
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
