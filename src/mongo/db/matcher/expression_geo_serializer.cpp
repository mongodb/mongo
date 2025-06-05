/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression_geo_serializer.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <vector>

namespace mongo {
namespace {
void appendGeoNearLegacyArray(BSONObjBuilder& bob,
                              const BSONElement& e,
                              const SerializationOptions& opts) {
    if (!opts.isReplacingLiteralsWithRepresentativeValues()) {
        opts.appendLiteral(&bob, e);
    } else {
        // Legacy $geoNear, $nearSphere, and $near require at minimum 2 coordinates to be
        // re-parseable, so the representative value is [1, 1].
        StringData fieldName = e.fieldNameStringData();
        bob.appendArray(fieldName, BSON_ARRAY(1 << 1));
    }
}

void appendShapeOperator(BSONObjBuilder& bob,
                         const BSONElement& e,
                         const SerializationOptions& opts) {
    if (!opts.isReplacingLiteralsWithRepresentativeValues()) {
        opts.appendLiteral(&bob, e);
        return;
    }

    StringData fieldName = e.fieldNameStringData();
    if (fieldName == kCenterField || fieldName == kCenterSphereField) {
        // $center and $centerSphere requires a pair of coordinates and a radius to be
        // re-parseable, so the representative value is [[1, 1],1].
        bob.appendArray(fieldName, BSON_ARRAY(BSON_ARRAY(1 << 1) << 1));
    } else if (fieldName == kBoxField) {
        // $box requires two pairs of coordinates to be re-parseable, so the
        // representative value is [[1, 1],[1,1]].
        bob.appendArray(fieldName, BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(1 << 1)));
    } else if (fieldName == kPolygonField) {
        // $polygon requires three pairs of coordinates to be re-parseable, so the representative
        // value is [[0,0],[0,1],[1,1]].
        bob.appendArray(fieldName,
                        BSON_ARRAY(BSON_ARRAY(0 << 0) << BSON_ARRAY(0 << 1) << BSON_ARRAY(1 << 1)));
    } else {
        opts.appendLiteral(&bob, e);
    }
}

void appendGeoJSONCoordinatesLiteral(BSONObjBuilder& bob,
                                     const BSONElement& coordinatesElem,
                                     const BSONElement& typeElem,
                                     const SerializationOptions& opts) {
    if (!opts.isReplacingLiteralsWithRepresentativeValues()) {
        opts.appendLiteral(&bob, coordinatesElem);
        return;
    }

    StringData fieldName = coordinatesElem.fieldNameStringData();

    // When a $geoNear expression is parsed (see GeoNearExpression::parseNewQuery()), a $geometry
    // object defaults to being parsed as a point, without checking the type of the geometry object.
    // This means we can query for a $geoNear expression that specifies an invalid type, or no type
    // at all. In order to accomodate this case, we default to type: 'Point' to ensure our
    // representative shape is re-parseable.
    auto geoType = GeoParser::geoJSONTypeStringToEnum(typeElem.valueStringData());
    if (geoType == GeoParser::GEOJSON_UNKNOWN) {
        geoType = GeoParser::GEOJSON_POINT;
    }
    switch (geoType) {
        case GeoParser::GEOJSON_POLYGON: {
            // Polygon requires four pairs of coordinates in a closed loop wrapped in an array to be
            // re-parseable, so the representative value is [[[0,0],[0,1],[1,1],[0,0]]].
            bob.appendArray(
                fieldName,
                BSON_ARRAY(BSON_ARRAY(BSON_ARRAY(0 << 0) << BSON_ARRAY(0 << 1) << BSON_ARRAY(1 << 1)
                                                         << BSON_ARRAY(0 << 0))));
            return;
        }
        case GeoParser::GEOJSON_MULTI_POLYGON: {
            // MultiPolygon requires four pairs of coordinates in a closed loop wrapped in 2 arrays
            // to be re-parseable, so the representative value is [[[[0,0],[0,1],[1,1],[0,0]]]].
            bob.appendArray(fieldName,
                            BSON_ARRAY(BSON_ARRAY(BSON_ARRAY(
                                BSON_ARRAY(0 << 0) << BSON_ARRAY(0 << 1) << BSON_ARRAY(1 << 1)
                                                   << BSON_ARRAY(0 << 0)))));
            return;
        }
        case GeoParser::GEOJSON_POINT: {
            // Point requires a pair of coordinates to be re-parseable, so the representative
            // value is [1,1].
            bob.appendArray(fieldName, BSON_ARRAY(1 << 1));
            return;
        }
        case GeoParser::GEOJSON_MULTI_POINT: {
            // MultiPoint requires a pair of coordinates wrapped in an array to be re-parseable, so
            // the representative value is [[1,1]].
            bob.appendArray(fieldName, BSON_ARRAY(BSON_ARRAY(1 << 1)));
            return;
        }
        case GeoParser::GEOJSON_LINESTRING: {
            // LineString requires two pairs of coordinates to be re-parseable, so the
            // representative value is [[0,0],[1,1]].
            bob.appendArray(fieldName, BSON_ARRAY(BSON_ARRAY(0 << 0) << BSON_ARRAY(1 << 1)));
            return;
        }
        case GeoParser::GEOJSON_MULTI_LINESTRING: {
            // MultiLineString requires two LineStrings wrapped in an array to be re-parseable, so
            // the representative value is [[[0,0],[1,1]],[[0,0],[1,1]]].
            bob.appendArray(fieldName,
                            BSON_ARRAY(BSON_ARRAY(BSON_ARRAY(0 << 0) << BSON_ARRAY(1 << 1))
                                       << BSON_ARRAY(BSON_ARRAY(0 << 0) << BSON_ARRAY(1 << 1))));
            return;
        }
        case GeoParser::GEOJSON_GEOMETRY_COLLECTION:
            opts.appendLiteral(&bob, coordinatesElem);
            return;
        case GeoParser::GEOJSON_UNKNOWN:
            break;
    }

    tasserted(8456600,
              str::stream() << "unexpected geo type found in coordinates serialization: "
                            << geoType);
}

void appendCRSObject(BSONObjBuilder& bob,
                     const BSONElement& crsObj,
                     const SerializationOptions& opts) {
    // 'crs' is always an object.
    tassert(7559700, "Expected 'crs' to be an object", crsObj.type() == BSONType::object);
    // 'crs' is required to have a 'type' field with the value 'name'.
    // Additionally, it is required to have an object properties field
    // with a single 'name' field.
    tassert(7559701,
            str::stream() << "Expected 'crs' to contain a string 'type' field, got " << crsObj,
            crsObj[kCrsTypeField] && crsObj[kCrsTypeField].type() == BSONType::string);
    tassert(7559702,
            str::stream() << "Expected 'crs' to contain a 'properties' object, got , " << crsObj,
            crsObj[kCrsPropertiesField] && crsObj[kCrsPropertiesField].type() == BSONType::object);
    tassert(7559703,
            str::stream() << "Expected 'crs.properties' to contain a 'name' "
                             "string field, got "
                          << crsObj[kCrsPropertiesField],
            crsObj[kCrsPropertiesField].Obj()[kCrsNameField] &&
                crsObj[kCrsPropertiesField].Obj()[kCrsNameField].type() == BSONType::string);

    // The CRS "type" and "properties.name" fields must be preserved for
    // kToRepresentativeParseableValue serialization policy so the query
    // shape can be re-parsed (and will be preserved for kUnchanged policy
    // as well).
    BSONObjBuilder crsObjBuilder(bob.subobjStart(kCrsField));
    if (opts.isSerializingLiteralsAsDebugTypes()) {
        opts.appendLiteral(&crsObjBuilder, crsObj[kCrsTypeField]);
    } else {
        crsObjBuilder.append(crsObj[kCrsTypeField]);
    }
    BSONObjBuilder crsPropBuilder(crsObjBuilder.subobjStart(kCrsPropertiesField));
    if (opts.isSerializingLiteralsAsDebugTypes()) {
        opts.appendLiteral(&crsPropBuilder, crsObj[kCrsPropertiesField].Obj()[kCrsNameField]);
    } else {
        crsPropBuilder.append(crsObj[kCrsPropertiesField].Obj()[kCrsNameField]);
    }
    crsPropBuilder.doneFast();
    crsObjBuilder.doneFast();
}

// This function maps closely to GeoParser::parseFromGeoJSON. We serialize a GeoJSON object that
// should have a type, coordinates, etc. This format may have been used as the RHS for a $geometry
// obj, or implicitly as the RHS of a $geoNear.
void appendGeoJSONObj(BSONObjBuilder& bob,
                      const BSONObj& geometryObj,
                      const SerializationOptions& opts) {
    auto typeElem = geometryObj[kGeometryTypeField];
    if (typeElem) {
        bob.append(typeElem);
    }
    if (auto coordinatesElem = geometryObj[kGeometryCoordinatesField]) {
        appendGeoJSONCoordinatesLiteral(bob, coordinatesElem, typeElem, opts);
    } else if (auto geometriesElem = geometryObj[GEOJSON_GEOMETRIES]) {
        // We have a collection of geometries rather than a single one. Recursively
        // serialize them and add to the output object.
        BSONArrayBuilder geometriesArrBuilder;
        for (const auto& geometry : geometriesElem.Array()) {
            BSONObjBuilder geometryBuilder;
            appendGeoJSONObj(geometryBuilder, geometry.Obj(), opts);
            geometriesArrBuilder.append(geometryBuilder.obj());
        }
        bob.append(GEOJSON_GEOMETRIES, geometriesArrBuilder.arr());
    }

    // 'crs' can be present if users want to use STRICT_SPHERE coordinate
    // system.
    if (auto crsElt = geometryObj[kCrsField]) {
        appendCRSObject(bob, crsElt, opts);
    }
}

/* appendGeometryOperator() implements the serialization of a $geometry, which could be either an
 * Array or Object. This code is closely tied to GeometryContainer parsing. During GeometryContainer
 * parsing, if the $geometry element is an array type, or if the first element of the object is a
 * number, we parse the element as a Point. Parsing a legacy point does not enforce specific field
 * names, the only constraint is that the coordinates be numeric. So you can essentially have a
 * $geometry: {"foo":1, "banana": 2} and it will interpret is as a point with x:1, y: 2.
 */
void appendGeometryOperator(BSONObjBuilder& bob,
                            const BSONElement& geometryElem,
                            const SerializationOptions& opts) {
    if (geometryElem.type() == BSONType::array) {
        // This would be like {$geometry: [0, 0]} which must be a point.
        auto asArray = geometryElem.Array();
        tassert(7539807,
                "Expected the point to have exactly 2 elements: an x and y.",
                asArray.size() == 2UL);
        bob.appendArray(
            kGeometryField,
            BSON_ARRAY(opts.serializeLiteral(asArray[0]) << opts.serializeLiteral(asArray[1])));
    } else {
        const auto& geometryObj = geometryElem.Obj();
        BSONObjBuilder nestedSubObj = bob.subobjStart(kGeometryField);
        if (geometryObj.firstElement().isNumber()) {
            BSONElement x, y;
            auto status = GeoParser::parseFlatPointCoordinates(geometryElem, x, y);
            tassert(8548500,
                    "Expected the point to have exactly 2 elements: an x and y.",
                    status.isOK());
            opts.appendLiteral(&nestedSubObj, x);
            opts.appendLiteral(&nestedSubObj, y);
        } else {
            appendGeoJSONObj(nestedSubObj, geometryObj, opts);
        }
        nestedSubObj.doneFast();
    }
}

/**
 * Typically, geoNear expressions have a single embedded object under the top-level
 * geoNear operator, but there is an exception for syntax that allows geoJSON
 * coordinates without specifying $geometry (e.g., {$nearSphere: {type: 'Point',
 * coordinates: [1,2]}}). We're iterating outer_it and appending extra literals to
 * handle $minDistance and $maxDistance fields that could be included outside the
 * primary geo object in those edge cases (e.g., {$nearSphere: {type: 'Point',
 * coordinates: [1,2]}, $minDistance: 10}).
 */
void appendGeoNearOperator(BSONObjBuilder& bob,
                           StringData fieldName,
                           const BSONElement& geoNearElem,
                           const SerializationOptions& opts) {
    if (geoNearElem.type() == BSONType::array) {
        appendGeoNearLegacyArray(bob, geoNearElem, opts);
    } else {
        auto geoNearObj = geoNearElem.Obj();
        BSONObjIterator embedded_it(geoNearObj);
        tassert(8548501, "Expected non-empty geometry object.", embedded_it.more());

        // If the first element of the embedded object is numeric, we could be dealing
        // with a legacy style embedded coordinate pair (a coordinate pair modeled like {<field1>:
        // <x>, <field2>: <y>}).
        if (geoNearObj.firstElement().isNumber()) {
            BSONElement x, y;
            auto status = GeoParser::parseFlatPointCoordinates(geoNearElem, x, y);
            // If we successfully parsed a legacy flat point as an embedded object, we
            // can return as we are done parsing the geometry.
            if (status.isOK()) {
                // We convert from embedded object {x: 0, y: 0} to legacy array [0, 0] so that those
                // Point formats have the same query shape.
                BSONObj legacyCoordinates = BSON(fieldName << BSON_ARRAY(x.number() << y.number()));
                appendGeoNearLegacyArray(bob, legacyCoordinates.firstElement(), opts);
                return;
            }
            BSONElement maxDist;
            status = GeoParser::parseLegacyPointWithMaxDistance(geoNearElem, x, y, maxDist);
            if (status.isOK()) {
                BSONObj legacyCoordinates =
                    BSON(fieldName << BSON_ARRAY(x.number() << y.number() << maxDist.number()));
                appendGeoNearLegacyArray(bob, legacyCoordinates.firstElement(), opts);
                return;
            }
        }

        BSONObjBuilder subObj = BSONObjBuilder(bob.subobjStart(fieldName));
        // We couldn't parse a legacy coordinate pair expressed as an embedded object, so we
        // enumerate the embedded geometry obj and parse the internals. Typically we expect to find
        // $geometry, a GeoJSONPoint or $minDistance/$maxDistance/$uniqueDocs.
        while (embedded_it.more()) {
            auto embeddedElem = embedded_it.next();
            auto embeddedFieldName = embeddedElem.fieldNameStringData();
            if (embeddedFieldName == kGeometryField) {
                appendGeometryOperator(subObj, embeddedElem, opts);
            } else if (embeddedFieldName == kCrsField || embeddedFieldName == kGeometryTypeField ||
                       embeddedFieldName == kGeometryCoordinatesField) {
                appendGeoJSONObj(subObj, geoNearObj, opts);
            } else {
                // $minDistance/$maxDistance/$uniqueDocs.
                opts.appendLiteral(&subObj, embeddedElem);
            }
        }
        subObj.doneFast();
    }
}
}  // namespace

