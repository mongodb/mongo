// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/document_source_internal_compute_geo_near_distance.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <utility>
#include <vector>

#include <s2cellid.h>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(_internalComputeGeoNearDistance,
                                     InternalComputeGeoNearDistanceLiteParsed::parse,
                                     AllowedWithApiStrict::kInternal);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalComputeGeoNearDistance,
                                                   DocumentSourceInternalGeoNearDistance,
                                                   InternalComputeGeoNearDistanceStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalComputeGeoNearDistance,
                            DocumentSourceInternalGeoNearDistance::id);

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalGeoNearDistance::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    auto obj = elem.embeddedObjectUserCheck();
    uassert(5874500,
            str::stream() << DocumentSourceInternalGeoNearDistance::kKeyFieldName
                          << " field is required and must be a string",
            obj.hasField(DocumentSourceInternalGeoNearDistance::kKeyFieldName) &&
                obj[DocumentSourceInternalGeoNearDistance::kKeyFieldName].type() ==
                    BSONType::string);
    uassert(5874501,
            str::stream() << DocumentSourceInternalGeoNearDistance::kNearFieldName
                          << " field is required and must be an object or array",
            obj.hasField(DocumentSourceInternalGeoNearDistance::kNearFieldName) &&
                obj[DocumentSourceInternalGeoNearDistance::kNearFieldName].isABSONObj());
    uassert(5874502,
            str::stream() << DocumentSourceInternalGeoNearDistance::kDistanceFieldFieldName
                          << " field is required and must be a string",
            obj.hasField(DocumentSourceInternalGeoNearDistance::kDistanceFieldFieldName) &&
                obj[DocumentSourceInternalGeoNearDistance::kDistanceFieldFieldName].type() ==
                    BSONType::string);
    uassert(
        5874503,
        str::stream() << DocumentSourceInternalGeoNearDistance::kDistanceMultiplierFieldName
                      << " field is required and must be a number",
        obj.hasField(DocumentSourceInternalGeoNearDistance::kDistanceMultiplierFieldName) &&
            obj[DocumentSourceInternalGeoNearDistance::kDistanceMultiplierFieldName].isNumber());
    int expectedNumArgs = 4;
    uassert(5874510,
            str::stream() << kStageName << " expected " << expectedNumArgs << " arguments but got "
                          << obj.nFields(),
            obj.nFields() == expectedNumArgs);

    auto nearElm = obj[DocumentSourceInternalGeoNearDistance::kNearFieldName];
    auto centroid = std::make_unique<PointWithCRS>();
    uassertStatusOK(GeoParser::parseQueryPoint(nearElm, centroid.get()));

    boost::intrusive_ptr<DocumentSourceInternalGeoNearDistance> out =
        new DocumentSourceInternalGeoNearDistance(
            pExpCtx,
            obj[DocumentSourceInternalGeoNearDistance::kKeyFieldName].String(),
            std::move(centroid),
            nearElm.embeddedObject().getOwned(),
            obj[DocumentSourceInternalGeoNearDistance::kDistanceFieldFieldName].String(),
            obj[DocumentSourceInternalGeoNearDistance::kDistanceMultiplierFieldName]
                .numberDouble());

    return out;
}

DocumentSourceInternalGeoNearDistance::DocumentSourceInternalGeoNearDistance(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    std::string key,
    std::unique_ptr<PointWithCRS> centroid,
    const BSONObj& coords,
    std::string distanceField,
    double distanceMultiplier)
    : DocumentSource(kStageName, pExpCtx),
      _key(std::move(key)),
      _centroid(std::move(centroid)),
      _coords(coords),
      _distanceField(std::move(distanceField)),
      _distanceMultiplier(distanceMultiplier) {}

Value getNearFieldRepresentativeValue(BSONType coordinateType) {
    if (isNumericBSONType(coordinateType)) {
        return Value({Value(1), Value(1)});
    } else {
        return Value(BSON("type" << "Point" << "coordinates" << BSON_ARRAY(1 << 1)));
    }
}

Value DocumentSourceInternalGeoNearDistance::serialize(
    const query_shape::SerializationOptions& opts) const {
    MutableDocument out;

    out.setField(DocumentSourceInternalGeoNearDistance::kNearFieldName,
                 opts.serializeLiteral(
                     _coords, getNearFieldRepresentativeValue(_coords.firstElementType())));
    out.setField(DocumentSourceInternalGeoNearDistance::kKeyFieldName,
                 Value(opts.serializeFieldPathFromString(_key)));
    out.setField(DocumentSourceInternalGeoNearDistance::kDistanceFieldFieldName,
                 Value(opts.serializeFieldPath(_distanceField)));
    out.setField(DocumentSourceInternalGeoNearDistance::kDistanceMultiplierFieldName,
                 opts.serializeLiteral(_distanceMultiplier));

    return Value(DOC(getSourceName() << out.freeze()));
}

}  // namespace mongo
