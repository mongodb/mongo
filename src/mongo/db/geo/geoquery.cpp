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

#include "mongo/db/geo/geoquery.h"

#ifdef _WIN32
#include <float.h>
#define nextafter _nextafter
#else
#include <cmath> // nextafter
#endif

#include "mongo/db/geo/geoconstants.h"

namespace mongo {

    bool NearQuery::parseFromGeoNear(const BSONObj &obj, double radius) {
        if (obj["near"].eoo()) { return false; }
        BSONObj nearObj = obj["near"].embeddedObject();

        if (!GeoParser::isPoint(nearObj) || !GeoParser::parsePoint(nearObj, &centroid)) {
            return false;
        }

        // The CRS for the legacy points dictates that distances are in radians.
        fromRadians = (FLAT == centroid.crs);

        if (!obj["minDistance"].eoo()) {
            uassert(17035, "minDistance must be a number", obj["minDistance"].isNumber());
            double distArg = obj["minDistance"].number();
            uassert(16901, "minDistance must be non-negative", distArg >= 0.0);
            if (fromRadians) {
                minDistance = distArg * radius;
            } else {
                minDistance = distArg;
            }
        }

        if (!obj["maxDistance"].eoo()) {
            uassert(17036, "maxDistance must be a number", obj["maxDistance"].isNumber());
            double distArg = obj["maxDistance"].number();
            uassert(16902, "maxDistance must be non-negative", distArg >= 0.0);
            if (fromRadians) {
                maxDistance = distArg * radius;
            } else {
                maxDistance = distArg;
            }

            uassert(17037, "maxDistance too large",
                    maxDistance <= nextafter(M_PI * radius, DBL_MAX));
        }
        return true;
    }

    bool NearQuery::parseFrom(const BSONObj &obj) {
        bool hasGeometry = false;
        bool hasMaxDistance = false;

        // First, try legacy near, e.g.:
        // t.find({ loc : { $nearSphere: [0,0], $minDistance: 1, $maxDistance: 3 }})
        // t.find({ loc : { $nearSphere: [0,0] }})
        // t.find({ loc : { $near: { someGeoJSONPoint}})
        BSONObjIterator it(obj);
        while (it.more()) {
            BSONElement e = it.next();
            bool isNearSphere = mongoutils::str::equals(e.fieldName(), "$nearSphere");
            bool isMinDistance = mongoutils::str::equals(e.fieldName(), "$minDistance");
            bool isMaxDistance = mongoutils::str::equals(e.fieldName(), "$maxDistance");
            bool isNear = mongoutils::str::equals(e.fieldName(), "$near")
                          || mongoutils::str::equals(e.fieldName(), "$geoNear");
            if (isNearSphere || isNear) {
                if (!e.isABSONObj()) { return false; }
                BSONObj embeddedObj = e.embeddedObject();

                if (!GeoParser::isPoint(embeddedObj)) { continue; }
                if (!GeoParser::parsePoint(embeddedObj, &centroid)) { return false; }

                if (isNearSphere) {
                    fromRadians = (centroid.crs == FLAT);
                    hasGeometry = true;
                } else if (isNear && (centroid.crs == SPHERE)) {
                    // We don't accept $near : [oldstylepoint].
                    hasGeometry = true;
                }
            } else if (isMinDistance) {
                uassert(16893, "$minDistance must be a number", e.isNumber());
                minDistance = e.Number();
                uassert(16894, "$minDistance must be non-negative", minDistance >= 0.0);
            } else if (isMaxDistance) {
                uassert(16895, "$maxDistance must be a number", e.isNumber());
                maxDistance = e.Number();
                uassert(16896, "$maxDistance must be non-negative", maxDistance >= 0.0);
                hasMaxDistance = true;
            }
        }

        // Add fudge to maxValidDistance so we don't throw when the provided maxDistance
        // is on the edge.
        double maxValidDistance = nextafter(fromRadians ?
                                            M_PI :
                                            kRadiusOfEarthInMeters * M_PI, DBL_MAX);

        uassert(17038, "$minDistance too large", minDistance < maxValidDistance);
        uassert(17039, "$maxDistance too large",
                !hasMaxDistance || maxDistance <= maxValidDistance);

        if (hasGeometry) { return true; }

        // Next, try "new" near:
        // t.find({"geo" : {"$near" : {"$geometry": pointA, $minDistance: 1, $maxDistance: 3}}})
        BSONElement e = obj.firstElement();
        if (!e.isABSONObj()) { return false; }
        BSONObj::MatchType matchType = static_cast<BSONObj::MatchType>(e.getGtLtOp());
        if (BSONObj::opNEAR != matchType) { return false; }

        // Restart it.
        it = BSONObjIterator(e.embeddedObject());
        while (it.more()) {
            BSONElement e = it.next();
            if (mongoutils::str::equals(e.fieldName(), "$geometry")) {
                if (e.isABSONObj()) {
                    BSONObj embeddedObj = e.embeddedObject();
                    uassert(16885, "$near requires a point, given " + embeddedObj.toString(),
                            GeoParser::isPoint(embeddedObj));
                    if (!GeoParser::parsePoint(embeddedObj, &centroid)) { return false; }
                    uassert(16681, "$near requires geojson point, given " + embeddedObj.toString(),
                            (SPHERE == centroid.crs));
                    hasGeometry = true;
                }
            } else if (mongoutils::str::equals(e.fieldName(), "$minDistance")) {
                uassert(16897, "$minDistance must be a number", e.isNumber());
                minDistance = e.Number();
                uassert(16898, "$minDistance must be non-negative", minDistance >= 0.0);
                uassert(17084, "$minDistance too large", minDistance < maxValidDistance);
            } else if (mongoutils::str::equals(e.fieldName(), "$maxDistance")) {
                uassert(16899, "$maxDistance must be a number", e.isNumber());
                maxDistance = e.Number();
                uassert(16900, "$maxDistance must be non-negative", maxDistance >= 0.0);
                uassert(16992, "$maxDistance too large", maxDistance <= maxValidDistance);
            }
        }
        return hasGeometry;
    }

