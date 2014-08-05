/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/db/geo/geo_query.h"

#include "mongo/db/geo/geoparser.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::equals;

    bool NearQuery::parseLegacyQuery(const BSONObj &obj) {

        bool hasGeometry = false;

        // First, try legacy near, e.g.:
        // t.find({ loc : { $nearSphere: [0,0], $minDistance: 1, $maxDistance: 3 }})
        // t.find({ loc : { $nearSphere: [0,0] }})
        // t.find({ loc : { $near : [0, 0, 1] } });
        // t.find({ loc : { $near: { someGeoJSONPoint}})
        // t.find({ loc : { $geoNear: { someGeoJSONPoint}})
        BSONObjIterator it(obj);
        while (it.more()) {
            BSONElement e = it.next();
            if (equals(e.fieldName(), "$near") || equals(e.fieldName(), "$geoNear")
                                               || equals(e.fieldName(), "$nearSphere")) {
                if (!e.isABSONObj()) { return false; }
                BSONObj embeddedObj = e.embeddedObject();

                if ((GeoParser::isPoint(embeddedObj) && GeoParser::parsePoint(embeddedObj, &centroid))
                    || GeoParser::parsePointWithMaxDistance(embeddedObj, &centroid, &maxDistance)) {
                    uassert(18522, "max distance must be non-negative", maxDistance >= 0.0);
                    hasGeometry = true;
                    isNearSphere = equals(e.fieldName(), "$nearSphere");
                }
            } else if (equals(e.fieldName(), "$minDistance")) {
                uassert(16893, "$minDistance must be a number", e.isNumber());
                minDistance = e.Number();
                uassert(16894, "$minDistance must be non-negative", minDistance >= 0.0);
            } else if (equals(e.fieldName(), "$maxDistance")) {
                uassert(16895, "$maxDistance must be a number", e.isNumber());
                maxDistance = e.Number();
                uassert(16896, "$maxDistance must be non-negative", maxDistance >= 0.0);
            } else if (equals(e.fieldName(), "$uniqueDocs")) {
                warning() << "ignoring deprecated option $uniqueDocs";
            }
        }

        return hasGeometry;
    }

    Status NearQuery::parseNewQuery(const BSONObj &obj) {
        bool hasGeometry = false;

        BSONObjIterator objIt(obj);
        if (!objIt.more()) {
            return Status(ErrorCodes::BadValue, "empty geo near query object");
        }
        BSONElement e = objIt.next();
        // Just one arg. to $geoNear.
        if (objIt.more()) {
            return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                          "geo near accepts just one argument when querying for a GeoJSON " <<
                          "point. Extra field found: " << objIt.next());
        }

        // Parse "new" near:
        // t.find({"geo" : {"$near" : {"$geometry": pointA, $minDistance: 1, $maxDistance: 3}}})
        // t.find({"geo" : {"$geoNear" : {"$geometry": pointA, $minDistance: 1, $maxDistance: 3}}})
        if (!e.isABSONObj()) {
            return Status(ErrorCodes::BadValue, "geo near query argument is not an object");
        }
        BSONObj::MatchType matchType = static_cast<BSONObj::MatchType>(e.getGtLtOp());
        if (BSONObj::opNEAR != matchType) {
            return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                          "invalid geo near query operator: " << e.fieldName());
        }

        // Iterate over the argument.
        BSONObjIterator it(e.embeddedObject());
        while (it.more()) {
            BSONElement e = it.next();
            if (equals(e.fieldName(), "$geometry")) {
                if (e.isABSONObj()) {
                    BSONObj embeddedObj = e.embeddedObject();
                    uassert(16885, "$near requires a point, given " + embeddedObj.toString(),
                            GeoParser::isPoint(embeddedObj));
                    if (!GeoParser::parsePoint(embeddedObj, &centroid)) {
                        return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                                      "invalid point in geo near query $geometry argument: " <<
                                      embeddedObj);
                    }
                    uassert(16681, "$near requires geojson point, given " + embeddedObj.toString(),
                            (SPHERE == centroid.crs));
                    hasGeometry = true;
                }
            } else if (equals(e.fieldName(), "$minDistance")) {
                uassert(16897, "$minDistance must be a number", e.isNumber());
                minDistance = e.Number();
                uassert(16898, "$minDistance must be non-negative", minDistance >= 0.0);
            } else if (equals(e.fieldName(), "$maxDistance")) {
                uassert(16899, "$maxDistance must be a number", e.isNumber());
                maxDistance = e.Number();
                uassert(16900, "$maxDistance must be non-negative", maxDistance >= 0.0);
            }
        }

        if (!hasGeometry) {
            return Status(ErrorCodes::BadValue, "$geometry is required for geo near query");
        }

        return Status::OK();
    }


    Status NearQuery::parseFrom(const BSONObj &obj) {

        Status status = Status::OK();

        if (!parseLegacyQuery(obj)) {
            // Clear out any half-baked data.
            minDistance = 0;
            isNearSphere = false;
            maxDistance = std::numeric_limits<double>::max();
            centroid = PointWithCRS();
            // ...and try parsing new format.
            status = parseNewQuery(obj);
        }

        if (!status.isOK())
            return status;

        // Fixup the near query for anonoyances caused by $nearSphere
        if (isNearSphere) {

            // The user-provided point can be flat for a spherical query - needs to be projectable
            uassert(17444,
                    "Legacy point is out of bounds for spherical query",
                    ShapeProjection::supportsProject(centroid, SPHERE));

            unitsAreRadians = SPHERE != centroid.crs;
            // GeoJSON points imply wrapping queries
            isWrappingQuery = SPHERE == centroid.crs;

            // Project the point to a spherical CRS now that we've got the settings we need
            // We need to manually project here since we aren't using GeometryContainer
            ShapeProjection::projectInto(&centroid, SPHERE);
        }
        else {
            unitsAreRadians = false;
            isWrappingQuery = SPHERE == centroid.crs;
        }

        return status;
    }

    bool GeoQuery::parseLegacyQuery(const BSONObj &obj) {
        // The only legacy syntax is {$within: {.....}}
        BSONObjIterator outerIt(obj);
        if (!outerIt.more()) { return false; }
        BSONElement withinElt = outerIt.next();
        if (outerIt.more()) { return false; }
        if (!withinElt.isABSONObj()) { return false; }
        if (!equals(withinElt.fieldName(), "$within") && !equals(withinElt.fieldName(), "$geoWithin")) {
            return false;
        }
        BSONObj withinObj = withinElt.embeddedObject();

        bool hasGeometry = false;

        BSONObjIterator withinIt(withinObj);
        while (withinIt.more()) {
            BSONElement elt = withinIt.next();
            if (equals(elt.fieldName(), "$uniqueDocs")) {
                warning() << "deprecated $uniqueDocs option: " << obj.toString() << endl;
                // return false;
            }
            else if (elt.isABSONObj()) {
                hasGeometry = geoContainer.parseFrom(elt.wrap());
            }
            else {
                warning() << "bad geo query: " << obj.toString() << endl;
                return false;
            }
        }

        predicate = GeoQuery::WITHIN;

        return hasGeometry;
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
        if (!(parseLegacyQuery(obj) || parseNewQuery(obj)))
            return false;

        // Big polygon with strict winding order is represented as an S2Loop in SPHERE CRS.
        // So converting the query to SPHERE CRS makes things easier than projecting all the data
        // into STRICT_SPHERE CRS.
        if (STRICT_SPHERE == geoContainer.getNativeCRS()) {
            if (!geoContainer.supportsProject(SPHERE))
                return false;
            geoContainer.projectInto(SPHERE);
        }

        // $geoIntersect queries are hardcoded to *always* be in SPHERE CRS
        // TODO: This is probably bad semantics, should not do this
        if (GeoQuery::INTERSECT == predicate) {
            if (!geoContainer.supportsProject(SPHERE))
                return false;
            geoContainer.projectInto(SPHERE);
        }

        return true;
    }

}  // namespace mongo
