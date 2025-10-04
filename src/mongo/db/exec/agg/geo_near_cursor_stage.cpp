/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/geo_near_cursor_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/geo_near_cursor_stage.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_geo_near_cursor.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceGeoNearCursorToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource = boost::dynamic_pointer_cast<const DocumentSourceGeoNearCursor>(source);

    tassert(10422501, "expected 'DocumentSourceGeoNearCursor' type", documentSource);

    return make_intrusive<exec::agg::GeoNearCursorStage>(documentSource->kStageName,
                                                         documentSource->_catalogResourceHandle,
                                                         documentSource->getExpCtx(),
                                                         documentSource->_cursorType,
                                                         documentSource->_resumeTrackingType,
                                                         documentSource->_distanceField,
                                                         documentSource->_locationField,
                                                         documentSource->_distanceMultiplier,
                                                         documentSource->_sharedState);
}

namespace exec::agg {


REGISTER_AGG_STAGE_MAPPING(geoNearCursorStage,
                           DocumentSourceGeoNearCursor::id,
                           documentSourceGeoNearCursorToStageFn);

GeoNearCursorStage::GeoNearCursorStage(
    StringData sourceName,
    const boost::intrusive_ptr<CatalogResourceHandle>& catalogResourceHandle,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    CursorType cursorType,
    ResumeTrackingType resumeTrackingType,
    boost::optional<FieldPath> distanceField,
    boost::optional<FieldPath> locationField,
    double distanceMultiplier,
    const std::shared_ptr<CursorSharedState>& cursorSharedState)
    : CursorStage(sourceName,
                  catalogResourceHandle,
                  expCtx,
                  cursorType,
                  resumeTrackingType,
                  std::move(cursorSharedState)),
      _distanceField(std::move(distanceField)),
      _locationField(std::move(locationField)),
      _distanceMultiplier(distanceMultiplier) {}

Document GeoNearCursorStage::transformDoc(Document&& objInput) const {
    MutableDocument output(std::move(objInput));

    // Scale the distance by the requested factor.
    tassert(9911902,
            str::stream()
                << "Query returned a document that is unexpectedly missing the geoNear distance: "
                << output.peek().toString(),
            output.peek().metadata().hasGeoNearDistance());
    const auto distance = output.peek().metadata().getGeoNearDistance() * _distanceMultiplier;

    if (_distanceField) {
        output.setNestedField(*_distanceField, Value(distance));
    }
    if (_locationField) {
        tassert(9911903,
                str::stream()
                    << "Query returned a document that is unexpectedly missing the geoNear point: "
                    << output.peek().toString(),
                output.peek().metadata().hasGeoNearPoint());
        output.setNestedField(*_locationField, output.peek().metadata().getGeoNearPoint());
    }

    // Always set the sort key. Sometimes it will be needed in a sharded cluster to perform a merge
    // sort. Other times it will be needed by $rankFusion. It is not expensive, so just make it
    // unconditionally available.
    const bool isSingleElementKey = true;
    output.metadata().setSortKey(Value(distance), isSingleElementKey);

    return output.freeze();
}
}  // namespace exec::agg
}  // namespace mongo