    bool GeoQuery::parseLegacyQuery(const BSONObj &obj) {
        // Legacy within parsing #1: t.find({ loc : [0,0] }) This is should be
        // point-only.  We tag it as intersect and limit $within to
        // space-containing geometry.
        if (GeoParser::isPoint(obj) && geoContainer.parseFrom(obj)) {
            predicate = GeoQuery::INTERSECT;
            return true;
        }

        BSONObjIterator it(obj);
        if (!it.more()) { return false; }
        BSONElement e = it.next();
        if (!e.isABSONObj()) { return false; }
        BSONObj embeddedObj = e.embeddedObject();
        // Legacy within #2 : t.find({ loc : { $within : { $box/etc : ...
        bool contains = (BSONObj::opWITHIN == static_cast<BSONObj::MatchType>(e.getGtLtOp()));
        if (contains && geoContainer.parseFrom(embeddedObj)) {
            predicate = GeoQuery::WITHIN;
            return true;
        }

        return false;
    }

    bool GeoQuery::parseNewQuery(const BSONObj &obj) {
        // pointA = { "type" : "Point", "coordinates": [ 40, 5 ] }
        // t.find({ "geo" : { "$intersect" : { "$geometry" : pointA} } })
        // t.find({ "geo" : { "$within" : { "$geometry" : polygon } } })
        // where field.name is "geo"
        BSONElement e = obj.firstElement();
        if (!e.isABSONObj()) { return false; }

        BSONObj::MatchType matchType = static_cast<BSONObj::MatchType>(e.getGtLtOp());
        if (BSONObj::opGEO_INTERSECTS == matchType) {
            predicate = GeoQuery::INTERSECT;
        } else if (BSONObj::opWITHIN == matchType) {
            predicate = GeoQuery::WITHIN;
        } else {
            return false;
        }

        bool hasGeometry = false;
        BSONObjIterator argIt(e.embeddedObject());
        while (argIt.more()) {
            BSONElement e = argIt.next();
            if (mongoutils::str::equals(e.fieldName(), "$geometry")) {
                if (e.isABSONObj()) {
                    BSONObj embeddedObj = e.embeddedObject();
                     if (geoContainer.parseFrom(embeddedObj)) {
                         hasGeometry = true;
                     }
                }
            }
        }

        // Don't want to give the error below if we could not pull any geometry out.
        if (!hasGeometry) { return false; }

        if (GeoQuery::WITHIN == predicate) {
            // Why do we only deal with $within {polygon}?
            // 1. Finding things within a point is silly and only valid
            // for points and degenerate lines/polys.
            //
            // 2. Finding points within a line is easy but that's called intersect.
            // Finding lines within a line is kind of tricky given what S2 gives us.
            // Doing line-within-line is a valid yet unsupported feature,
            // though I wonder if we want to preserve orientation for lines or
            // allow (a,b),(c,d) to be within (c,d),(a,b).  Anyway, punt on
            // this for now.
            uassert(16672, "$within not supported with provided geometry: " + obj.toString(),
                    geoContainer.supportsContains());
        }

        return hasGeometry;
    }

