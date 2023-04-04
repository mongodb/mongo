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


#include "mongo/platform/basic.h"

#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_internal_compute_geo_near_distance.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_match.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

using boost::intrusive_ptr;

constexpr StringData DocumentSourceGeoNear::kKeyFieldName;

REGISTER_DOCUMENT_SOURCE(geoNear,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceGeoNear::createFromBson,
                         AllowedWithApiStrict::kAlways);

Value DocumentSourceGeoNear::serialize(SerializationOptions opts) const {
    MutableDocument result;

    if (keyFieldPath) {
        result.setField(kKeyFieldName, Value(opts.serializeFieldPath(*keyFieldPath)));
    }

    auto nearValue = [&]() -> Value {
        if (auto constGeometry = dynamic_cast<ExpressionConstant*>(_nearGeometry.get());
            constGeometry) {
            return opts.serializeLiteralValue(constGeometry->getValue());
        } else {
            return _nearGeometry->serialize(opts);
        }
    }();
    result.setField("near", nearValue);
    result.setField("distanceField", Value(opts.serializeFieldPath(*distanceField)));

    if (maxDistance) {
        result.setField("maxDistance", opts.serializeLiteralValue(*maxDistance));
    }

    if (minDistance) {
        result.setField("minDistance", opts.serializeLiteralValue(*minDistance));
    }

    if (opts.redactIdentifiers || opts.replacementForLiteralArgs) {
        auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(query, pExpCtx));
        result.setField("query", Value(matchExpr->serialize(opts)));
    } else {
        result.setField("query", Value(query));
    }
    result.setField("spherical", opts.serializeLiteralValue(spherical));
    if (distanceMultiplier) {
        result.setField("distanceMultiplier", opts.serializeLiteralValue(*distanceMultiplier));
    }

    if (includeLocs)
        result.setField("includeLocs", Value(opts.serializeFieldPath(*includeLocs)));

    return Value(DOC(getSourceName() << result.freeze()));
}

boost::intrusive_ptr<DocumentSource> DocumentSourceGeoNear::optimize() {
    _nearGeometry = _nearGeometry->optimize();
    return this;
}

Pipeline::SourceContainer::iterator DocumentSourceGeoNear::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {

    // Currently this is the only rewrite.
    itr = splitForTimeseries(itr, container);

    return itr;
}

