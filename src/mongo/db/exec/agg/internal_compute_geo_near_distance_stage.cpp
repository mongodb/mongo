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

#include "mongo/db/exec/agg/internal_compute_geo_near_distance_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/pipeline/document_source_internal_compute_geo_near_distance.h"

#include <memory>
#include <vector>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalGeoNearDistanceToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource = dynamic_cast<DocumentSourceInternalGeoNearDistance*>(source.get());

    tassert(10565600, "expected 'DocumentSourceInternalGeoNearDistance' type", documentSource);

    return make_intrusive<exec::agg::InternalGeoNearDistanceStage>(
        documentSource->kStageName,
        documentSource->getExpCtx(),
        documentSource->_key,
        documentSource->_centroid->clone(),
        documentSource->_coords,
        documentSource->_distanceField,
        documentSource->_distanceMultiplier);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(internalGeoNearDistanceStage,
                           DocumentSourceInternalGeoNearDistance::id,
                           documentSourceInternalGeoNearDistanceToStageFn);

InternalGeoNearDistanceStage::InternalGeoNearDistanceStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    std::string key,
    std::unique_ptr<PointWithCRS> centroid,
    const BSONObj& coords,
    FieldPath distanceField,
    double distanceMultiplier)
    : Stage(stageName, pExpCtx),
      _key(std::move(key)),
      _centroid(std::move(centroid)),
      _coords(coords),
      _distanceField(std::move(distanceField)),
      _distanceMultiplier(distanceMultiplier) {}

GetNextResult InternalGeoNearDistanceStage::doGetNext() {
    auto next = pSource->getNext();

    if (next.isAdvanced()) {
        // Extract all the geometries out of this document for the near query
        std::vector<std::unique_ptr<StoredGeometry>> geometries;
        StoredGeometry::extractGeometries(next.getDocument().toBson(), _key, &geometries, false);

        // Compute the minimum distance of all the geometries in the document
        double minDistance = -1;
        for (auto it = geometries.begin(); it != geometries.end(); ++it) {
            StoredGeometry& stored = **it;

            if (!stored.geometry.supportsProject(_centroid->crs)) {
                continue;
            }
            stored.geometry.projectInto(_centroid->crs);

            double nextDistance = stored.geometry.minDistance(*_centroid);

            if (minDistance < 0 || nextDistance < minDistance) {
                minDistance = nextDistance;
            }
        }
        minDistance *= _distanceMultiplier;

        MutableDocument doc(next.releaseDocument());
        doc.setNestedField(_distanceField, Value{minDistance});

        return doc.freeze();
    }

    return next;
}

}  // namespace exec::agg
}  // namespace mongo