    bool GeoQuery::parseFrom(const BSONObj &obj) {
        return parseLegacyQuery(obj) || parseNewQuery(obj);
    }

    const S2Region& GeoQuery::getRegion() const {
        return geoContainer.getRegion();
    }

    bool GeoQuery::hasS2Region() const {
        return geoContainer.hasS2Region();
    }

    bool GeometryContainer::supportsContains() const {
        return NULL != _polygon
               || NULL != _cap
               || NULL != _multiPolygon
               || (NULL != _geometryCollection
                   && (_geometryCollection->polygons.vector().size() > 0
                       || _geometryCollection->multiPolygons.vector().size() > 0));
    }

    bool GeometryContainer::hasS2Region() const {
        return NULL != _point
               || NULL != _line
               || (NULL != _polygon && _polygon->crs == SPHERE)
               || (NULL != _cap && _cap->crs == SPHERE)
               || NULL != _multiPoint
               || NULL != _multiLine
               || NULL != _multiPolygon
               || NULL != _geometryCollection;
    }

    bool GeoQuery::satisfiesPredicate(const GeometryContainer &otherContainer) const {
        verify(predicate == WITHIN || predicate == INTERSECT);

        if (WITHIN == predicate) {
            return geoContainer.contains(otherContainer);
        } else {
            return geoContainer.intersects(otherContainer);
        }
    }

    bool GeometryContainer::contains(const GeometryContainer& otherContainer) const {
        // First let's deal with the case where we are FLAT.
        if (NULL != _polygon && (FLAT == _polygon->crs)) {
            if (NULL == otherContainer._point) { return false; }
            return _polygon->oldPolygon.contains(otherContainer._point->oldPoint);
        }

        if (NULL != _box) {
            verify(FLAT == _box->crs);
            if (NULL == otherContainer._point) { return false; }
            return _box->box.inside(otherContainer._point->oldPoint);
        }

        if (NULL != _cap && (FLAT == _cap->crs)) {
            if (NULL == otherContainer._point) { return false; }
            // Let's be as consistent epsilon-wise as we can with the '2d' indextype.
            return distanceWithin(_cap->circle.center, otherContainer._point->oldPoint,
                                  _cap->circle.radius);
        }

        // Now we deal with all the SPHERE stuff.

        // Iterate over the other thing and see if we contain it all.
        if (NULL != otherContainer._point) {
            return contains(otherContainer._point->cell, otherContainer._point->point);
        }

        if (NULL != otherContainer._line) {
            return contains(otherContainer._line->line);
        }

        if (NULL != otherContainer._polygon) {
            return contains(otherContainer._polygon->polygon);
        }

        if (NULL != otherContainer._multiPoint) {
            for (size_t i = 0; i < otherContainer._multiPoint->points.size(); ++i) {
                if (!contains(otherContainer._multiPoint->cells[i],
                              otherContainer._multiPoint->points[i])) {
                    return false;
                }
            }
            return true;
        }

        if (NULL != otherContainer._multiLine) {
            const vector<S2Polyline*>& lines = otherContainer._multiLine->lines.vector();
            for (size_t i = 0; i < lines.size(); ++i) {
                if (!contains(*lines[i])) { return false; }
            }
            return true;
        }

        if (NULL != otherContainer._multiPolygon) {
            const vector<S2Polygon*>& polys = otherContainer._multiPolygon->polygons.vector();
            for (size_t i = 0; i < polys.size(); ++i) {
                if (!contains(*polys[i])) { return false; }
            }
            return true;
        }

        if (NULL != otherContainer._geometryCollection) {
            GeometryCollection& c = *otherContainer._geometryCollection;

            for (size_t i = 0; i < c.points.size(); ++i) {
                if (!contains(c.points[i].cell, c.points[i].point)) {
                    return false;
                }
            }

            const vector<LineWithCRS*>& lines = c.lines.vector();
            for (size_t i = 0; i < lines.size(); ++i) {
                if (!contains(lines[i]->line)) { return false; }
            }

            const vector<PolygonWithCRS*>& polys = c.polygons.vector();
            for (size_t i = 0; i < polys.size(); ++i) {
                if (!contains(polys[i]->polygon)) { return false; }
            }

            const vector<MultiPointWithCRS*>& multipoints = c.multiPoints.vector();
            for (size_t i = 0; i < multipoints.size(); ++i) {
                MultiPointWithCRS* mp = multipoints[i];
                for (size_t j = 0; j < mp->points.size(); ++j) {
                    if (!contains(mp->cells[j], mp->points[j])) { return false; }
                }
            }

            const vector<MultiLineWithCRS*>& multilines = c.multiLines.vector();
            for (size_t i = 0; i < multilines.size(); ++i) {
                const vector<S2Polyline*>& lines = multilines[i]->lines.vector();
                for (size_t j = 0; j < lines.size(); ++j) {
                    if (!contains(*lines[j])) { return false; }
                }
            }

            const vector<MultiPolygonWithCRS*>& multipolys = c.multiPolygons.vector();
            for (size_t i = 0; i < multipolys.size(); ++i) {
                const vector<S2Polygon*>& polys = multipolys[i]->polygons.vector();
                for (size_t j = 0; j < polys.size(); ++j) {
                    if (!contains(*polys[j])) { return false; }
                }
            }

            return true;
        }

        return false;
    }

