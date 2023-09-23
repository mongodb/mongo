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

#include <vector>


#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {
void appendLegacyGeoLiteral(BSONObjBuilder* bob,
                            const BSONElement& e,
                            const SerializationOptions& opts) {
    if (opts.literalPolicy != LiteralSerializationPolicy::kToRepresentativeParseableValue) {
        opts.appendLiteral(bob, e);
        return;
    }

    StringData fieldName = e.fieldNameStringData();
    if (fieldName == kNearSphereField || fieldName == kNearField) {
        // Legacy $nearSphere and $near requires at minimum 2 coordinates to be
        // re-parseable, so the representative value is [1, 1].
        bob->appendArray(fieldName, BSON_ARRAY(1 << 1));
    } else if (fieldName == kCenterField || fieldName == kCenterSphereField) {
        // $center and $centerSphere requires a pair of coordinates and a radius to be
        // re-parseable, so the representative value is [[1, 1],1].
        bob->appendArray(fieldName, BSON_ARRAY(BSON_ARRAY(1 << 1) << 1));
    } else if (fieldName == kBoxField) {
        // $box requires two pairs of coordinates to be re-parseable, so the
        // representative value is [[1, 1],[1,1]].
        bob->appendArray(fieldName, BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(1 << 1)));
    } else if (fieldName == kPolygonField) {
        // $polygon requires three pairs of coordinates to be re-parseable, so the representative
        // value is [[0,0],[0,1],[1,1]].
        bob->appendArray(
            fieldName, BSON_ARRAY(BSON_ARRAY(0 << 0) << BSON_ARRAY(0 << 1) << BSON_ARRAY(1 << 1)));
    } else {
        opts.appendLiteral(bob, e);
    }
}

void appendGeoJSONCoordinatesLiteral(BSONObjBuilder* bob,
                                     const BSONElement& e,
                                     StringData geoJSONType,
                                     const SerializationOptions& opts) {
    if (opts.literalPolicy != LiteralSerializationPolicy::kToRepresentativeParseableValue) {
        opts.appendLiteral(bob, e);
        return;
    }

    StringData fieldName = e.fieldNameStringData();
    if (geoJSONType == "Polygon"_sd) {
        // Polygon requires four pairs of coordinates in a closed loop wrapped in an array to be
        // re-parseable, so the representative value is [[[0,0],[0,1],[1,1],[0,0]]].
        bob->appendArray(
            fieldName,
            BSON_ARRAY(BSON_ARRAY(BSON_ARRAY(0 << 0) << BSON_ARRAY(0 << 1) << BSON_ARRAY(1 << 1)
                                                     << BSON_ARRAY(0 << 0))));
    } else if (geoJSONType == "MultiPolygon"_sd) {
        // MultiPolygon requires four pairs of coordinates in a closed loop wrapped in 2 arrays to
        // be re-parseable, so the representative value is [[[[0,0],[0,1],[1,1],[0,0]]]].
        bob->appendArray(fieldName,
                         BSON_ARRAY(BSON_ARRAY(BSON_ARRAY(
                             BSON_ARRAY(0 << 0)
                             << BSON_ARRAY(0 << 1) << BSON_ARRAY(1 << 1) << BSON_ARRAY(0 << 0)))));
    } else if (geoJSONType == "Point"_sd) {
        // Point requires a pair of coordinates to be re-parseable, so the representative
        // value is [1,1].
        bob->appendArray(fieldName, BSON_ARRAY(1 << 1));
    } else if (geoJSONType == "MultiPoint"_sd) {
        // MultiPoint requires a pair of coordinates wrapped in an array to be re-parseable, so the
        // representative value is [[1,1]].
        bob->appendArray(fieldName, BSON_ARRAY(BSON_ARRAY(1 << 1)));
    } else if (geoJSONType == "LineString"_sd) {
        // LineString requires two pairs of coordinates to be re-parseable, so the representative
        // value is [[0,0],[1,1]].
        bob->appendArray(fieldName, BSON_ARRAY(BSON_ARRAY(0 << 0) << BSON_ARRAY(1 << 1)));
    } else {
        opts.appendLiteral(bob, e);
    }
}

