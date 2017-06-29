// expression_geo.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/db/matcher/expression_geo.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/platform/basic.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {


using mongoutils::str::equals;

//
// GeoExpression
//

// Put simple constructors here for unique_ptr.
GeoExpression::GeoExpression() : field(""), predicate(INVALID) {}
GeoExpression::GeoExpression(const std::string& f) : field(f), predicate(INVALID) {}

Status GeoExpression::parseQuery(const BSONObj& obj) {
    BSONObjIterator outerIt(obj);
    // "within" / "geoWithin" / "geoIntersects"
    BSONElement queryElt = outerIt.next();
    if (outerIt.more()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "can't parse extra field: " << outerIt.next());
    }

    auto keyword = MatchExpressionParser::parsePathAcceptingKeyword(queryElt);
    if (PathAcceptingKeyword::GEO_INTERSECTS == keyword) {
        predicate = GeoExpression::INTERSECT;
    } else if (PathAcceptingKeyword::WITHIN == keyword) {
        predicate = GeoExpression::WITHIN;
    } else {
        // eoo() or unknown query predicate.
        return Status(ErrorCodes::BadValue,
                      str::stream() << "invalid geo query predicate: " << obj);
    }

    // Parse geometry after predicates.
    if (Object != queryElt.type())
        return Status(ErrorCodes::BadValue, "geometry must be an object");
    BSONObj geoObj = queryElt.Obj();

    BSONObjIterator geoIt(geoObj);

    while (geoIt.more()) {
        BSONElement elt = geoIt.next();
        if (str::equals(elt.fieldName(), "$uniqueDocs")) {
            // Deprecated "$uniqueDocs" field
            warning() << "deprecated $uniqueDocs option: " << redact(obj);
        } else {
            // The element must be a geo specifier. "$box", "$center", "$geometry", etc.
            geoContainer.reset(new GeometryContainer());
            Status status = geoContainer->parseFromQuery(elt);
            if (!status.isOK())
                return status;
        }
    }

    if (geoContainer == NULL) {
        return Status(ErrorCodes::BadValue, "geo query doesn't have any geometry");
    }

    return Status::OK();
}

Status GeoExpression::parseFrom(const BSONObj& obj) {
    // Initialize geoContainer and parse BSON object
    Status status = parseQuery(obj);
    if (!status.isOK())
        return status;

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
    if (GeoExpression::WITHIN == predicate && !geoContainer->supportsContains()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "$within not supported with provided geometry: " << obj);
    }

    // Big polygon with strict winding order is represented as an S2Loop in SPHERE CRS.
    // So converting the query to SPHERE CRS makes things easier than projecting all the data
    // into STRICT_SPHERE CRS.
    if (STRICT_SPHERE == geoContainer->getNativeCRS()) {
        if (!geoContainer->supportsProject(SPHERE)) {
            return Status(ErrorCodes::BadValue, "only polygon supported with strict winding order");
        }
        geoContainer->projectInto(SPHERE);
    }

    // $geoIntersect queries are hardcoded to *always* be in SPHERE CRS
    // TODO: This is probably bad semantics, should not do this
    if (GeoExpression::INTERSECT == predicate) {
        if (!geoContainer->supportsProject(SPHERE)) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "$geoIntersect not supported with provided geometry: "
                                        << obj);
        }
        geoContainer->projectInto(SPHERE);
    }

    return Status::OK();
}

//
// GeoNearExpression
//

GeoNearExpression::GeoNearExpression()
    : minDistance(0),
      maxDistance(std::numeric_limits<double>::max()),
      isNearSphere(false),
      unitsAreRadians(false),
      isWrappingQuery(false) {}

GeoNearExpression::GeoNearExpression(const std::string& f)
    : field(f),
      minDistance(0),
      maxDistance(std::numeric_limits<double>::max()),
      isNearSphere(false),
      unitsAreRadians(false),
      isWrappingQuery(false) {}