    bool containsPoint(const S2Polygon& poly, const S2Cell& otherCell, const S2Point& otherPoint) {
        // This is much faster for actual containment checking.
        if (poly.Contains(otherPoint)) { return true; }
        // This is slower but contains edges/vertices.
        return poly.MayIntersect(otherCell);
    }

    bool GeometryContainer::contains(const S2Cell& otherCell, const S2Point& otherPoint) const {
        if (NULL != _polygon && (_polygon->crs == SPHERE)) {
            return containsPoint(_polygon->polygon, otherCell, otherPoint);
        }

        if (NULL != _cap && (_cap->crs == SPHERE)) {
            return _cap->cap.MayIntersect(otherCell);
        }

        if (NULL != _multiPolygon) {
            const vector<S2Polygon*>& polys = _multiPolygon->polygons.vector();
            for (size_t i = 0; i < polys.size(); ++i) {
                if (containsPoint(*polys[i], otherCell, otherPoint)) { return true; }
            }
        }

        if (NULL != _geometryCollection) {
            const vector<PolygonWithCRS*>& polys = _geometryCollection->polygons.vector();
            for (size_t i = 0; i < polys.size(); ++i) {
                if (containsPoint(polys[i]->polygon, otherCell, otherPoint)) { return true; }
            }
            
            const vector<MultiPolygonWithCRS*>& multipolys =_geometryCollection->multiPolygons.vector();
            for (size_t i = 0; i < multipolys.size(); ++i) {
                const vector<S2Polygon*>& innerpolys = multipolys[i]->polygons.vector();
                for (size_t j = 0; j < innerpolys.size(); ++j) {
                    if (containsPoint(*innerpolys[j], otherCell, otherPoint)) { return true; }
                }
            }
        }

        return false;
    }

    bool containsLine(const S2Polygon& poly, const S2Polyline& otherLine) {
        // Kind of a mess.  We get a function for clipping the line to the
        // polygon.  We do this and make sure the line is the same as the
        // line we're clipping against.
        vector<S2Polyline*> clipped;
        poly.IntersectWithPolyline(&otherLine, &clipped);
        if (1 != clipped.size()) { return false; }
        // If the line is entirely contained within the polygon, we should be
        // getting it back verbatim, so really there should be no error.
        bool ret = clipped[0]->NearlyCoversPolyline(otherLine,
                S1Angle::Degrees(1e-10));
        for (size_t i = 0; i < clipped.size(); ++i) delete clipped[i];
        return ret;
    }

    bool GeometryContainer::contains(const S2Polyline& otherLine) const {
        if (NULL != _polygon && (_polygon->crs == SPHERE)) {
            return containsLine(_polygon->polygon, otherLine);
        }

        if (NULL != _multiPolygon) {
            const vector<S2Polygon*>& polys = _multiPolygon->polygons.vector();
            for (size_t i = 0; i < polys.size(); ++i) {
                if (containsLine(*polys[i], otherLine)) { return true; }
            }
        }

        if (NULL != _geometryCollection) {
            const vector<PolygonWithCRS*>& polys = _geometryCollection->polygons.vector();
            for (size_t i = 0; i < polys.size(); ++i) {
                if (containsLine(polys[i]->polygon, otherLine)) { return true; }
            }
            
            const vector<MultiPolygonWithCRS*>& multipolys =_geometryCollection->multiPolygons.vector();
            for (size_t i = 0; i < multipolys.size(); ++i) {
                const vector<S2Polygon*>& innerpolys = multipolys[i]->polygons.vector();
                for (size_t j = 0; j < innerpolys.size(); ++j) {
                    if (containsLine(*innerpolys[j], otherLine)) { return true; }
                }
            }
        }

        return false;
    }