/**
 * geoNearExpressionCustomSerialization implements the serialization of geoNear queries (i.e $near,
 * $geoNear, $nearSphere). GeoNear queries can be broken down into two styles of queries:
 * 1. Legacy Style (see GeoNearExpression::parseLegacyQuery()):
 *     t.find({ loc : { $nearSphere: [0,0], $minDistance: 1, $maxDistance: 3 }})
 *     t.find({ loc : { $nearSphere: [0,0] }})
 *     t.find({ loc : { $near : [0, 0, 1] } });
 *     t.find({ loc : { $near: { someGeoJSONPoint}})
 *     t.find({ loc : { $geoNear: { someGeoJSONPoint}})
 * 2. New style queries:
 *     t.find({loc: {$geoNear : { $geometry: {someGeoJSONPoint}, $maxDistance:3 }}})
 */
void geoNearExpressionCustomSerialization(BSONObjBuilder& bob,
                                          const BSONObj& obj,
                                          const SerializationOptions& opts,
                                          bool includePath) {
    BSONObjIterator outer_it(obj);
    while (outer_it.more()) {
        auto elem = outer_it.next();
        if (elem.isABSONObj()) {
            StringData fieldName = elem.fieldNameStringData();
            if (fieldName == kNearField || fieldName == kGeoNearField ||
                fieldName == kNearSphereField) {
                appendGeoNearOperator(bob, fieldName, elem, opts);
            } else if (fieldName == kGeometryField) {
                appendGeometryOperator(bob, elem, opts);
            } else {
                opts.appendLiteral(&bob, elem);
            }
        } else {
            // Here we only expect to see $minDistance/$maxDistance/$uniqueDocs
            opts.appendLiteral(&bob, elem);
        }
    }
}