Pipeline::SourceContainer::iterator DocumentSourceGeoNear::splitForTimeseries(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    // Only do this rewrite if we are immediately following an $_internalUnpackBucket stage.
    if (container->begin() == itr ||
        !dynamic_cast<DocumentSourceInternalUnpackBucket*>(std::prev(itr)->get()))
        return std::next(itr);

    // If _nearGeometry is not a constant, do nothing.
    // It might be constant in a future call to optimizeAt(), once any variables have been filled
    // in.
    _nearGeometry = _nearGeometry->optimize();
    auto nearConst = dynamic_cast<ExpressionConstant*>(_nearGeometry.get());
    if (!nearConst)
        return std::next(itr);

    // If the user didn't specify a field name to query, do nothing.
    // Normally when we use a DocumentSourceGeoNearCursor we infer this from the presence of an
    // index, but when we use an explicit $sort there might not be an index involved.
    if (!keyFieldPath)
        return std::next(itr);

    tassert(5860206, "$geoNear distanceField unexpectedly null", distanceField);

    // In this case, we know:
    // - there are stages before us
    // - the query point is a known constant
    // - the field name

    // It's fine for this error message to say "on a time-series collection" because we only get
    // here when an $_internalUnpackBucket stage precedes us.
    uassert(5860207,
            "Must not specify 'query' for $geoNear on a time-series collection; use $match instead",
            query.isEmpty());
    uassert(
        5860208, "$geoNear 'includeLocs' is not supported on time-series metrics", !includeLocs);

    // Replace the stage with $geoWithin, $computeGeoDistance, $sort.

    // Use GeoNearExpression to parse the arguments. This makes it easier to handle a variety of
    // cases: for example, if the query point is GeoJSON, then 'spherical' is implicitly true.
    GeoNearExpression nearExpr;
    // asNearQuery() is something like '{fieldName: {$near: ...}}'.
    // GeoNearExpression seems to want something like '{$near: ...}'.
    auto nearQuery = asNearQuery(keyFieldPath->fullPath()).firstElement().Obj().getOwned();
    auto exprStatus = nearExpr.parseFrom(nearQuery);
    uassert(6330900,
            str::stream() << "Unable to parse geoNear query: " << exprStatus.reason(),
            exprStatus.isOK());
    tassert(5860204,
            "Unexpected GeoNearExpression field name after asNearQuery(): "_sd + nearExpr.field,
            nearExpr.field == ""_sd);

    Pipeline::SourceContainer replacement;
    // 1. $geoWithin maxDistance
    //    We always include a $geoWithin predicate, even if maxDistance covers the entire space,
    //    because it takes care of excluding documents that don't have the geo field we're querying.
    if (nearExpr.centroid->crs == SPHERE) {
        // {$match: {field: {$geoWithin: {$centerSphere: [[x, y], radiusRadians]}}}}
        double x = nearExpr.centroid->oldPoint.x;
        double y = nearExpr.centroid->oldPoint.y;
        auto radiusRadians = [&](double radius) -> double {
            if (nearExpr.unitsAreRadians) {
                // In this mode, $geoNear interprets the given maxDistance as radians.
                return radius;
            } else {
                // Otherwise it interprets maxDistance as meters.
                auto maxDistanceMeters = radius;
                return maxDistanceMeters / kRadiusOfEarthInMeters;
            }
        };
        replacement.push_back(DocumentSourceMatch::create(
            BSON(keyFieldPath->fullPath()
                 << BSON("$geoWithin"
                         << BSON("$centerSphere" << BSON_ARRAY(
                                     BSON_ARRAY(x << y) << radiusRadians(nearExpr.maxDistance))))),
            pExpCtx));

        if (minDistance) {
            // Also include an inside-out $geoWithin. This query is imprecise due to rounding error,
            // so we will include an additional, precise filter later in the pipeline.
            double antipodalX = x < 0 ? x + 180 : x - 180;
            double antipodalY = -y;
            double insideOutRadiusRadians = M_PI - radiusRadians(nearExpr.minDistance);
            if (insideOutRadiusRadians > 0) {
                replacement.push_back(DocumentSourceMatch::create(
                    BSON(keyFieldPath->fullPath()
                         << BSON("$geoWithin" << BSON("$centerSphere" << BSON_ARRAY(
                                                          BSON_ARRAY(antipodalX << antipodalY)
                                                          << insideOutRadiusRadians)))),
                    pExpCtx));
            }
        }
    } else if (nearExpr.centroid->crs == FLAT) {
        // {$match: {field: {$geoWithin: {$center: [[x, y], radius]}}}}
        tassert(5860205,
                "'isNearSphere' should have resulted in a SPHERE crs.",
                !nearExpr.isNearSphere);
        auto x = nearExpr.centroid->oldPoint.x;
        auto y = nearExpr.centroid->oldPoint.y;
        replacement.push_back(DocumentSourceMatch::create(
            BSON(keyFieldPath->fullPath()
                 << BSON("$geoWithin" << BSON(
                             "$center" << BSON_ARRAY(BSON_ARRAY(x << y) << nearExpr.maxDistance)))),
            pExpCtx));

        if (std::isnormal(nearExpr.minDistance)) {
            // $geoWithin includes its boundary, so a negated $geoWithin excludes the boundary.
            // So we need to tweak the radius here to include those points on the boundary.
            // This means this filter is approximate, so we'll include an additional filter for
            // minDistance after unpacking.

            // Making the radius 1% smaller seems like a big enough tweak that we won't miss any
            // boundary points, and a small enough tweak to still be selective. It also preserves
            // the sign of minDistance (whereas subtracting an epsilon wouldn't, necessarily).
            // Only do this when isnormal(minDistance), to ensure we have enough bits of precision.
            auto radius = 0.99 * nearExpr.minDistance;
            replacement.push_back(DocumentSourceMatch::create(
                BSON(keyFieldPath->fullPath() << BSON(
                         "$not" << BSON("$geoWithin" << BSON("$center" << BSON_ARRAY(
                                                                 BSON_ARRAY(x << y) << radius))))),
                pExpCtx));
        }
    } else {
        tasserted(5860203, "Expected coordinate system to be either SPHERE or FLAT.");
    }

    // 2. Compute geo distance.
    {
        auto multiplier = (distanceMultiplier ? *distanceMultiplier : 1.0);
        if (nearExpr.unitsAreRadians) {
            // In this mode, $geoNear would report distances in radians instead of meters.
            // To imitate this behavior, we need to scale down here too.
            multiplier /= kRadiusOfEarthInMeters;
        }

        auto coords = nearExpr.centroid->crs == SPHERE
            ? BSON("near" << BSON("type"
                                  << "Point"
                                  << "coordinates"
                                  << BSON_ARRAY(nearExpr.centroid->oldPoint.x
                                                << nearExpr.centroid->oldPoint.y)))
            : BSON("near" << BSON_ARRAY(nearExpr.centroid->oldPoint.x
                                        << nearExpr.centroid->oldPoint.y));
        tassert(5860220, "", coords.firstElement().isABSONObj());

        auto centroid = std::make_unique<PointWithCRS>();
        tassert(GeoParser::parseQueryPoint(coords.firstElement(), centroid.get())
                    .withContext("parsing centroid for $geoNear time-series rewrite"));

        replacement.push_back(make_intrusive<DocumentSourceInternalGeoNearDistance>(
            pExpCtx,
            keyFieldPath->fullPath(),
            std::move(centroid),
            coords.firstElement().Obj().getOwned(),
            distanceField->fullPath(),
            multiplier));
    }

    // 3. Filter precisely by minDistance / maxDistance.
    if (minDistance) {
        // 'minDistance' does not take 'distanceMultiplier' into account.
        replacement.push_back(DocumentSourceMatch::create(
            BSON(distanceField->fullPath() << BSON(
                     "$gte" << *minDistance * (distanceMultiplier ? *distanceMultiplier : 1.0))),
            pExpCtx));
    }
    if (maxDistance) {
        // 'maxDistance' does not take 'distanceMultiplier' into account.
        replacement.push_back(DocumentSourceMatch::create(
            BSON(distanceField->fullPath() << BSON(
                     "$lte" << *maxDistance * (distanceMultiplier ? *distanceMultiplier : 1.0))),
            pExpCtx));
    }

    // 4. $sort by geo distance.
    {
        // {$sort: {dist: 1}}
        replacement.push_back(DocumentSourceSort::create(pExpCtx,
                                                         SortPattern({
                                                             {true, *distanceField, nullptr},
                                                         })));
    }

    LOGV2_DEBUG(5860209,
                5,
                "$geoNear splitForTimeseries",
                "pipeline"_attr = Pipeline::serializeContainer(*container),
                "replacement"_attr = Pipeline::serializeContainer(replacement));

    auto prev = std::prev(itr);
    std::move(replacement.begin(), replacement.end(), std::inserter(*container, itr));
    container->erase(itr);
    return std::next(prev);
}