    bool containsPolygon(const S2Polygon& poly, const S2Polygon& otherPoly) {
        return poly.Contains(&otherPoly);
    }

    bool GeometryContainer::contains(const S2Polygon& otherPolygon) const {
        if (NULL != _polygon && (_polygon->crs == SPHERE)) {
            return containsPolygon(_polygon->polygon, otherPolygon);
        }

        if (NULL != _multiPolygon) {
            const vector<S2Polygon*>& polys = _multiPolygon->polygons.vector();
            for (size_t i = 0; i < polys.size(); ++i) {
                if (containsPolygon(*polys[i], otherPolygon)) { return true; }
            }
        }

        if (NULL != _geometryCollection) {
            const vector<PolygonWithCRS*>& polys = _geometryCollection->polygons.vector();
            for (size_t i = 0; i < polys.size(); ++i) {
                if (containsPolygon(polys[i]->polygon, otherPolygon)) { return true; }
            }
            
            const vector<MultiPolygonWithCRS*>& multipolys =_geometryCollection->multiPolygons.vector();
            for (size_t i = 0; i < multipolys.size(); ++i) {
                const vector<S2Polygon*>& innerpolys = multipolys[i]->polygons.vector();
                for (size_t j = 0; j < innerpolys.size(); ++j) {
                    if (containsPolygon(*innerpolys[j], otherPolygon)) { return true; }
                }
            }
        }

        return false;
    }

    bool GeometryContainer::intersects(const GeometryContainer& otherContainer) const {
        if (NULL != otherContainer._point) {
            return intersects(otherContainer._point->cell);
        } else if (NULL != otherContainer._line) {
            return intersects(otherContainer._line->line);
        } else if (NULL != otherContainer._polygon) {
            if (SPHERE != otherContainer._polygon->crs) { return false; }
            return intersects(otherContainer._polygon->polygon);
        } else if (NULL != otherContainer._multiPoint) {
            return intersects(*otherContainer._multiPoint);
        } else if (NULL != otherContainer._multiLine) {
            return intersects(*otherContainer._multiLine);
        } else if (NULL != otherContainer._multiPolygon) {
            return intersects(*otherContainer._multiPolygon);
        } else if (NULL != otherContainer._geometryCollection) {
            const GeometryCollection& c = *otherContainer._geometryCollection;

            for (size_t i = 0; i < c.points.size(); ++i) {
                if (intersects(c.points[i].cell)) { return true; }
            }

            for (size_t i = 0; i < c.polygons.vector().size(); ++i) {
                if (intersects(c.polygons.vector()[i]->polygon)) { return true; }
            }

            for (size_t i = 0; i < c.lines.vector().size(); ++i) {
                if (intersects(c.lines.vector()[i]->line)) { return true; }
            }

            for (size_t i = 0; i < c.multiPolygons.vector().size(); ++i) {
                if (intersects(*c.multiPolygons.vector()[i])) { return true; }
            }

            for (size_t i = 0; i < c.multiLines.vector().size(); ++i) {
                if (intersects(*c.multiLines.vector()[i])) { return true; }
            }

            for (size_t i = 0; i < c.multiPoints.vector().size(); ++i) {
                if (intersects(*c.multiPoints.vector()[i])) { return true; }
            }
        }

        return false;
    }

    bool GeometryContainer::intersects(const MultiPointWithCRS& otherMultiPoint) const {
        for (size_t i = 0; i < otherMultiPoint.cells.size(); ++i) {
            if (intersects(otherMultiPoint.cells[i])) { return true; }
        }
        return false;
    }

    bool GeometryContainer::intersects(const MultiLineWithCRS& otherMultiLine) const {
        for (size_t i = 0; i < otherMultiLine.lines.vector().size(); ++i) {
            if (intersects(*otherMultiLine.lines.vector()[i])) { return true; }
        }
        return false;
    }

    bool GeometryContainer::intersects(const MultiPolygonWithCRS& otherMultiPolygon) const {
        for (size_t i = 0; i < otherMultiPolygon.polygons.vector().size(); ++i) {
            if (intersects(*otherMultiPolygon.polygons.vector()[i])) { return true; }
        }
        return false;
    }

