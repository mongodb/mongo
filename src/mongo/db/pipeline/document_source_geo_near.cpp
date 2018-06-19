/**
 * Copyright 2011 (c) 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_geo_near.h"

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;

constexpr StringData DocumentSourceGeoNear::kKeyFieldName;
constexpr const char* DocumentSourceGeoNear::kStageName;

REGISTER_DOCUMENT_SOURCE(geoNear,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceGeoNear::createFromBson);

Value DocumentSourceGeoNear::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    MutableDocument result;

    if (keyFieldPath) {
        result.setField(kKeyFieldName, Value(keyFieldPath->fullPath()));
    }

    if (coordsIsArray) {
        result.setField("near", Value(BSONArray(coords)));
    } else {
        result.setField("near", Value(coords));
    }

    result.setField("distanceField", Value(distanceField->fullPath()));

    if (maxDistance) {
        result.setField("maxDistance", Value(*maxDistance));
    }

    if (minDistance) {
        result.setField("minDistance", Value(*minDistance));
    }

    result.setField("query", Value(query));
    result.setField("spherical", Value(spherical));
    if (distanceMultiplier) {
        result.setField("distanceMultiplier", Value(*distanceMultiplier));
    }

    if (includeLocs)
        result.setField("includeLocs", Value(includeLocs->fullPath()));

    return Value(DOC(getSourceName() << result.freeze()));
}

intrusive_ptr<DocumentSourceGeoNear> DocumentSourceGeoNear::create(
    const intrusive_ptr<ExpressionContext>& pCtx) {
    intrusive_ptr<DocumentSourceGeoNear> source(new DocumentSourceGeoNear(pCtx));
    return source;
}

intrusive_ptr<DocumentSource> DocumentSourceGeoNear::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pCtx) {
    intrusive_ptr<DocumentSourceGeoNear> out = new DocumentSourceGeoNear(pCtx);
    out->parseOptions(elem.embeddedObjectUserCheck());
    return out;
}

void DocumentSourceGeoNear::parseOptions(BSONObj options) {
    // First, check for explicitly-disallowed fields.

    // The old geoNear command used to accept a collation. We explicitly ban it here, since the
    // $geoNear stage should respect the collation associated with the entire pipeline.
    uassert(40227,
            "$geoNear does not accept the 'collation' parameter. Instead, specify a collation "
            "for the entire aggregation command.",
            !options["collation"]);

    // The following fields were present in older versions but are no longer supported.
    uassert(50858,
            "$geoNear no longer supports the 'limit' parameter. Use a $limit stage instead.",
            !options["limit"]);
    uassert(50857,
            "$geoNear no longer supports the 'num' parameter. Use a $limit stage instead.",
            !options["num"]);
    uassert(50856, "$geoNear no longer supports the 'start' argument.", !options["start"]);

    // The "near" and "distanceField" parameters are required.
    uassert(16605,
            "$geoNear requires a 'near' option as an Array",
            options["near"].isABSONObj());  // Array or Object (Object is deprecated)
    coordsIsArray = options["near"].type() == Array;
    coords = options["near"].embeddedObject().getOwned();

    uassert(16606,
            "$geoNear requires a 'distanceField' option as a String",
            options["distanceField"].type() == String);
    distanceField.reset(new FieldPath(options["distanceField"].str()));

    // The remaining fields are optional.
    if (auto maxDistElem = options["maxDistance"]) {
        uassert(ErrorCodes::TypeMismatch,
                "maxDistance must be a number",
                isNumericBSONType(maxDistElem.type()));
        maxDistance = options["maxDistance"].numberDouble();
        uassert(ErrorCodes::BadValue, "maxDistance must be nonnegative", *maxDistance >= 0);
    }

    if (auto minDistElem = options["minDistance"]) {
        uassert(ErrorCodes::TypeMismatch,
                "minDistance must be a number",
                isNumericBSONType(minDistElem.type()));
        minDistance = options["minDistance"].numberDouble();
        uassert(ErrorCodes::BadValue, "minDistance must be nonnegative", *minDistance >= 0);
    }

    if (auto distMultElem = options["distanceMultiplier"]) {
        uassert(ErrorCodes::TypeMismatch,
                "distanceMultiplier must be a number",
                isNumericBSONType(distMultElem.type()));
        distanceMultiplier = options["distanceMultiplier"].numberDouble();
        uassert(ErrorCodes::BadValue,
                "distanceMultiplier must be nonnegative",
                *distanceMultiplier >= 0);
    }

    if (auto queryElem = options["query"]) {
        uassert(ErrorCodes::TypeMismatch,
                "query must be an object",
                queryElem.type() == BSONType::Object);
        query = queryElem.embeddedObject().getOwned();
    }

    spherical = options["spherical"].trueValue();

    if (options.hasField("includeLocs")) {
        uassert(16607,
                "$geoNear requires that 'includeLocs' option is a String",
                options["includeLocs"].type() == String);
        includeLocs = FieldPath(options["includeLocs"].str());
    }

    if (options.hasField("uniqueDocs"))
        warning() << "ignoring deprecated uniqueDocs option in $geoNear aggregation stage";

    if (auto keyElt = options[kKeyFieldName]) {
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "$geoNear parameter '" << DocumentSourceGeoNear::kKeyFieldName
                              << "' must be of type string but found type: "
                              << typeName(keyElt.type()),
                keyElt.type() == BSONType::String);
        const auto keyFieldStr = keyElt.valueStringData();
        uassert(ErrorCodes::BadValue,
                str::stream() << "$geoNear parameter '" << DocumentSourceGeoNear::kKeyFieldName
                              << "' cannot be the empty string",
                !keyFieldStr.empty());
        keyFieldPath = FieldPath(keyFieldStr);
    }
}

BSONObj DocumentSourceGeoNear::asNearQuery(StringData nearFieldName) const {
    BSONObjBuilder queryBuilder;
    queryBuilder.appendElements(query);

    BSONObjBuilder nearBuilder(queryBuilder.subobjStart(nearFieldName));
    if (spherical) {
        if (coordsIsArray) {
            nearBuilder.appendArray("$nearSphere", coords);
        } else {
            nearBuilder.append("$nearSphere", coords);
        }
    } else {
        if (coordsIsArray) {
            nearBuilder.appendArray("$near", coords);
        } else {
            nearBuilder.append("$near", coords);
        }
    }
    if (minDistance) {
        nearBuilder.append("$minDistance", *minDistance);
    }
    if (maxDistance) {
        nearBuilder.append("$maxDistance", *maxDistance);
    }
    nearBuilder.doneFast();
    return queryBuilder.obj();
}

bool DocumentSourceGeoNear::needsGeoNearPoint() const {
    return static_cast<bool>(includeLocs);
}

DocumentSource::GetDepsReturn DocumentSourceGeoNear::getDependencies(DepsTracker* deps) const {
    // TODO (SERVER-35424): Implement better dependency tracking. For example, 'distanceField' is
    // produced by this stage, and we could inform the query system that it need not include it in
    // its response. For now, assume that we require the entire document as well as the appropriate
    // geoNear metadata.
    deps->setNeedsMetadata(DepsTracker::MetadataType::GEO_NEAR_DISTANCE, true);
    deps->setNeedsMetadata(DepsTracker::MetadataType::GEO_NEAR_POINT, needsGeoNearPoint());

    deps->needWholeDocument = true;
    return GetDepsReturn::EXHAUSTIVE_FIELDS;
}

DocumentSourceGeoNear::DocumentSourceGeoNear(const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx), coordsIsArray(false), spherical(false) {}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceGeoNear::getMergeSources() {
    return {DocumentSourceSort::create(
        pExpCtx, BSON(distanceField->fullPath() << 1 << "$mergePresorted" << true))};
}
}  // namespace mongo