intrusive_ptr<DocumentSourceGeoNear> DocumentSourceGeoNear::create(
    const intrusive_ptr<ExpressionContext>& pCtx) {
    intrusive_ptr<DocumentSourceGeoNear> source(new DocumentSourceGeoNear(pCtx));
    return source;
}

intrusive_ptr<DocumentSource> DocumentSourceGeoNear::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pCtx) {
    intrusive_ptr<DocumentSourceGeoNear> out = new DocumentSourceGeoNear(pCtx);
    out->parseOptions(elem.embeddedObjectUserCheck(), pCtx);
    return out;
}

bool DocumentSourceGeoNear::hasQuery() const {
    return true;
}

void DocumentSourceGeoNear::parseOptions(BSONObj options,
                                         const boost::intrusive_ptr<ExpressionContext>& pCtx) {
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
    uassert(5860400, "$geoNear requires a 'near' argument", options["near"]);
    _nearGeometry =
        Expression::parseOperand(pCtx.get(), options["near"], pCtx->variablesParseState);

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
        LOGV2_WARNING(23758, "ignoring deprecated uniqueDocs option in $geoNear aggregation stage");

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

BSONObj DocumentSourceGeoNear::asNearQuery(StringData nearFieldName) {
    BSONObjBuilder queryBuilder;
    queryBuilder.appendElements(query);

    BSONObjBuilder nearBuilder(queryBuilder.subobjStart(nearFieldName));

    auto opName = spherical ? "$nearSphere" : "$near";
    optimize();
    if (auto constGeometry = dynamic_cast<ExpressionConstant*>(_nearGeometry.get());
        constGeometry) {
        auto geomValue = constGeometry->getValue();
        uassert(5860401,
                "$geoNear requires near argument to be a GeoJSON object or a legacy point(array)",
                geomValue.isObject() || geomValue.isArray());
        geomValue.addToBsonObj(&nearBuilder, opName);
    } else {
        uassert(5860402, "$geoNear requires a constant near argument", constGeometry);
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

DepsTracker::State DocumentSourceGeoNear::getDependencies(DepsTracker* deps) const {
    expression::addDependencies(_nearGeometry.get(), deps);
    // TODO (SERVER-35424): Implement better dependency tracking. For example, 'distanceField' is
    // produced by this stage, and we could inform the query system that it need not include it in
    // its response. For now, assume that we require the entire document as well as the appropriate
    // geoNear metadata.
    deps->setNeedsMetadata(DocumentMetadataFields::kGeoNearDist, true);
    deps->setNeedsMetadata(DocumentMetadataFields::kGeoNearPoint, needsGeoNearPoint());

    deps->needWholeDocument = true;
    return DepsTracker::State::EXHAUSTIVE_FIELDS;
}

void DocumentSourceGeoNear::addVariableRefs(std::set<Variables::Id>* refs) const {
    expression::addVariableRefs(_nearGeometry.get(), refs);
}

DocumentSourceGeoNear::DocumentSourceGeoNear(const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(kStageName, pExpCtx), spherical(false) {}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceGeoNear::distributedPlanLogic() {
    // {shardsStage, mergingStage, sortPattern}
    return DistributedPlanLogic{this, nullptr, BSON(distanceField->fullPath() << 1)};
}

}  // namespace mongo