/**
 * geoExpressionCustomSerialization() implements the serialization of geoExpressions ($within,
 * $geoWithin, $geoIntersects). Examples of such expressions are:
 * { $geoWithin : { $geometry : <GeoJSON> } }
 * { $geoIntersects : { $geometry : <GeoJSON> } }
 * { $geoWithin : { $box : [[x1, y1], [x2, y2]] } }
 * { $geoWithin : { $polygon : [[x1, y1], [x1, y2], [x2, y2], [x2, y1]] } }
 * { $geoWithin : { $center : [[x1, y1], r], } }
 * { $geoWithin : { $centerSphere : [[x, y], radius] } }
 * { $geoIntersects : { $geometry : [1, 2] } }
 */
void geoExpressionCustomSerialization(BSONObjBuilder& bob,
                                      const BSONObj& obj,
                                      const SerializationOptions& opts,
                                      bool includePath) {
    BSONObjIterator outerIt(obj);
    BSONElement geoExprElem = outerIt.next();
    tassert(8548502, "Invalid extra fields in geo expression.", !outerIt.more());
    tassert(8548503, "Geo expression must be an object.", geoExprElem.type() == BSONType::object);
    auto fieldName = geoExprElem.fieldNameStringData();

    BSONObjBuilder subObj = BSONObjBuilder(bob.subobjStart(fieldName));
    auto geoObj = geoExprElem.Obj();
    BSONObjIterator embedded_it(geoObj);
    while (embedded_it.more()) {
        auto elem = embedded_it.next();
        fieldName = elem.fieldNameStringData();
        // $geoWithin/$geoIntersects can only have a $geometry or shape operators as operands
        // (i.e $polygon, $box, etc.)
        if (fieldName == kGeometryField) {
            appendGeometryOperator(subObj, elem, opts);
            break;
        } else {
            appendShapeOperator(subObj, elem, opts);
        }
    }
}
}  // namespace mongo