    // Does this (GeometryContainer) intersect the provided data?
    bool GeometryContainer::intersects(const S2Cell &otherPoint) const {
        if (NULL != _point) {
            return _point->cell.MayIntersect(otherPoint);
        } else if (NULL != _line) {
            return _line->line.MayIntersect(otherPoint);
        } else if (NULL != _polygon) {
            return _polygon->polygon.MayIntersect(otherPoint);
        } else if (NULL != _multiPoint) {
            const vector<S2Cell>& cells = _multiPoint->cells;
            for (size_t i = 0; i < cells.size(); ++i) {
                if (cells[i].MayIntersect(otherPoint)) { return true; }
            }
        } else if (NULL != _multiLine) {
            const vector<S2Polyline*>& lines = _multiLine->lines.vector();
            for (size_t i = 0; i < lines.size(); ++i) {
                if (lines[i]->MayIntersect(otherPoint)) { return true; }
            }
        } else if (NULL != _multiPolygon) {
            const vector<S2Polygon*>& polys = _multiPolygon->polygons.vector();
            for (size_t i = 0; i < polys.size(); ++i) {
                if (polys[i]->MayIntersect(otherPoint)) { return true; }
            }
        } else if (NULL != _geometryCollection) {
            const GeometryCollection& c = *_geometryCollection;

            for (size_t i = 0; i < c.points.size(); ++i) {
                if (c.points[i].cell.MayIntersect(otherPoint)) { return true; }
            }

            for (size_t i = 0; i < c.polygons.vector().size(); ++i) {
                if (c.polygons.vector()[i]->polygon.MayIntersect(otherPoint)) { return true; }
            }

            for (size_t i = 0; i < c.lines.vector().size(); ++i) {
                if (c.lines.vector()[i]->line.MayIntersect(otherPoint)) { return true; }
            }

            for (size_t i = 0; i < c.multiPolygons.vector().size(); ++i) {
                const vector<S2Polygon*>& innerPolys =
                    c.multiPolygons.vector()[i]->polygons.vector();
                for (size_t j = 0; j < innerPolys.size(); ++j) {
                    if (innerPolys[j]->MayIntersect(otherPoint)) { return true; }
                }
            }

            for (size_t i = 0; i < c.multiLines.vector().size(); ++i) {
                const vector<S2Polyline*>& innerLines =
                    c.multiLines.vector()[i]->lines.vector();
                for (size_t j = 0; j < innerLines.size(); ++j) {
                    if (innerLines[j]->MayIntersect(otherPoint)) { return true; }
                }
            }

            for (size_t i = 0; i < c.multiPoints.vector().size(); ++i) {
                const vector<S2Cell>& innerCells = c.multiPoints.vector()[i]->cells;
                for (size_t j = 0; j < innerCells.size(); ++j) {
                    if (innerCells[j].MayIntersect(otherPoint)) { return true; }
                }
            }
        }

        return false;
    }

    bool polygonLineIntersection(const S2Polyline& line, const S2Polygon& poly) {
        // TODO(hk): modify s2 library to just let us know if it intersected
        // rather than returning all this.
        vector<S2Polyline*> clipped;
        poly.IntersectWithPolyline(&line, &clipped);
        bool ret = clipped.size() > 0;
        for (size_t i = 0; i < clipped.size(); ++i) delete clipped[i];
        return ret;
    }

    bool GeometryContainer::intersects(const S2Polyline& otherLine) const {
        if (NULL != _point) {
            return otherLine.MayIntersect(_point->cell);
        } else if (NULL != _line) {
            return otherLine.Intersects(&_line->line);
        } else if (NULL != _polygon && (_polygon->crs == SPHERE)) {
            return polygonLineIntersection(otherLine, _polygon->polygon);
        } else if (NULL != _multiPoint) {
            for (size_t i = 0; i < _multiPoint->cells.size(); ++i) {
                if (otherLine.MayIntersect(_multiPoint->cells[i])) { return true; }
            }
        } else if (NULL != _multiLine) {
            for (size_t i = 0; i < _multiLine->lines.vector().size(); ++i) {
                if (otherLine.Intersects(_multiLine->lines.vector()[i])) {
                    return true;
                }
            }
        } else if (NULL != _multiPolygon) {
            for (size_t i = 0; i < _multiPolygon->polygons.vector().size(); ++i) {
                if (polygonLineIntersection(otherLine, *_multiPolygon->polygons.vector()[i])) {
                    return true;
                }
            }
        } else if (NULL != _geometryCollection) {
            const GeometryCollection& c = *_geometryCollection;

            for (size_t i = 0; i < c.points.size(); ++i) {
                if (otherLine.MayIntersect(c.points[i].cell)) { return true; }
            }

            for (size_t i = 0; i < c.polygons.vector().size(); ++i) {
                if (polygonLineIntersection(otherLine, c.polygons.vector()[i]->polygon)) {
                    return true;
                }
            }

            for (size_t i = 0; i < c.lines.vector().size(); ++i) {
                if (c.lines.vector()[i]->line.Intersects(&otherLine)) { return true; }
            }

            for (size_t i = 0; i < c.multiPolygons.vector().size(); ++i) {
                const vector<S2Polygon*>& innerPolys =
                    c.multiPolygons.vector()[i]->polygons.vector();
                for (size_t j = 0; j < innerPolys.size(); ++j) {
                    if (polygonLineIntersection(otherLine, *innerPolys[j])) {
                        return true;
                    }
                }
            }

            for (size_t i = 0; i < c.multiLines.vector().size(); ++i) {
                const vector<S2Polyline*>& innerLines =
                    c.multiLines.vector()[i]->lines.vector();
                for (size_t j = 0; j < innerLines.size(); ++j) {
                    if (innerLines[j]->Intersects(&otherLine)) { return true; }
                }
            }

            for (size_t i = 0; i < c.multiPoints.vector().size(); ++i) {
                const vector<S2Cell>& innerCells = c.multiPoints.vector()[i]->cells;
                for (size_t j = 0; j < innerCells.size(); ++j) {
                    if (otherLine.MayIntersect(innerCells[j])) { return true; }
                }
            }
        }

        return false;
    }

