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


#include "mongo/db/pipeline/document_source_geo_near.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/pipeline/document_source_internal_compute_geo_near_distance.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_geo_parser.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/logv2/log.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <s2cellid.h>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

using boost::intrusive_ptr;

constexpr StringData DocumentSourceGeoNear::kKeyFieldName;

REGISTER_DOCUMENT_SOURCE(geoNear,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceGeoNear::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(geoNear, DocumentSourceGeoNear::id)

Value DocumentSourceGeoNear::serialize(const SerializationOptions& opts) const {
    MutableDocument result;

    if (keyFieldPath) {
        result.setField(kKeyFieldName, Value(opts.serializeFieldPath(*keyFieldPath)));
    }

    // Serialize the expression as a literal if possible
    auto serializeExpr = [&](boost::intrusive_ptr<Expression> expr) -> Value {
        if (auto constExpr = dynamic_cast<ExpressionConstant*>(expr.get()); constExpr) {
            return opts.serializeLiteral(constExpr->getValue());
        } else {
            return expr->serialize(opts);
        }
    };

    result.setField("near", serializeExpr(_nearGeometry));
    if (distanceField) {
        result.setField("distanceField", Value(opts.serializeFieldPath(*distanceField)));
    }

    if (maxDistance) {
        result.setField("maxDistance", serializeExpr(maxDistance));
    }

    if (minDistance) {
        result.setField("minDistance", serializeExpr(minDistance));
    }

    // When the query is missing, we serialize to an empty document here (instead of omitting) in
    // order to maintain stability of the query shape hash for this stage. See SERVER-104645.
    result.setField("query",
                    query ? Value(query->getMatchExpression()->serialize(opts)) : Value{BSONObj{}});
    result.setField("spherical", opts.serializeLiteral(spherical));
    if (distanceMultiplier) {
        result.setField("distanceMultiplier", opts.serializeLiteral(*distanceMultiplier));
    }

    if (includeLocs)
        result.setField("includeLocs", Value(opts.serializeFieldPath(*includeLocs)));

    return Value(DOC(getSourceName() << result.freeze()));
}

boost::intrusive_ptr<DocumentSource> DocumentSourceGeoNear::optimize() {
    _nearGeometry = _nearGeometry->optimize();
    if (minDistance) {
        minDistance = minDistance->optimize();
    }
    if (maxDistance) {
        maxDistance = maxDistance->optimize();
    }
    return this;
}

DocumentSourceContainer::iterator DocumentSourceGeoNear::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {

    // Currently this is the only rewrite.
    itr = splitForTimeseries(itr, container);

    return itr;
}

DocumentSourceContainer::iterator DocumentSourceGeoNear::splitForTimeseries(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    tassert(9911904, "", *itr == this);

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


    if (minDistance) {
        minDistance = minDistance->optimize();
        auto minConst = dynamic_cast<ExpressionConstant*>(minDistance.get());
        if (!minConst)
            return std::next(itr);
    }

    if (maxDistance) {
        maxDistance = maxDistance->optimize();
        auto maxConst = dynamic_cast<ExpressionConstant*>(maxDistance.get());
        if (!maxConst)
            return std::next(itr);
    }

    // If the user didn't specify a field name to query, do nothing.
    // Normally when we use a DocumentSourceGeoNearCursor we infer this from the presence of an
    // index, but when we use an explicit $sort there might not be an index involved.
    if (!keyFieldPath)
        return std::next(itr);

    // TODO SERVER-97616: Make distanceField optional for time-series.
    uassert(5860206, "$geoNear distanceField is required for time-series queries", distanceField);

    // In this case, we know:
    // - there are stages before us
    // - the query point is a known constant
    // - the field name

    // It's fine for this error message to say "on a time-series collection" because we only get
    // here when an $_internalUnpackBucket stage precedes us.
    uassert(5860207,
            "Must not specify 'query' for $geoNear on a time-series collection; use $match instead",
            !query);
    uassert(
        5860208, "$geoNear 'includeLocs' is not supported on time-series metrics", !includeLocs);

    // Replace the stage with $geoWithin, $computeGeoDistance, $sort.

    // Use GeoNearExpression to parse the arguments. This makes it easier to handle a variety of
    // cases: for example, if the query point is GeoJSON, then 'spherical' is implicitly true.
    GeoNearExpression nearExpr;
    // asNearQuery() is something like '{fieldName: {$near: ...}}'.
    // GeoNearExpression seems to want something like '{$near: ...}'.
    auto nearQuery = asNearQuery(keyFieldPath->fullPath()).firstElement().Obj().getOwned();
    auto exprStatus = parsers::matcher::parseGeoNearExpressionFromBSON(nearQuery, nearExpr);
    uassert(6330900,
            str::stream() << "Unable to parse geoNear query: " << exprStatus.reason(),
            exprStatus.isOK());
    tassert(5860204,
            "Unexpected GeoNearExpression field name after asNearQuery(): "_sd + nearExpr.field,
            nearExpr.field == ""_sd);

    DocumentSourceContainer replacement;
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
            getExpCtx()));

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
                    getExpCtx()));
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
            getExpCtx()));

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
                getExpCtx()));
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
            ? BSON("near" << BSON("type" << "Point"
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
            getExpCtx(),
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
            BSON(distanceField->fullPath()
                 << BSON("$gte" << nearExpr.minDistance *
                             (distanceMultiplier ? *distanceMultiplier : 1.0))),
            getExpCtx()));
    }
    if (maxDistance) {
        // 'maxDistance' does not take 'distanceMultiplier' into account.
        replacement.push_back(DocumentSourceMatch::create(
            BSON(distanceField->fullPath()
                 << BSON("$lte" << nearExpr.maxDistance *
                             (distanceMultiplier ? *distanceMultiplier : 1.0))),
            getExpCtx()));
    }

    // 4. $sort by geo distance.
    {
        // {$sort: {dist: 1}}
        replacement.push_back(DocumentSourceSort::create(getExpCtx(),
                                                         SortPattern({
                                                             {true, *distanceField, nullptr},
                                                         })));
    }

    LOGV2_DEBUG(5860209,
                5,
                "$geoNear splitForTimeseries",
                "pipeline"_attr = Pipeline::serializeContainerForLogging(*container),
                "replacement"_attr = Pipeline::serializeContainerForLogging(replacement));

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

    // The "near" parameter is required.
    uassert(5860400, "$geoNear requires a 'near' argument", options[kNearFieldName]);

    // go through all the fields
    for (auto&& argument : options) {
        const auto argName = argument.fieldNameStringData();
        if (argName == kNearFieldName) {
            _nearGeometry =
                Expression::parseOperand(pCtx.get(), argument, pCtx->variablesParseState);
        } else if (argName == kDistanceFieldFieldName) {
            uassert(16606,
                    "$geoNear requires that the 'distanceField' option is a String",
                    argument.type() == BSONType::string);
            distanceField = FieldPath(argument.str());
        } else if (argName == "maxDistance") {
            maxDistance = Expression::parseOperand(pCtx.get(), argument, pCtx->variablesParseState);
        } else if (argName == "minDistance") {
            minDistance = Expression::parseOperand(pCtx.get(), argument, pCtx->variablesParseState);
        } else if (argName == "distanceMultiplier") {
            uassert(ErrorCodes::TypeMismatch,
                    "distanceMultiplier must be a number",
                    isNumericBSONType(argument.type()));
            distanceMultiplier = argument.numberDouble();
            uassert(ErrorCodes::BadValue,
                    "distanceMultiplier must be nonnegative",
                    *distanceMultiplier >= 0);
        } else if (argName == "query") {
            uassert(ErrorCodes::TypeMismatch,
                    "query must be an object",
                    argument.type() == BSONType::object);
            auto queryObj = argument.embeddedObject();
            if (!queryObj.isEmpty()) {
                query = std::make_unique<Matcher>(queryObj.getOwned(), getExpCtx());
            }
        } else if (argName == "spherical") {
            spherical = argument.trueValue();
        } else if (argName == kIncludeLocsFieldName) {
            uassert(16607,
                    "$geoNear requires that 'includeLocs' option is a String",
                    argument.type() == BSONType::string);
            includeLocs = FieldPath(argument.str());
        } else if (argName == "uniqueDocs") {
            LOGV2_WARNING(23758,
                          "ignoring deprecated uniqueDocs option in $geoNear aggregation stage");
        } else if (argName == kKeyFieldName) {
            uassert(ErrorCodes::TypeMismatch,
                    str::stream() << "$geoNear parameter '" << DocumentSourceGeoNear::kKeyFieldName
                                  << "' must be of type string but found type: "
                                  << typeName(argument.type()),
                    argument.type() == BSONType::string);
            const auto keyFieldStr = argument.valueStringData();
            uassert(ErrorCodes::BadValue,
                    str::stream() << "$geoNear parameter '" << DocumentSourceGeoNear::kKeyFieldName
                                  << "' cannot be the empty string",
                    !keyFieldStr.empty());
            keyFieldPath = FieldPath(keyFieldStr);
        } else {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "Unknown argument to $geoNear: " << argument.fieldName());
        }
    }
}

