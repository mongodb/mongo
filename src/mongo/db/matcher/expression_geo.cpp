/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/db/matcher/expression_geo.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/basic.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

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
        // $geoWithin doesn't accept multiple shapes.
        if (geoContainer && queryElt.fieldNameStringData() == "$geoWithin"_sd) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "$geoWithin doesn't accept multiple shapes "
                                        << queryElt.toString());
        }
        if (elt.fieldNameStringData() == "$uniqueDocs") {
            // Deprecated "$uniqueDocs" field
            LOGV2_WARNING(23847, "Deprecated $uniqueDocs option", "query"_attr = redact(obj));
        } else {
            // The element must be a geo specifier. "$box", "$center", "$geometry", etc.
            geoContainer.reset(new GeometryContainer());
            Status status = geoContainer->parseFromQuery(elt);
            if (!status.isOK())
                return status;
        }
    }

    if (geoContainer == nullptr) {
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
                          str::stream()
                              << "$geoIntersect not supported with provided geometry: " << obj);
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
        StringData fieldName = e.fieldNameStringData();
        if ((fieldName == "$near") || (fieldName == "$geoNear") || (fieldName == "$nearSphere")) {
            if (!e.isABSONObj()) {
                return false;
            }
            BSONObj embeddedObj = e.embeddedObject();

            if (GeoParser::parseQueryPoint(e, centroid.get()).isOK() ||
                GeoParser::parsePointWithMaxDistance(embeddedObj, centroid.get(), &maxDistance)) {
                uassert(18522, "max distance must be non-negative", maxDistance >= 0.0);
                hasGeometry = true;
                isNearSphere = (e.fieldNameStringData() == "$nearSphere");
            }
        } else if (fieldName == "$minDistance") {
            uassert(16893, "$minDistance must be a number", e.isNumber());
            minDistance = e.Number();
            uassert(16894, "$minDistance must be non-negative", minDistance >= 0.0);
        } else if (fieldName == "$maxDistance") {
            uassert(16895, "$maxDistance must be a number", e.isNumber());
            maxDistance = e.Number();
            uassert(16896, "$maxDistance must be non-negative", maxDistance >= 0.0);
        } else if (fieldName == "$uniqueDocs") {
            LOGV2_WARNING(23848, "Ignoring deprecated option $uniqueDocs");
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
                      str::stream()
                          << "geo near accepts just one argument when querying for a GeoJSON "
                          << "point. Extra field found: " << objIt.next());
    }

    // Parse "new" near:
    // t.find({"geo" : {"$near" : {"$geometry": pointA, $minDistance: 1, $maxDistance: 3}}})
    // t.find({"geo" : {"$geoNear" : {"$geometry": pointA, $minDistance: 1, $maxDistance: 3}}})
    if (!e.isABSONObj()) {
        return Status(ErrorCodes::BadValue, "geo near query argument is not an object");
    }

    if (PathAcceptingKeyword::GEO_NEAR != MatchExpressionParser::parsePathAcceptingKeyword(e)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "invalid geo near query operator: " << e.fieldName());
    }

    // Returns true if 'x' is a valid numeric value, that is, a non-negative finite number.
    auto isValidNumericValue = [](double x) -> bool { return x >= 0.0 && std::isfinite(x); };

    // Iterate over the argument.
    BSONObjIterator it(e.embeddedObject());
    while (it.more()) {
        BSONElement e = it.next();
        StringData fieldName = e.fieldNameStringData();
        if (fieldName == "$geometry") {
            if (e.isABSONObj()) {
                BSONObj embeddedObj = e.embeddedObject();
                Status status = GeoParser::parseQueryPoint(e, centroid.get());
                if (!status.isOK()) {
                    return Status(ErrorCodes::BadValue,
                                  str::stream()
                                      << "invalid point in geo near query $geometry argument: "
                                      << embeddedObj << "  " << status.reason());
                }
                if (SPHERE != centroid->crs) {
                    return Status(ErrorCodes::BadValue,
                                  str::stream() << "$near requires geojson point, given "
                                                << embeddedObj.toString());
                }
                hasGeometry = true;
            }
        } else if (fieldName == "$minDistance") {
            if (!e.isNumber()) {
                return Status(ErrorCodes::BadValue, "$minDistance must be a number");
            }
            minDistance = e.Number();
            if (!isValidNumericValue(minDistance)) {
                return Status(ErrorCodes::BadValue, "$minDistance must be non-negative");
            }
        } else if (fieldName == "$maxDistance") {
            if (!e.isNumber()) {
                return Status(ErrorCodes::BadValue, "$maxDistance must be a number");
            }
            maxDistance = e.Number();
            if (!isValidNumericValue(maxDistance)) {
                return Status(ErrorCodes::BadValue, "$maxDistance must be non-negative");
            }
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
        if (!ShapeProjection::supportsProject(*centroid, SPHERE)) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Legacy point is out of bounds for spherical query");
        }

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

/**
 * Takes ownership of the passed-in GeoExpression.
 */
GeoMatchExpression::GeoMatchExpression(StringData path,
                                       const GeoExpression* query,
                                       const BSONObj& rawObj,
                                       clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(GEO, path, std::move(annotation)),
      _rawObj(rawObj),
      _query(query),
      _canSkipValidation(false) {}

/**
 * Takes shared ownership of the passed-in GeoExpression.
 */
GeoMatchExpression::GeoMatchExpression(StringData path,
                                       std::shared_ptr<const GeoExpression> query,
                                       const BSONObj& rawObj,
                                       clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(GEO, path, std::move(annotation)),
      _rawObj(rawObj),
      _query(query),
      _canSkipValidation(false) {}

bool GeoMatchExpression::matchesSingleElement(const BSONElement& e, MatchDetails* details) const {
    return contains(_query->getGeometry(), _query->getPred(), _canSkipValidation, e, details);
}

bool GeoMatchExpression::contains(const GeometryContainer& queryGeom,
                                  const GeoExpression::Predicate& queryPredicate,
                                  bool skipValidation,
                                  const BSONElement& e,
                                  MatchDetails*) {
    if (!e.isABSONObj())
        return false;

    GeometryContainer geometry;
    if (!geometry.parseFromStorage(e, skipValidation).isOK())
        return false;

    // Never match big polygon
    if (geometry.getNativeCRS() == STRICT_SPHERE)
        return false;

    // Project this geometry into the CRS of the larger geometry.

    // In the case of index validation, we are projecting the geometry of the query
    // into the CRS of the index to confirm that the index region convers/includes
    // the region described by the predicate.

    if (!geometry.supportsProject(queryGeom.getNativeCRS()))
        return false;

    return contains(queryGeom, queryPredicate, &geometry);
}

bool GeoMatchExpression::contains(const GeometryContainer& queryGeom,
                                  const GeoExpression::Predicate& queryPredicate,
                                  GeometryContainer* geometry) {
    geometry->projectInto(queryGeom.getNativeCRS());
    if (GeoExpression::WITHIN == queryPredicate) {
        return queryGeom.contains(*geometry);
    } else {
        verify(GeoExpression::INTERSECT == queryPredicate);
        return queryGeom.intersects(*geometry);
    }
}

bool GeoMatchExpression::matchesGeoContainer(const GeometryContainer& input) const {
    // Never match big polygon
    if (input.getNativeCRS() == STRICT_SPHERE)
        return false;

    // Project this geometry into the CRS of the larger geometry.

    // In the case of index validation, we are projecting the geometry of the query
    // into the CRS of the index to confirm that the index region convers/includes
    // the region described by the predicate.

    if (!input.supportsProject(_query->getGeometry().getNativeCRS()))
        return false;

    GeometryContainer geometry{input};
    return contains(_query->getGeometry(), _query->getPred(), &geometry);
}

void GeoMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);

    BSONObjBuilder builder;
    serialize(&builder, true);
    debug << "GEO raw = " << builder.obj().toString();

    MatchExpression::TagData* td = getTag();
    if (nullptr != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
}

BSONObj GeoMatchExpression::getSerializedRightHandSide() const {
    BSONObjBuilder subobj;
    subobj.appendElements(_rawObj);
    return subobj.obj();
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
    std::unique_ptr<GeoMatchExpression> next =
        std::make_unique<GeoMatchExpression>(path(), _query, _rawObj, _errorAnnotation);
    next->_canSkipValidation = _canSkipValidation;
    if (getTag()) {
        next->setTag(getTag()->clone());
    }
    return next;
}

//
// Parse-only geo expressions: geoNear (formerly known as near).
//

GeoNearMatchExpression::GeoNearMatchExpression(StringData path,
                                               const GeoNearExpression* query,
                                               const BSONObj& rawObj)
    : LeafMatchExpression(GEO_NEAR, path), _rawObj(rawObj), _query(query) {}

GeoNearMatchExpression::GeoNearMatchExpression(StringData path,
                                               std::shared_ptr<const GeoNearExpression> query,
                                               const BSONObj& rawObj)
    : LeafMatchExpression(GEO_NEAR, path), _rawObj(rawObj), _query(query) {}

bool GeoNearMatchExpression::matchesSingleElement(const BSONElement& e,
                                                  MatchDetails* details) const {
    return true;
}

void GeoNearMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << "GEONEAR " << _query->toString();
    MatchExpression::TagData* td = getTag();
    if (nullptr != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
}

BSONObj GeoNearMatchExpression::getSerializedRightHandSide() const {
    BSONObjBuilder objBuilder;
    objBuilder.appendElements(_rawObj);
    return objBuilder.obj();
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
    std::unique_ptr<GeoNearMatchExpression> next =
        std::make_unique<GeoNearMatchExpression>(path(), _query, _rawObj);
    if (getTag()) {
        next->setTag(getTag()->clone());
    }
    return next;
}
}  // namespace mongo