    // Does 'this' intersect with the provided polygon?
    bool GeometryContainer::intersects(const S2Polygon& otherPolygon) const {
        if (NULL != _point) {
            return otherPolygon.MayIntersect(_point->cell);
        } else if (NULL != _line) {
            return polygonLineIntersection(_line->line, otherPolygon);
        } else if (NULL != _polygon) {
            return otherPolygon.Intersects(&_polygon->polygon);
        } else if (NULL != _multiPoint) {
            for (size_t i = 0; i < _multiPoint->cells.size(); ++i) {
                if (otherPolygon.MayIntersect(_multiPoint->cells[i])) { return true; }
            }
        } else if (NULL != _multiLine) {
            for (size_t i = 0; i < _multiLine->lines.vector().size(); ++i) {
                if (polygonLineIntersection(*_multiLine->lines.vector()[i], otherPolygon)) {
                    return true;
                }
            }
        } else if (NULL != _multiPolygon) {
            for (size_t i = 0; i < _multiPolygon->polygons.vector().size(); ++i) {
                if (otherPolygon.Intersects(_multiPolygon->polygons.vector()[i])) {
                    return true;
                }
            }
        } else if (NULL != _geometryCollection) {
            const GeometryCollection& c = *_geometryCollection;

            for (size_t i = 0; i < c.points.size(); ++i) {
                if (otherPolygon.MayIntersect(c.points[i].cell)) { return true; }
            }

            for (size_t i = 0; i < c.polygons.vector().size(); ++i) {
                if (otherPolygon.Intersects(&c.polygons.vector()[i]->polygon)) {
                    return true;
                }
            }

            for (size_t i = 0; i < c.lines.vector().size(); ++i) {
                if (polygonLineIntersection(c.lines.vector()[i]->line, otherPolygon)) {
                    return true;
                }
            }

            for (size_t i = 0; i < c.multiPolygons.vector().size(); ++i) {
                const vector<S2Polygon*>& innerPolys =
                    c.multiPolygons.vector()[i]->polygons.vector();
                for (size_t j = 0; j < innerPolys.size(); ++j) {
                    if (otherPolygon.Intersects(innerPolys[j])) {
                        return true;
                    }
                }
            }

            for (size_t i = 0; i < c.multiLines.vector().size(); ++i) {
                const vector<S2Polyline*>& innerLines =
                    c.multiLines.vector()[i]->lines.vector();
                for (size_t j = 0; j < innerLines.size(); ++j) {
                    if (polygonLineIntersection(*innerLines[j], otherPolygon)) {
                        return true;
                    }
                }
            }

            for (size_t i = 0; i < c.multiPoints.vector().size(); ++i) {
                const vector<S2Cell>& innerCells = c.multiPoints.vector()[i]->cells;
                for (size_t j = 0; j < innerCells.size(); ++j) {
                    if (otherPolygon.MayIntersect(innerCells[j])) {
                        return true;
                    }
                }
            }
        }

        return false;
    }

