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

#include "mongo/db/geo/geoquery.h"

namespace mongo {

    bool NearQuery::parseFromGeoNear(const BSONObj &obj, double radius) {
        if (obj["near"].eoo()) { return false; }
        BSONObj nearObj = obj["near"].embeddedObject();
        // The CRS for the legacy points dictates that distances are in radians.
        fromRadians = GeoParser::isLegacyPoint(nearObj);
        if (!GeoParser::parsePoint(nearObj, &centroid)) { return false; }

        if (!obj["maxDistance"].eoo()) {
            if (obj["maxDistance"].isNumber()) {
                double distArg = obj["maxDistance"].number();
                if (fromRadians) {
                    maxDistance = distArg * radius;
                } else {
                    maxDistance = distArg;
                }
            } else {
                return false;
            }
        }
        return true;
    }

    bool NearQuery::parseFrom(const BSONObj &obj, double radius) {
        bool hasGeometry = false;

        // First, try legacy near.
        // Legacy near parsing: t.find({ loc : { $nearSphere: [0,0], $maxDistance: 3 }})
        // Legacy near parsing: t.find({ loc : { $nearSphere: [0,0] }})
        // Legacy near parsing: t.find({ loc : { $near: { someGeoJSONPoint}})
        BSONObjIterator it(obj);
        while (it.more()) {
            BSONElement e = it.next();
            bool isNearSphere = mongoutils::str::equals(e.fieldName(), "$nearSphere");
            bool isMaxDistance = mongoutils::str::equals(e.fieldName(), "$maxDistance");
            bool isNear = mongoutils::str::equals(e.fieldName(), "$near");
            if (isNearSphere || isNear) {
                if (!e.isABSONObj()) { return false; }
                BSONObj embeddedObj = e.embeddedObject();
                if (isNearSphere && GeoParser::isPoint(embeddedObj)) {
                    fromRadians = GeoParser::isLegacyPoint(embeddedObj);
                    GeoParser::parsePoint(embeddedObj, &centroid);
                    hasGeometry = true;
                } else if (isNear && GeoParser::isGeoJSONPoint(embeddedObj)) {
                    GeoParser::parseGeoJSONPoint(embeddedObj, &centroid);
                    hasGeometry = true;
                }
            } else if (isMaxDistance) {
                maxDistance = e.Number();
            }
        }

        if (fromRadians) {
            maxDistance *= radius;
        }

        if (hasGeometry) { return true; }

        // Next, try "new" near
        // New near: t.find({ "geo" : { "$near" : { "$geometry" : pointA, $maxDistance : 20 }}})
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
                    uassert(16681, "$near requires geojson point, given " + embeddedObj.toString(),
                            GeoParser::isGeoJSONPoint(embeddedObj));
                    GeoParser::parseGeoJSONPoint(embeddedObj, &centroid);
                    hasGeometry = true;
                }
            } else if (mongoutils::str::equals(e.fieldName(), "$maxDistance")) {
                if (e.isNumber()) {
                    maxDistance = e.Number();
                }
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
        bool contains = mongoutils::str::equals(e.fieldName(), "$within");
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

    bool GeoQuery::satisfiesPredicate(const GeometryContainer &otherContainer) const {
        verify(predicate == WITHIN || predicate == INTERSECT);

        if (WITHIN == predicate) {
            return geoContainer.contains(otherContainer);
        } else {
            return geoContainer.intersects(otherContainer);
        }
    }

    bool GeometryContainer::contains(const GeometryContainer& otherContainer) const {
        if (NULL != _oldPolygon) {
            if (NULL == otherContainer._oldPoint) { return false; }
            return _oldPolygon->contains(*otherContainer._oldPoint);
        }
        if (NULL != _oldBox) {
            if (NULL == otherContainer._oldPoint) { return false; }
            return _oldBox->inside(*otherContainer._oldPoint);
        }
        if (NULL != _cap) {
            if (NULL == otherContainer._cell) { return false; }
            return _cap->MayIntersect(*otherContainer._cell);
        }
        if (NULL != _oldCircle) {
            if (NULL == otherContainer._oldPoint) { return false; }
            // Let's be as consistent epsilon-wise as we can with the '2d' indextype.
            return distanceWithin(_oldCircle->center, *otherContainer._oldPoint,
                                  _oldCircle->radius);
        }
        if (NULL != _polygon) {
            if (NULL != otherContainer._cell) {
                // Intersecting a point is containing a point.  Hooray!
                return _polygon->MayIntersect(*otherContainer._cell);
            } else if (NULL != otherContainer._line) {
                // Kind of a mess.  We get a function for clipping the line to the
                // polygon.  We do this and make sure the line is the same as the
                // line we're clipping against.
                vector<S2Polyline*> clipped;
                _polygon->IntersectWithPolyline(otherContainer._line.get(), &clipped);
                if (1 != clipped.size()) { return false; }
                // If the line is entirely contained within the polygon, we should be
                // getting it back verbatim, so really there should be no error.
                bool ret = clipped[0]->NearlyCoversPolyline(*otherContainer._line,
                    S1Angle::Degrees(1e-10));
                for (size_t i = 0; i < clipped.size(); ++i) delete clipped[i];
                return ret;
            } else if (NULL != otherContainer._polygon) {
                return _polygon->Contains(otherContainer._polygon.get());
            } else { return false; }
        }
        // Containment only works for polygons/boxes/circles.
        return false;
    }

    bool GeometryContainer::intersects(const GeometryContainer& otherContainer) const {
        if (NULL != otherContainer._cell) {
            return intersects(*otherContainer._cell);
        } else if (NULL != otherContainer._line) {
            return intersects(*otherContainer._line);
        } else if (NULL != otherContainer._polygon) {
            return intersects(*otherContainer._polygon);
        } else {
            return false;
        }
    }

    // Does this (GeometryContainer) intersect the provided data?
    bool GeometryContainer::intersects(const S2Cell &otherPoint) const {
        if (NULL != _cell) {
            return _cell->MayIntersect(otherPoint);
        } else if (NULL != _line) {
            return _line->MayIntersect(otherPoint);
        } else {
            verify(NULL != _polygon);
            return _polygon->MayIntersect(otherPoint);
        }
    }

    bool GeometryContainer::intersects(const S2Polyline& otherLine) const {
        if (NULL != _cell) {
            return otherLine.MayIntersect(*_cell);
        } else if (NULL != _line) {
            return otherLine.Intersects(_line.get());
        } else {
            verify(NULL != _polygon);
            // TODO(hk): modify s2 library to just let us know if it intersected
            // rather than returning all this.
            vector<S2Polyline*> clipped;
            _polygon->IntersectWithPolyline(&otherLine, &clipped);
            bool ret = clipped.size() > 0;
            for (size_t i = 0; i < clipped.size(); ++i) delete clipped[i];
            return ret;
        }
    }

    bool GeometryContainer::intersects(const S2Polygon& otherPolygon) const {
        if (NULL != _cell) {
            return otherPolygon.MayIntersect(*_cell);
        } else if (NULL != _line) {
            // TODO(hk): modify s2 library to just let us know if it intersected
            // rather than returning all this.
            vector<S2Polyline*> clipped;
            otherPolygon.IntersectWithPolyline(_line.get(), &clipped);
            bool ret = clipped.size() > 0;
            for (size_t i = 0; i < clipped.size(); ++i) delete clipped[i];
            return ret;
        } else {
            verify(NULL != _polygon);
            return otherPolygon.Intersects(_polygon.get());
        }
    }

    bool GeometryContainer::parseFrom(const BSONObj& obj) {
        // Free up any pointers we might have left over from previous parses.
        *this = GeometryContainer();
        if (GeoParser::isGeoJSONPolygon(obj)) {
            // We can't really pass these things around willy-nilly except by ptr.
            _polygon.reset(new S2Polygon());
            GeoParser::parseGeoJSONPolygon(obj, _polygon.get());
        } else if (GeoParser::isPoint(obj)) {
            _cell.reset(new S2Cell());
            GeoParser::parsePoint(obj, _cell.get());
            _oldPoint.reset(new Point());
            GeoParser::parsePoint(obj, _oldPoint.get());
        } else if (GeoParser::isLineString(obj)) {
            _line.reset(new S2Polyline());
            GeoParser::parseLineString(obj, _line.get());
        } else if (GeoParser::isLegacyBox(obj)) {
            _oldBox.reset(new Box());
            GeoParser::parseLegacyBox(obj, _oldBox.get());
        } else if (GeoParser::isLegacyPolygon(obj)) {
            _oldPolygon.reset(new Polygon());
            GeoParser::parseLegacyPolygon(obj, _oldPolygon.get());
        } else if (GeoParser::isLegacyCenter(obj)) {
            _oldCircle.reset(new Circle());
            GeoParser::parseLegacyCenter(obj, _oldCircle.get());
        } else if (GeoParser::isLegacyCenterSphere(obj)) {
            _cap.reset(new S2Cap());
            GeoParser::parseLegacyCenterSphere(obj, _cap.get());
        } else {
            return false;
        }
        return true;
    }

    bool GeoQuery::hasS2Region() const {
        return geoContainer.hasS2Region();
    }

    const S2Region& GeometryContainer::getRegion() const {
        if (NULL != _cell) {
            return *_cell;
        } else if (NULL != _line) {
            return *_line;
        } else if (NULL != _cap) {
            return *_cap;
        } else {
            verify(NULL != _polygon);
            return *_polygon;
        }
    }
}  // namespace mongo