BSONObj DocumentSourceGeoNear::asNearQuery(StringData nearFieldName) {
    BSONObjBuilder queryBuilder;
    queryBuilder.appendElements(getQuery());

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
        if (auto constMinDistance = dynamic_cast<ExpressionConstant*>(minDistance.get());
            constMinDistance) {
            auto minDistanceVal = constMinDistance->getValue();
            uassert(ErrorCodes::TypeMismatch,
                    "$geoNear requires $minDistance to evaluate to a number",
                    minDistanceVal.numeric());
            uassert(ErrorCodes::BadValue,
                    "minDistance must be nonnegative",
                    minDistanceVal.getDouble() >= 0);
            nearBuilder.append("$minDistance", minDistanceVal.getDouble());
        } else {
            uasserted(7555701, "$geoNear requires $minDistance to evaluate to a constant number");
        }
    }
    if (maxDistance) {
        if (auto constMaxDistance = dynamic_cast<ExpressionConstant*>(maxDistance.get());
            constMaxDistance) {
            auto maxDistanceVal = constMaxDistance->getValue();
            uassert(ErrorCodes::TypeMismatch,
                    "$geoNear requires $maxDistance to evaluate to a number",
                    maxDistanceVal.numeric());
            uassert(ErrorCodes::BadValue,
                    "maxDistance must be nonnegative",
                    maxDistanceVal.getDouble() >= 0);
            nearBuilder.append("$maxDistance", maxDistanceVal.getDouble());
        } else {
            uasserted(7555702, "$geoNear requires $maxDistance to evaluate to a constant number");
        }
    }
    nearBuilder.doneFast();
    return queryBuilder.obj();
}