    bool GeometryContainer::parseFrom(const BSONObj& obj) {
        *this = GeometryContainer();

        if (GeoParser::isPolygon(obj)) {
            // We can't really pass these things around willy-nilly except by ptr.
            _polygon.reset(new PolygonWithCRS());
            if (!GeoParser::parsePolygon(obj, _polygon.get())) { return false; }
        } else if (GeoParser::isPoint(obj)) {
            _point.reset(new PointWithCRS());
            if (!GeoParser::parsePoint(obj, _point.get())) { return false; }
        } else if (GeoParser::isLine(obj)) {
            _line.reset(new LineWithCRS());
            if (!GeoParser::parseLine(obj, _line.get())) { return false; }
        } else if (GeoParser::isBox(obj)) {
            _box.reset(new BoxWithCRS());
            if (!GeoParser::parseBox(obj, _box.get())) { return false; }
        } else if (GeoParser::isCap(obj)) {
            _cap.reset(new CapWithCRS());
            if (!GeoParser::parseCap(obj, _cap.get())) { return false; }
        } else if (GeoParser::isMultiPoint(obj)) {
            _multiPoint.reset(new MultiPointWithCRS());
            if (!GeoParser::parseMultiPoint(obj, _multiPoint.get())) { return false; }
            _region.reset(new S2RegionUnion());
            for (size_t i = 0; i < _multiPoint->cells.size(); ++i) {
                _region->Add(&_multiPoint->cells[i]);
            }
        } else if (GeoParser::isMultiLine(obj)) {
            _multiLine.reset(new MultiLineWithCRS());
            if (!GeoParser::parseMultiLine(obj, _multiLine.get())) { return false; }
            _region.reset(new S2RegionUnion());
            for (size_t i = 0; i < _multiLine->lines.vector().size(); ++i) {
                _region->Add(_multiLine->lines.vector()[i]);
            }
        } else if (GeoParser::isMultiPolygon(obj)) {
            _multiPolygon.reset(new MultiPolygonWithCRS());
            if (!GeoParser::parseMultiPolygon(obj, _multiPolygon.get())) { return false; }
            _region.reset(new S2RegionUnion());
            for (size_t i = 0; i < _multiPolygon->polygons.vector().size(); ++i) {
                _region->Add(_multiPolygon->polygons.vector()[i]);
            }
        } else if (GeoParser::isGeometryCollection(obj)) {
            _geometryCollection.reset(new GeometryCollection());
            if (!GeoParser::parseGeometryCollection(obj, _geometryCollection.get())) {
                return false;
            }
            _region.reset(new S2RegionUnion());
            for (size_t i = 0; i < _geometryCollection->points.size(); ++i) {
                _region->Add(&_geometryCollection->points[i].cell);
            }
            for (size_t i = 0; i < _geometryCollection->lines.vector().size(); ++i) {
                _region->Add(&_geometryCollection->lines.vector()[i]->line);
            }
            for (size_t i = 0; i < _geometryCollection->polygons.vector().size(); ++i) {
                _region->Add(&_geometryCollection->polygons.vector()[i]->polygon);
            }
            for (size_t i = 0; i < _geometryCollection->multiPoints.vector().size(); ++i) {
                MultiPointWithCRS* multiPoint = _geometryCollection->multiPoints.vector()[i];
                for (size_t j = 0; j < multiPoint->cells.size(); ++j) {
                    _region->Add(&multiPoint->cells[j]);
                }
            }
            for (size_t i = 0; i < _geometryCollection->multiLines.vector().size(); ++i) {
                const MultiLineWithCRS* multiLine = _geometryCollection->multiLines.vector()[i];
                for (size_t j = 0; j < multiLine->lines.vector().size(); ++j) {
                    _region->Add(multiLine->lines.vector()[j]);
                }
            }
            for (size_t i = 0; i < _geometryCollection->multiPolygons.vector().size(); ++i) {
                const MultiPolygonWithCRS* multiPolygon =
                    _geometryCollection->multiPolygons.vector()[i];
                for (size_t j = 0; j < multiPolygon->polygons.vector().size(); ++j) {
                    _region->Add(multiPolygon->polygons.vector()[j]);
                }
            }
        } else {
            return false;
        }

        return true;
    }

    const S2Region& GeometryContainer::getRegion() const {
        if (NULL != _point) {
            // _point->crs might be FLAT but we "upgrade" it for free.
            return _point->cell;
        } else if (NULL != _line) {
            return _line->line;
        } else if (NULL != _cap && SPHERE == _cap->crs) {
            return _cap->cap;
        } else if (NULL != _multiPoint) {
            return *_region;
        } else if (NULL != _multiLine) {
            return *_region;
        } else if (NULL != _multiPolygon) {
            return *_region;
        } else if (NULL != _geometryCollection) {
            return *_region;
        } else {
            verify(NULL != _polygon);
            verify(SPHERE == _polygon->crs);
            return _polygon->polygon;
        }
    }
}  // namespace mongo