bool GeoNearExpression::parseLegacyQuery(const BSONObj& obj) {
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
        if (equals(e.fieldName(), "$near") || equals(e.fieldName(), "$geoNear") ||
            equals(e.fieldName(), "$nearSphere")) {
            if (!e.isABSONObj()) {
                return false;
            }
            BSONObj embeddedObj = e.embeddedObject();

            if (GeoParser::parseQueryPoint(e, centroid.get()).isOK() ||
                GeoParser::parsePointWithMaxDistance(embeddedObj, centroid.get(), &maxDistance)) {
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
        } else {
            // In a query document, $near queries can have no non-geo sibling parameters.
            uasserted(34413,
                      str::stream() << "invalid argument in geo near query: " << e.fieldName());
        }
    }

    return hasGeometry;
}

Status GeoNearExpression::parseNewQuery(const BSONObj& obj) {
    bool hasGeometry = false;

    BSONObjIterator objIt(obj);
    if (!objIt.more()) {
        return Status(ErrorCodes::BadValue, "empty geo near query object");
    }
    BSONElement e = objIt.next();
    // Just one arg. to $geoNear.
    if (objIt.more()) {
        return Status(ErrorCodes::BadValue,
                      mongoutils::str::stream()
                          << "geo near accepts just one argument when querying for a GeoJSON "
                          << "point. Extra field found: "
                          << objIt.next());
    }

    // Parse "new" near:
    // t.find({"geo" : {"$near" : {"$geometry": pointA, $minDistance: 1, $maxDistance: 3}}})
    // t.find({"geo" : {"$geoNear" : {"$geometry": pointA, $minDistance: 1, $maxDistance: 3}}})
    if (!e.isABSONObj()) {
        return Status(ErrorCodes::BadValue, "geo near query argument is not an object");
    }

    if (PathAcceptingKeyword::GEO_NEAR != MatchExpressionParser::parsePathAcceptingKeyword(e)) {
        return Status(ErrorCodes::BadValue,
                      mongoutils::str::stream() << "invalid geo near query operator: "
                                                << e.fieldName());
    }

    // Iterate over the argument.
    BSONObjIterator it(e.embeddedObject());
    while (it.more()) {
        BSONElement e = it.next();
        if (equals(e.fieldName(), "$geometry")) {
            if (e.isABSONObj()) {
                BSONObj embeddedObj = e.embeddedObject();
                Status status = GeoParser::parseQueryPoint(e, centroid.get());
                if (!status.isOK()) {
                    return Status(ErrorCodes::BadValue,
                                  str::stream()
                                      << "invalid point in geo near query $geometry argument: "
                                      << embeddedObj
                                      << "  "
                                      << status.reason());
                }
                uassert(16681,
                        "$near requires geojson point, given " + embeddedObj.toString(),
                        (SPHERE == centroid->crs));
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
        } else {
            // Return an error if a bad argument was passed inside the query document.
            return Status(ErrorCodes::BadValue,
                          str::stream() << "invalid argument in geo near query: " << e.fieldName());
        }
    }

    if (!hasGeometry) {
        return Status(ErrorCodes::BadValue, "$geometry is required for geo near query");
    }

    return Status::OK();
}


Status GeoNearExpression::parseFrom(const BSONObj& obj) {
    Status status = Status::OK();
    centroid.reset(new PointWithCRS());

    if (!parseLegacyQuery(obj)) {
        // Clear out any half-baked data.
        minDistance = 0;
        isNearSphere = false;
        maxDistance = std::numeric_limits<double>::max();
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
                ShapeProjection::supportsProject(*centroid, SPHERE));

        unitsAreRadians = SPHERE != centroid->crs;
        // GeoJSON points imply wrapping queries
        isWrappingQuery = SPHERE == centroid->crs;

        // Project the point to a spherical CRS now that we've got the settings we need
        // We need to manually project here since we aren't using GeometryContainer
        ShapeProjection::projectInto(centroid.get(), SPHERE);
    } else {
        unitsAreRadians = false;
        isWrappingQuery = SPHERE == centroid->crs;
    }

    return status;
}

