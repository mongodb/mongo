// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/internal_compute_geo_near_distance_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/pipeline/document_source_internal_compute_geo_near_distance.h"

#include <memory>
#include <string_view>
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
    std::string_view stageName,
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