void appendCRSObject(BSONObjBuilder* bob,
                     const BSONElement& crsObj,
                     const SerializationOptions& opts) {
    // 'crs' is always an object.
    tassert(7559700, "Expected 'crs' to be an object", crsObj.type() == BSONType::Object);
    // 'crs' is required to have a 'type' field with the value 'name'.
    // Additionally, it is required to have an object properties field
    // with a single 'name' field.
    tassert(7559701,
            str::stream() << "Expected 'crs' to contain a string 'type' field, got " << crsObj,
            crsObj[kCrsTypeField] && crsObj[kCrsTypeField].type() == BSONType::String);
    tassert(7559702,
            str::stream() << "Expected 'crs' to contain a 'properties' object, got , " << crsObj,
            crsObj[kCrsPropertiesField] && crsObj[kCrsPropertiesField].type() == BSONType::Object);
    tassert(7559703,
            str::stream() << "Expected 'crs.properties' to contain a 'name' "
                             "string field, got "
                          << crsObj[kCrsPropertiesField],
            crsObj[kCrsPropertiesField].Obj()[kCrsNameField] &&
                crsObj[kCrsPropertiesField].Obj()[kCrsNameField].type() == BSONType::String);

    // The CRS "type" and "properties.name" fields must be preserved for
    // kToRepresentativeParseableValue serialization policy so the query
    // shape can be re-parsed (and will be preserved for kUnchanged policy
    // as well).
    BSONObjBuilder crsObjBuilder(bob->subobjStart(kCrsField));
    if (opts.literalPolicy == LiteralSerializationPolicy::kToDebugTypeString) {
        opts.appendLiteral(&crsObjBuilder, crsObj[kCrsTypeField]);
    } else {
        crsObjBuilder.append(crsObj[kCrsTypeField]);
    }
    BSONObjBuilder crsPropBuilder(crsObjBuilder.subobjStart(kCrsPropertiesField));
    if (opts.literalPolicy == LiteralSerializationPolicy::kToDebugTypeString) {
        opts.appendLiteral(&crsPropBuilder, crsObj[kCrsPropertiesField].Obj()[kCrsNameField]);
    } else {
        crsPropBuilder.append(crsObj[kCrsPropertiesField].Obj()[kCrsNameField]);
    }
    crsPropBuilder.doneFast();
    crsObjBuilder.doneFast();
}

void appendGeometrySubObject(BSONObjBuilder* bob,
                             const BSONObj& geometryObj,
                             const SerializationOptions& opts) {
    auto typeElem = geometryObj[kGeometryTypeField];
    if (typeElem) {
        bob->append(typeElem);
    }
    if (auto coordinatesElem = geometryObj[kGeometryCoordinatesField]) {
        appendGeoJSONCoordinatesLiteral(bob, coordinatesElem, typeElem.valueStringData(), opts);
    }

    // 'crs' can be present if users want to use STRICT_SPHERE coordinate
    // system.
    if (auto crsElt = geometryObj[kCrsField]) {
        appendCRSObject(bob, crsElt, opts);
    }
}
}  // namespace

void geoCustomSerialization(BSONObjBuilder* bob,
                            const BSONObj& obj,
                            const SerializationOptions& opts) {
    BSONElement outerElem = obj.firstElement();

    // Legacy GeoNear query.
    if (outerElem.type() == mongo::Array) {
        BSONObjIterator it(obj);
        while (it.more()) {
            // In a legacy GeoNear query, the value associated with the first field ($near or
            // $geoNear) is an array where the first two array elements represent the x and y
            // coordinates respectively. An optional third array element denotes the $maxDistance.
            // Alternatively, a legacy query can have a $maxDistance suboperator to make it more
            // explicit. None of these values are enums so it is fine to treat them as literals
            // during redaction.
            appendLegacyGeoLiteral(bob, it.next(), opts);
        }
        return;
    }

    // Non-legacy geo expressions have embedded objects that have to be traversed.
    BSONObjIterator outer_it(obj);
    while (outer_it.more()) {
        auto elem = outer_it.next();

        if (!elem.isABSONObj()) {
            // Typically, geo expressions have a single embedded object under the top-level geo
            // operator, but there is an exception for syntax that allows geoJSON coordinates
            // without specifying $geometry (e.g., {$nearSphere: {type: 'Point', coordinates:
            // [1,2]}}). We're iterating outer_it and appending extra literals to handle
            // $minDistance and $maxDistance fields that could be included outside the primary geo
            // object in those edge cases (e.g., {$nearSphere: {type: 'Point', coordinates: [1,2]},
            // $minDistance: 10}).
            opts.appendLiteral(bob, elem);
        } else {
            StringData fieldName = elem.fieldNameStringData();
            BSONObjBuilder subObj = BSONObjBuilder(bob->subobjStart(fieldName));
            BSONObjIterator embedded_it(elem.embeddedObject());

            while (embedded_it.more()) {
                BSONElement argElem = embedded_it.next();
                fieldName = argElem.fieldNameStringData();
                if (fieldName == kGeometryField) {
                    if (argElem.type() == BSONType::Array) {
                        // This would be like {$geometry: [0, 0]} which must be a point.
                        auto asArray = argElem.Array();
                        tassert(7539807,
                                "Expected the point to have exactly 2 elements: an x and y.",
                                asArray.size() == 2UL);
                        subObj.appendArray(fieldName,
                                           BSON_ARRAY(opts.serializeLiteral(asArray[0])
                                                      << opts.serializeLiteral(asArray[1])));
                    } else {
                        BSONObjBuilder nestedSubObj = bob->subobjStart(kGeometryField);
                        appendGeometrySubObject(&nestedSubObj, argElem.Obj(), opts);
                        nestedSubObj.doneFast();
                    }
                } else if (fieldName == kGeometryTypeField) {
                    // This handles an edge-case where syntax allows geoJSON coordinates without
                    // specifying $geometry; e.g., {$nearSphere: {type: 'Point', coordinates:
                    // [1,2]}}.
                    appendGeometrySubObject(&subObj, elem.Obj(), opts);

                    // appendGeometrySubObj handles all fields in this subObj, so we break out of
                    // the inner loop to avoid duplicating fields.
                    break;
                } else {
                    appendLegacyGeoLiteral(&subObj, argElem, opts);
                }
            }
            subObj.doneFast();
        }
    }
}
}  // namespace mongo