//
// GeoMatchExpression and GeoNearMatchExpression
//

//
// Geo queries we don't need an index to answer: geoWithin and geoIntersects
//

Status GeoMatchExpression::init(StringData path,
                                const GeoExpression* query,
                                const BSONObj& rawObj) {
    _query.reset(query);
    _rawObj = rawObj;
    return setPath(path);
}

bool GeoMatchExpression::matchesSingleElement(const BSONElement& e) const {
    if (!e.isABSONObj())
        return false;

    GeometryContainer geometry;

    if (!geometry.parseFromStorage(e, _canSkipValidation).isOK())
        return false;

    // Never match big polygon
    if (geometry.getNativeCRS() == STRICT_SPHERE)
        return false;

    // Project this geometry into the CRS of the query
    if (!geometry.supportsProject(_query->getGeometry().getNativeCRS()))
        return false;

    geometry.projectInto(_query->getGeometry().getNativeCRS());

    if (GeoExpression::WITHIN == _query->getPred()) {
        return _query->getGeometry().contains(geometry);
    } else {
        verify(GeoExpression::INTERSECT == _query->getPred());
        return _query->getGeometry().intersects(geometry);
    }
}

void GeoMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);

    BSONObjBuilder builder;
    serialize(&builder);
    debug << "GEO raw = " << builder.obj().toString();

    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
}

void GeoMatchExpression::serialize(BSONObjBuilder* out) const {
    BSONObjBuilder subobj(out->subobjStart(path()));
    subobj.appendElements(_rawObj);
    subobj.doneFast();
}

bool GeoMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const GeoMatchExpression* realOther = static_cast<const GeoMatchExpression*>(other);

    if (path() != realOther->path())
        return false;

    return SimpleBSONObjComparator::kInstance.evaluate(_rawObj == realOther->_rawObj);
}

std::unique_ptr<MatchExpression> GeoMatchExpression::shallowClone() const {
    std::unique_ptr<GeoMatchExpression> next = stdx::make_unique<GeoMatchExpression>();
    next->init(path(), NULL, _rawObj).transitional_ignore();
    next->_query = _query;
    next->_canSkipValidation = _canSkipValidation;
    if (getTag()) {
        next->setTag(getTag()->clone());
    }
    return std::move(next);
}

//
// Parse-only geo expressions: geoNear (formerly known as near).
//

Status GeoNearMatchExpression::init(StringData path,
                                    const GeoNearExpression* query,
                                    const BSONObj& rawObj) {
    _query.reset(query);
    _rawObj = rawObj;
    return setPath(path);
}

bool GeoNearMatchExpression::matchesSingleElement(const BSONElement& e) const {
    // See ops/update.cpp.
    // This node is removed by the query planner.  It's only ever called if we're getting an
    // elemMatchKey.
    return true;
}

void GeoNearMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << "GEONEAR " << _query->toString();
    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
}

void GeoNearMatchExpression::serialize(BSONObjBuilder* out) const {
    BSONObjBuilder subobj(out->subobjStart(path()));
    subobj.appendElements(_rawObj);
    subobj.doneFast();
}

bool GeoNearMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const GeoNearMatchExpression* realOther = static_cast<const GeoNearMatchExpression*>(other);

    if (path() != realOther->path())
        return false;

    return SimpleBSONObjComparator::kInstance.evaluate(_rawObj == realOther->_rawObj);
}

std::unique_ptr<MatchExpression> GeoNearMatchExpression::shallowClone() const {
    std::unique_ptr<GeoNearMatchExpression> next = stdx::make_unique<GeoNearMatchExpression>();
    next->init(path(), NULL, _rawObj).transitional_ignore();
    next->_query = _query;
    if (getTag()) {
        next->setTag(getTag()->clone());
    }
    return std::move(next);
}
}