bool DocumentSourceGeoNear::needsGeoNearPoint() const {
    return static_cast<bool>(includeLocs);
}

DepsTracker::State DocumentSourceGeoNear::getDependencies(DepsTracker* deps) const {
    expression::addDependencies(_nearGeometry.get(), deps);
    if (minDistance)
        expression::addDependencies(minDistance.get(), deps);
    if (maxDistance)
        expression::addDependencies(maxDistance.get(), deps);
    // TODO (SERVER-35424): Implement better dependency tracking. For example, 'distanceField' is
    // produced by this stage, and we could inform the query system that it need not include it in
    // its response. For now, assume that we require the entire document as well as the appropriate
    // geoNear metadata.

    // This may look confusing, but we must call two setters on the DepsTracker for different
    // purposes. We mark the fields as available metadata that can be consumed by any
    // downstream stage for $meta field validation. We also also mark that this stage does
    // require the meta fields so that the executor knows to produce the metadata.
    // TODO SERVER-100902 Split $meta validation out of dependency tracking.
    deps->setMetadataAvailable(DocumentMetadataFields::kGeoNearDist);
    deps->setNeedsMetadata(DocumentMetadataFields::kGeoNearDist);
    if (needsGeoNearPoint()) {
        deps->setMetadataAvailable(DocumentMetadataFields::kGeoNearPoint);
        deps->setNeedsMetadata(DocumentMetadataFields::kGeoNearPoint);
    }

    deps->needWholeDocument = true;
    return DepsTracker::State::EXHAUSTIVE_FIELDS;
}

void DocumentSourceGeoNear::addVariableRefs(std::set<Variables::Id>* refs) const {
    expression::addVariableRefs(_nearGeometry.get(), refs);
    if (minDistance)
        expression::addVariableRefs(minDistance.get(), refs);
    if (maxDistance)
        expression::addVariableRefs(maxDistance.get(), refs);
}

DocumentSourceGeoNear::DocumentSourceGeoNear(const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(kStageName, pExpCtx), spherical(false) {}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceGeoNear::distributedPlanLogic() {
    DistributedPlanLogic logic;
    logic.shardsStage = this;
    // Note that we may not output a distance field, and it may have a different name if we do.
    // This is okay because the merging logic just uses this field to determine sort direction,
    // while the sort key is made accessible in the document metadata.
    logic.mergeSortPattern = BSON("distance" << 1);
    return logic;
}

}  // namespace mongo
