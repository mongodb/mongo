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


#include "mongo/db/exec/classic/geo_near.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <s2regionintersection.h>  // For s2 search


// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/classic/fetch.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/expression_geo_index_knobs_gen.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/expression_geo_index_mapping.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"

#include <r1interval.h>
#include <s1angle.h>
#include <s2.h>
#include <s2cap.h>
#include <s2cell.h>
#include <s2cellid.h>
#include <s2cellunion.h>
#include <s2latlng.h>
#include <s2region.h>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

using std::abs;
using std::unique_ptr;

//
// Shared GeoNear search functionality
//

static const double kCircOfEarthInMeters = 2 * M_PI * kRadiusOfEarthInMeters;
static const double kMaxEarthDistanceInMeters = kCircOfEarthInMeters / 2;
static const double kMetersPerDegreeAtEquator = kCircOfEarthInMeters / 360;

static double computeGeoNearDistance(const GeoNearParams& nearParams, WorkingSetMember* member) {
    //
    // Generic GeoNear distance computation
    // Distances are computed by projecting the stored geometry into the query CRS, and
    // computing distance in that CRS.
    //

    // Must have an object in order to get geometry out of it.
    tassert(9911912, "", member->hasObj());

    CRS queryCRS = nearParams.nearQuery->centroid->crs;

    // Extract all the geometries out of this document for the near query. Note that the NearStage
    // stage can only be run with an existing index. Therefore, it is always safe to skip geometry
    // validation.
    std::vector<std::unique_ptr<StoredGeometry>> geometries;
    StoredGeometry::extractGeometries(
        member->doc.value().toBson(), nearParams.nearQuery->field, &geometries, true);

    // Compute the minimum distance of all the geometries in the document
    double minDistance = -1;
    Value minDistanceMetadata;
    for (auto it = geometries.begin(); it != geometries.end(); ++it) {
        StoredGeometry& stored = **it;

        // NOTE: A stored document with STRICT_SPHERE CRS is treated as a malformed document
        // and ignored. Since GeoNear requires an index, there's no stored STRICT_SPHERE shape.
        // So we don't check it here.

        // NOTE: For now, we're sure that if we get this far in the query we'll have an
        // appropriate index which validates the type of geometry we're pulling back here.
        // TODO: It may make sense to change our semantics and, by default, only return
        // shapes in the same CRS from $geoNear.
        if (!stored.geometry.supportsProject(queryCRS))
            continue;
        stored.geometry.projectInto(queryCRS);

        double nextDistance = stored.geometry.minDistance(*nearParams.nearQuery->centroid);

        if (minDistance < 0 || nextDistance < minDistance) {
            minDistance = nextDistance;
            minDistanceMetadata = Value{stored.element};
        }
    }

    if (minDistance < 0) {
        // No distance to report
        return -1;
    }

    if (nearParams.addDistMeta) {
        if (nearParams.nearQuery->unitsAreRadians) {
            // Hack for nearSphere
            // TODO: Remove nearSphere?
            tassert(9911927, "", SPHERE == queryCRS);
            member->metadata().setGeoNearDistance(minDistance / kRadiusOfEarthInMeters);
        } else {
            member->metadata().setGeoNearDistance(minDistance);
        }
    }

    if (nearParams.addPointMeta) {
        member->metadata().setGeoNearPoint(minDistanceMetadata);
    }

    return minDistance;
}

static R2Annulus geoNearDistanceBounds(const GeoNearExpression& query) {
    const CRS queryCRS = query.centroid->crs;

    if (FLAT == queryCRS) {
        return R2Annulus(query.centroid->oldPoint, query.minDistance, query.maxDistance);
    }

    tassert(9911913, "", SPHERE == queryCRS);

    // TODO: Tighten this up a bit by making a CRS for "sphere with radians"
    double minDistance = query.minDistance;
    double maxDistance = query.maxDistance;

    if (query.unitsAreRadians) {
        // Our input bounds are in radians, convert to meters since the query CRS is actually
        // SPHERE.  We'll convert back to radians on outputting distances.
        minDistance *= kRadiusOfEarthInMeters;
        maxDistance *= kRadiusOfEarthInMeters;
    }

    // GOTCHA: oldPoint is a misnomer - it is the original point data and is in the correct
    // CRS.  We must not try to derive the original point from the spherical S2Point generated
    // as an optimization - the mapping is not 1->1 - [-180, 0] and [180, 0] map to the same
    // place.
    // TODO: Wrapping behavior should not depend on the index, which would make $near code
    // insensitive to which direction we explore the index in.
    return R2Annulus(query.centroid->oldPoint,
                     min(minDistance, kMaxEarthDistanceInMeters),
                     min(maxDistance, kMaxEarthDistanceInMeters));
}

//
// GeoNear2DStage
//

static R2Annulus twoDDistanceBounds(const GeoNearParams& nearParams,
                                    const IndexDescriptor* twoDIndex) {
    R2Annulus fullBounds = geoNearDistanceBounds(*nearParams.nearQuery);
    const CRS queryCRS = nearParams.nearQuery->centroid->crs;

    if (FLAT == queryCRS) {
        // Reset the full bounds based on our index bounds.
        // The index status should always be valid.
        auto result = invariantStatusOK(GeoHashConverter::createFromDoc(twoDIndex->infoObj()));

        // The biggest distance possible in this indexed collection is the diagonal of the
        // square indexed region.
        const GeoHashConverter::Parameters& hashParams = result->getParams();
        const double sqrt2Approx = 1.5;
        const double diagonalDist = sqrt2Approx * (hashParams.max - hashParams.min);

        fullBounds = R2Annulus(
            fullBounds.center(), fullBounds.getInner(), min(fullBounds.getOuter(), diagonalDist));
    } else {
        // Spherical queries have upper bounds set by the earth - no-op
        // TODO: Wrapping errors would creep in here if nearSphere wasn't defined to not wrap
        tassert(9911914, "", SPHERE == queryCRS);
        tassert(9911915, "", !nearParams.nearQuery->isWrappingQuery);
    }

    return fullBounds;
}

GeoNear2DStage::DensityEstimator::DensityEstimator(const CollectionAcquisition* collection,
                                                   PlanStage::Children* children,
                                                   BSONObj infoObj,
                                                   const GeoNearParams* nearParams,
                                                   const R2Annulus& fullBounds)
    : _collection(collection),
      _children(children),
      _nearParams(nearParams),
      _fullBounds(fullBounds),
      _currentLevel(0) {
    // The index status should always be valid.
    auto result = invariantStatusOK(GeoHashConverter::createFromDoc(infoObj));

    _converter = std::move(result);
    _centroidCell = _converter->hash(_nearParams->nearQuery->centroid->oldPoint);

    // Since appendVertexNeighbors(level, output) requires level < hash.getBits(),
    // we have to start to find documents at most GeoHash::kMaxBits - 1. Thus the finest
    // search area is 16 * finest cell area at GeoHash::kMaxBits.
    _currentLevel = std::max(0, _converter->getParams().bits - 1);
}

// Initialize the internal states
void GeoNear2DStage::DensityEstimator::buildIndexScan(ExpressionContext* expCtx,
                                                      WorkingSet* workingSet,
                                                      const IndexDescriptor* twoDIndex) {
    // Scan bounds on 2D indexes are only over the 2D field - other bounds aren't applicable.
    // This is handled in query planning.
    IndexScanParams scanParams(
        expCtx->getOperationContext(), _collection->getCollectionPtr(), twoDIndex);
    scanParams.bounds = _nearParams->baseBounds;

    // The "2d" field is always the first in the index
    const string twoDFieldName = _nearParams->nearQuery->field;
    const int twoDFieldPosition = 0;

    // Construct index intervals used by this stage
    OrderedIntervalList oil;
    oil.name = scanParams.bounds.fields[twoDFieldPosition].name;

    vector<GeoHash> neighbors;
    // Return the neighbors of closest vertex to this cell at the given level.
    _centroidCell.appendVertexNeighbors(_currentLevel, &neighbors);
    std::sort(neighbors.begin(), neighbors.end());

    for (vector<GeoHash>::const_iterator it = neighbors.begin(); it != neighbors.end(); it++) {
        mongo::BSONObjBuilder builder;
        it->appendHashMin(&builder, "");
        it->appendHashMax(&builder, "");
        oil.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
            builder.obj(), BoundInclusion::kIncludeBothStartAndEndKeys));
    }

    tassert(9911916, "", oil.isValidFor(1));

    // Intersect the $near bounds we just generated into the bounds we have for anything else
    // in the scan (i.e. $within)
    IndexBoundsBuilder::intersectize(oil, &scanParams.bounds.fields[twoDFieldPosition]);

    tassert(9911917, "", !_indexScan);
    _indexScan = new IndexScan(expCtx, *_collection, scanParams, workingSet, nullptr);
    _children->emplace_back(_indexScan);
}

// Return IS_EOF is we find a document in it's ancestor cells and set estimated distance
// from the nearest document.
PlanStage::StageState GeoNear2DStage::DensityEstimator::work(ExpressionContext* expCtx,
                                                             WorkingSet* workingSet,
                                                             const IndexDescriptor* twoDIndex,
                                                             WorkingSetID* out,
                                                             double* estimatedDistance) {
    if (!_indexScan) {
        // Setup index scan stage for current level.
        buildIndexScan(expCtx, workingSet, twoDIndex);
    }

    WorkingSetID workingSetID;
    PlanStage::StageState state = _indexScan->work(&workingSetID);

    if (state == PlanStage::IS_EOF) {
        // We ran through the neighbors but found nothing.
        //
        // Before going to the next-coarsest level, check whether our search area contains the
        // entire search annulus, since we don't want to spend time doing density estimation over
        // areas that are much larger than the requested $maxDistance.
        //
        // The search area consists of four cells with side length S. Within its cell, the closest
        // vertex to the search point must be the vertex shared with the other three cells. If the
        // search point lies in the upper left cell, this means that it must lie in the lower right
        // quadrant of that cell. Furthermore, this lower-right quadrant has a side-length of S/2.
        //
        //   +-----------+-----------+
        //   |           |           |
        //   |       S/2 |           |
        //   +     +-----+           |
        //   |     | o   |           |
        //   |     |     |           |
        //   +-----+-----+-----------+
        //   |           |           |
        //   |           |           |
        //   |           |           |
        //   |           |           |
        //   |           |           |
        //   +-----------+-----------+
        //         S
        //
        // As long as the outer radius of the search annulus is less than S/2, it must be entirely
        // contained within these four cells.
        if (_fullBounds.getOuter() < (0.5 * _converter->sizeEdge(_currentLevel))) {
            // We're covering the entire search annulus. Return EOF to indicate we're done.
            *estimatedDistance = 0.5 * _converter->sizeEdge(_currentLevel);
            return PlanStage::IS_EOF;
        }

        if (_currentLevel > 0u) {
            // Advance to the next level and search again.
            _currentLevel--;
            // Reset index scan for the next level.
            tassert(9911918, "", _children->back().get() == _indexScan);
            _indexScan = nullptr;
            _children->pop_back();
            return PlanStage::NEED_TIME;
        }

        // We are already at the top level.
        *estimatedDistance = _converter->sizeEdge(_currentLevel);
        return PlanStage::IS_EOF;
    } else if (state == PlanStage::ADVANCED) {
        // Found a document at current level.
        *estimatedDistance = _converter->sizeEdge(_currentLevel);
        // Clean up working set.
        workingSet->free(workingSetID);
        return PlanStage::IS_EOF;
    } else if (state == PlanStage::NEED_YIELD) {
        *out = workingSetID;
    }

    // Propagate NEED_TIME or errors
    return state;
}

PlanStage::StageState GeoNear2DStage::initialize(OperationContext* opCtx,
                                                 WorkingSet* workingSet,
                                                 WorkingSetID* out) {
    if (!_densityEstimator) {
        _densityEstimator.reset(new DensityEstimator(
            &collection(), &_children, indexDescriptor()->infoObj(), &_nearParams, _fullBounds));
    }

    double estimatedDistance;
    PlanStage::StageState state =
        _densityEstimator->work(expCtx(), workingSet, indexDescriptor(), out, &estimatedDistance);

    if (state == PlanStage::IS_EOF) {
        // 2d index only works with legacy points as centroid. $nearSphere will project
        // the point into SPHERE CRS and calculate distance based on that.
        // STRICT_SPHERE is impossible here, as GeoJSON centroid is not allowed for 2d index.

        // Estimator finished its work, we need to finish initialization too.
        if (SPHERE == _nearParams.nearQuery->centroid->crs) {
            // Estimated distance is in degrees, convert it to meters multiplied by 3.
            _boundsIncrement = (estimatedDistance * kRadiusOfEarthInMeters * 3) * (M_PI / 180);
            // Limit boundsIncrement to ~20KM, so that the first circle won't be too aggressive.
            _boundsIncrement = std::min(_boundsIncrement, kMaxEarthDistanceInMeters / 1000.0);
        } else {
            // We expand the radius by 3 times to give a reasonable starting search area.
            // Assume points are distributed evenly. X is the edge size of cells at whose
            // level we found a document in 4 neighbors. Thus the closest point is at least
            // X/2 far from the centroid. The distance between two points is at least X.
            // The area of Pi * (3X)^2 ~= 28 * X^2 will cover dozens of points at most.
            // We'll explore the space with exponentially increasing radius if this guess is
            // too small, so starting from a conservative initial radius doesn't hurt.

            _boundsIncrement = 3 * estimatedDistance;
        }
        tassert(9911919, "", _boundsIncrement > 0.0);

        // Clean up
        _densityEstimator.reset(nullptr);
    }

    return state;
}

static const string kTwoDIndexNearStage("GEO_NEAR_2D");

GeoNear2DStage::GeoNear2DStage(const GeoNearParams& nearParams,
                               ExpressionContext* expCtx,
                               WorkingSet* workingSet,
                               CollectionAcquisition collection,
                               const IndexDescriptor* twoDIndex)
    : NearStage(expCtx,
                kTwoDIndexNearStage.c_str(),
                STAGE_GEO_NEAR_2D,
                workingSet,
                collection,
                twoDIndex),
      _nearParams(nearParams),
      _fullBounds(twoDDistanceBounds(nearParams, twoDIndex)),
      _currBounds(_fullBounds.center(), -1, _fullBounds.getInner()),
      _boundsIncrement(0.0) {
    _specificStats.keyPattern = twoDIndex->keyPattern();
    _specificStats.indexName = twoDIndex->indexName();
    _specificStats.indexVersion = static_cast<int>(twoDIndex->version());
}


namespace {
// Helper class to maintain ownership of a match expression alongside an index scan
class FetchStageWithMatch final : public FetchStage {
public:
    FetchStageWithMatch(ExpressionContext* expCtx,
                        WorkingSet* ws,
                        std::unique_ptr<PlanStage> child,
                        MatchExpression* filter,
                        CollectionAcquisition collection)
        : FetchStage(expCtx, ws, std::move(child), filter, collection), _matcher(filter) {}

private:
    // Owns matcher
    const unique_ptr<MatchExpression> _matcher;
};
}  // namespace

static double min2DBoundsIncrement(const GeoNearExpression& query,
                                   const IndexDescriptor* twoDIndex) {
    // The index status should always be valid.
    auto result = invariantStatusOK(GeoHashConverter::createFromDoc(twoDIndex->infoObj()));

    // The hasher error is the diagonal of a 2D hash region - it's generally not helpful
    // to change region size such that a search radius is smaller than the 2D hash region
    // max radius.  This is slightly conservative for now (box diagonal vs circle radius).
    const double minBoundsIncrement = result->getError() / 2;

    const CRS queryCRS = query.centroid->crs;
    if (FLAT == queryCRS)
        return minBoundsIncrement;

    tassert(9911920, "", SPHERE == queryCRS);

    // If this is a spherical query, units are in meters - this is just a heuristic
    return minBoundsIncrement * kMetersPerDegreeAtEquator;
}

static R2Annulus projectBoundsToTwoDDegrees(R2Annulus sphereBounds) {
    const double outerDegrees = rad2deg(sphereBounds.getOuter() / kRadiusOfEarthInMeters);
    const double innerDegrees = rad2deg(sphereBounds.getInner() / kRadiusOfEarthInMeters);
    const double maxErrorDegrees = computeXScanDistance(sphereBounds.center().y, outerDegrees);

    return R2Annulus(sphereBounds.center(),
                     max(0.0, innerDegrees - maxErrorDegrees),
                     outerDegrees + maxErrorDegrees);
}

std::unique_ptr<NearStage::CoveredInterval> GeoNear2DStage::nextInterval(OperationContext* opCtx,
                                                                         WorkingSet* workingSet) {
    // The search is finished if we searched at least once and all the way to the edge
    if (_currBounds.getInner() >= 0 && _currBounds.getOuter() == _fullBounds.getOuter()) {
        return nullptr;
    }

    //
    // Setup the next interval
    //

    if (!_specificStats.intervalStats.empty()) {
        const IntervalStats& lastIntervalStats = _specificStats.intervalStats.back();

        // TODO: Generally we want small numbers of results fast, then larger numbers later
        if (lastIntervalStats.numResultsReturned < 300)
            _boundsIncrement *= 2;
        else if (lastIntervalStats.numResultsReturned > 600)
            _boundsIncrement /= 2;
    }

    _boundsIncrement =
        max(_boundsIncrement, min2DBoundsIncrement(*_nearParams.nearQuery, indexDescriptor()));

    R2Annulus nextBounds(_currBounds.center(),
                         _currBounds.getOuter(),
                         min(_currBounds.getOuter() + _boundsIncrement, _fullBounds.getOuter()));

    const bool isLastInterval = (nextBounds.getOuter() == _fullBounds.getOuter());
    _currBounds = nextBounds;

    //
    // Get a covering region for this interval
    //

    const CRS queryCRS = _nearParams.nearQuery->centroid->crs;

    unique_ptr<R2Region> coverRegion;

    if (FLAT == queryCRS) {
        // NOTE: Due to floating point math issues, FLAT searches of a 2D index need to treat
        // containment and distance separately.
        // Ex: (distance) 54.001 - 54 > 0.001, but (containment) 54 + 0.001 <= 54.001
        // The idea is that a $near search with bounds is really a $within search, sorted by
        // distance.  We attach a custom $within : annulus matcher to do the $within search,
        // and adjust max/min bounds slightly since, as above, containment does not mean the
        // distance calculation won't slightly overflow the boundary.
        //
        // The code below adjusts:
        // 1) Overall min/max bounds of the generated distance intervals to be more inclusive
        // 2) Bounds of the interval covering to be more inclusive
        // ... and later on we add the custom $within : annulus matcher.
        //
        // IMPORTANT: The *internal* interval distance bounds are *exact thresholds* - these
        // should not be adjusted.
        // TODO: Maybe integrate annuluses as a standard shape, and literally transform $near
        // internally into a $within query with $near just as sort.

        // Compute the maximum axis-aligned distance error
        const double epsilon = std::numeric_limits<double>::epsilon() *
            (max(abs(_fullBounds.center().x), abs(_fullBounds.center().y)) +
             _fullBounds.getOuter());

        if (nextBounds.getInner() > 0 && nextBounds.getInner() == _fullBounds.getInner()) {
            nextBounds = R2Annulus(nextBounds.center(),
                                   max(0.0, nextBounds.getInner() - epsilon),
                                   nextBounds.getOuter());
        }

        if (nextBounds.getOuter() > 0 && nextBounds.getOuter() == _fullBounds.getOuter()) {
            // We're at the max bound of the search, adjust interval maximum
            nextBounds = R2Annulus(
                nextBounds.center(), nextBounds.getInner(), nextBounds.getOuter() + epsilon);
        }

        // *Always* adjust the covering bounds to be more inclusive
        coverRegion.reset(new R2Annulus(nextBounds.center(),
                                        max(0.0, nextBounds.getInner() - epsilon),
                                        nextBounds.getOuter() + epsilon));
    } else {
        tassert(9911921, "", SPHERE == queryCRS);
        // TODO: As above, make this consistent with $within : $centerSphere

        // Our intervals aren't in the same CRS as our index, so we need to adjust them
        coverRegion.reset(new R2Annulus(projectBoundsToTwoDDegrees(nextBounds)));
    }

    //
    // Setup the stages for this interval
    //

    // Scan bounds on 2D indexes are only over the 2D field - other bounds aren't applicable.
    // This is handled in query planning.
    IndexScanParams scanParams(opCtx, collectionPtr(), indexDescriptor());

    // This does force us to do our own deduping of results.
    scanParams.bounds = _nearParams.baseBounds;

    // The "2d" field is always the first in the index
    const string twoDFieldName = _nearParams.nearQuery->field;
    const int twoDFieldPosition = 0;

    std::vector<GeoHash> unorderedCovering = ExpressionMapping::get2dCovering(
        *coverRegion, indexDescriptor()->infoObj(), gInternalGeoNearQuery2DMaxCoveringCells.load());

    // Make sure the same index key isn't visited twice
    R2CellUnion diffUnion;
    diffUnion.init(unorderedCovering);
    diffUnion.getDifference(_scannedCells);
    // After taking the difference, there may be cells in the covering that don't intersect
    // with the annulus.
    diffUnion.detach(&unorderedCovering);

    // Add the cells in this covering to the _scannedCells union
    _scannedCells.add(unorderedCovering);

    OrderedIntervalList coveredIntervals;
    coveredIntervals.name = scanParams.bounds.fields[twoDFieldPosition].name;
    ExpressionMapping::GeoHashsToIntervalsWithParents(unorderedCovering, &coveredIntervals);

    // Intersect the $near bounds we just generated into the bounds we have for anything else
    // in the scan (i.e. $within)
    IndexBoundsBuilder::intersectize(coveredIntervals,
                                     &scanParams.bounds.fields[twoDFieldPosition]);

    // These parameters are stored by the index, and so must be ok
    invariantStatusOK(GeoHashConverter::createFromDoc(indexDescriptor()->infoObj()));

    // 2D indexes support covered search over additional fields they contain
    auto scan = std::make_unique<IndexScan>(
        expCtx(), collection(), scanParams, workingSet, _nearParams.filter);

    MatchExpression* docMatcher = nullptr;

    // FLAT searches need to add an additional annulus $within matcher, see above
    // TODO: Find out if this matcher is actually needed
    if (FLAT == queryCRS) {
        docMatcher = new TwoDPtInAnnulusExpression(_fullBounds, StringData(twoDFieldName));
    }

    // FetchStage owns index scan
    _children.emplace_back(std::make_unique<FetchStageWithMatch>(
        expCtx(), workingSet, std::move(scan), docMatcher, collection()));

    return std::make_unique<CoveredInterval>(
        _children.back().get(), nextBounds.getInner(), nextBounds.getOuter(), isLastInterval);
}

double GeoNear2DStage::computeDistance(WorkingSetMember* member) {
    return computeGeoNearDistance(_nearParams, member);
}

//
// GeoNear2DSphereStage
//

static int getFieldPosition(const IndexDescriptor* index, const string& fieldName) {
    int fieldPosition = 0;

    BSONObjIterator specIt(index->keyPattern());
    while (specIt.more()) {
        if (specIt.next().fieldName() == fieldName) {
            break;
        }
        ++fieldPosition;
    }

    if (fieldPosition == index->keyPattern().nFields())
        return -1;

    return fieldPosition;
}

static const string kS2IndexNearStage("GEO_NEAR_2DSPHERE");

GeoNear2DSphereStage::GeoNear2DSphereStage(const GeoNearParams& nearParams,
                                           ExpressionContext* expCtx,
                                           WorkingSet* workingSet,
                                           CollectionAcquisition collection,
                                           const IndexDescriptor* s2Index)
    : NearStage(expCtx,
                kS2IndexNearStage.c_str(),
                STAGE_GEO_NEAR_2DSPHERE,
                workingSet,
                collection,
                s2Index),
      _nearParams(nearParams),
      _fullBounds(geoNearDistanceBounds(*nearParams.nearQuery)),
      _currBounds(_fullBounds.center(), -1, _fullBounds.getInner()),
      _boundsIncrement(0.0) {
    _specificStats.keyPattern = s2Index->keyPattern();
    _specificStats.indexName = s2Index->indexName();
    _specificStats.indexVersion = static_cast<int>(s2Index->version());

    // initialize2dsphereParams() does not require the collator during the GEO_NEAR_2DSPHERE stage.
    // It only requires the collator for index key generation. For query execution,
    // _nearParams.baseBounds should have collator-generated comparison keys in place of raw
    // strings, and _nearParams.filter should have the collator.
    const CollatorInterface* collator = nullptr;
    index2dsphere::initialize2dsphereParams(s2Index->infoObj(), collator, &_indexParams);
}

namespace {

S2Region* buildS2Region(const R2Annulus& sphereBounds) {
    // Internal bounds come in SPHERE CRS units
    // i.e. center is lon/lat, inner/outer are in meters
    S2LatLng latLng = S2LatLng::FromDegrees(sphereBounds.center().y, sphereBounds.center().x);

    vector<S2Region*> regions;

    const double inner = sphereBounds.getInner();
    const double outer = sphereBounds.getOuter();

    if (inner > 0) {
        // TODO: Currently a workaround to fix occasional floating point errors
        // in S2, where sometimes points near the axis will not be returned
        // if inner == 0
        S2Cap innerCap = S2Cap::FromAxisAngle(latLng.ToPoint(),
                                              S1Angle::Radians(inner / kRadiusOfEarthInMeters));
        innerCap = innerCap.Complement();
        regions.push_back(new S2Cap(innerCap));
    }

    // 'kEpsilon' is about 9 times the double-precision roundoff relative error.
    const double kEpsilon = 1e-15;

    // We only need to max bound if this is not a full search of the Earth
    // Using the constant here is important since we use the min of kMaxEarthDistance
    // and the actual bounds passed in to set up the search area.
    if ((outer * (1 + kEpsilon)) < kMaxEarthDistanceInMeters) {
        // SERVER-52953: The cell covering returned by S2 may have a matching point along its
        // boundary. In certain cases, this boundary point is not contained within the covering,
        // which means that this search will not match said point. As such, we avoid this issue by
        // finding a covering for the region expanded over a very small radius because this covering
        // is guaranteed to contain the boundary point.
        auto angle = S1Angle::Radians((outer * (1 + kEpsilon)) / kRadiusOfEarthInMeters);
        S2Cap outerCap = S2Cap::FromAxisAngle(latLng.ToPoint(), angle);

        // If 'outer' is sufficiently small, the computation of the S2Cap's height from 'angle' may
        // underflow, resulting in a height less than 'kEpsilon' and an empty cap. As such, we
        // guarantee that 'outerCap' has a height of at least 'kEpsilon'.
        if (outerCap.height() < kEpsilon) {
            outerCap = S2Cap::FromAxisHeight(latLng.ToPoint(), kEpsilon);
        }
        regions.push_back(new S2Cap(outerCap));
    }

    // if annulus is entire world, return a full cap
    if (regions.empty()) {
        regions.push_back(new S2Cap(S2Cap::Full()));
    }

    // Takes ownership of caps
    return new S2RegionIntersection(&regions);
}
}  // namespace

GeoNear2DSphereStage::DensityEstimator::DensityEstimator(const CollectionAcquisition* collection,
                                                         PlanStage::Children* children,
                                                         const GeoNearParams* nearParams,
                                                         const S2IndexingParams& indexParams,
                                                         const R2Annulus& fullBounds)
    : _collection(collection),
      _children(children),
      _nearParams(nearParams),
      _indexParams(indexParams),
      _fullBounds(fullBounds),
      _currentLevel(0) {
    // cellId.AppendVertexNeighbors(level, output) requires level < finest,
    // so we use the minimum of max_level - 1 and the user specified finest
    int level = std::min(S2::kMaxCellLevel - 1, gInternalQueryS2GeoFinestLevel.load());
    _currentLevel = std::max(0, level);
}

// Setup the index scan stage for neighbors at this level.
void GeoNear2DSphereStage::DensityEstimator::buildIndexScan(ExpressionContext* expCtx,
                                                            WorkingSet* workingSet,
                                                            const IndexDescriptor* s2Index) {
    IndexScanParams scanParams(
        expCtx->getOperationContext(), _collection->getCollectionPtr(), s2Index);
    scanParams.bounds = _nearParams->baseBounds;

    // Because the planner doesn't yet set up 2D index bounds, do it ourselves here
    const string s2Field = _nearParams->nearQuery->field;
    const int s2FieldPosition = getFieldPosition(s2Index, s2Field);
    fassert(28677, s2FieldPosition >= 0);
    OrderedIntervalList* coveredIntervals = &scanParams.bounds.fields[s2FieldPosition];
    coveredIntervals->intervals.clear();

    // Find 4 neighbors (3 neighbors at face vertex) at current level.
    const S2CellId& centerId = _nearParams->nearQuery->centroid->cell.id();
    vector<S2CellId> neighbors;

    // The search area expands 4X each time.
    // Return the neighbors of closest vertex to this cell at the given level.
    tassert(9911922, "", _currentLevel < centerId.level());
    centerId.AppendVertexNeighbors(_currentLevel, &neighbors);

    ExpressionMapping::S2CellIdsToIntervals(neighbors, _indexParams.indexVersion, coveredIntervals);

    // Index scan
    tassert(9911923, "", !_indexScan);
    _indexScan = new IndexScan(expCtx, *_collection, scanParams, workingSet, nullptr);
    _children->emplace_back(_indexScan);
}

PlanStage::StageState GeoNear2DSphereStage::DensityEstimator::work(ExpressionContext* expCtx,
                                                                   WorkingSet* workingSet,
                                                                   const IndexDescriptor* s2Index,
                                                                   WorkingSetID* out,
                                                                   double* estimatedDistance) {
    if (!_indexScan) {
        // Setup index scan stage for current level.
        buildIndexScan(expCtx, workingSet, s2Index);
    }

    WorkingSetID workingSetID;
    PlanStage::StageState state = _indexScan->work(&workingSetID);

    if (state == PlanStage::IS_EOF) {
        // We ran through the neighbors but found nothing.
        //
        // Before going to the next-coarsest level, check whether our search area contains the
        // entire search annulus, since we don't want to spend time doing density estimation over
        // areas that are much larger than the requested $maxDistance.
        //
        // The search area consists of four cells at level L. Within its cell, the closest vertex to
        // the search point must be the vertex shared with the other three cells. If the search
        // point lies in the upper left cell, this means that it must lie in the lower right
        // sub-cell at level L+1.
        //
        //   +-----------+-----------+
        //   |           |           |
        //   |        S  |           |
        //   +     +-----+           |
        //   |     | o   |           |
        //   |     |     |           |
        //   +-----+-----+-----------+
        //   |           |           |
        //   |           |           |
        //   |           |           |
        //   |           |           |
        //   |           |           |
        //   +-----------+-----------+
        //
        // In the diagram above, S is the width of the cell at level L+1. We can determine a lower
        // bound for the width any cell at this level, i.e. S > minWidth(L+1). As long as the outer
        // radius of the search annulus is less than minWidth(L+1), it must be entirely contained
        // within these four level L cells.
        if (_fullBounds.getOuter() <
            (S2::kMinWidth.GetValue(_currentLevel + 1) * kRadiusOfEarthInMeters)) {
            // We're covering the entire search annulus. Return EOF to indicate we're done.
            *estimatedDistance = S2::kMinWidth.GetValue(_currentLevel + 1) * kRadiusOfEarthInMeters;
            return PlanStage::IS_EOF;
        }

        if (_currentLevel > 0) {
            // Advance to the next level and search again.
            _currentLevel--;
            // Reset index scan for the next level.
            tassert(9911924, "", _children->back().get() == _indexScan);
            _indexScan = nullptr;
            _children->pop_back();
            return PlanStage::NEED_TIME;
        }

        // We are already at the top level.
        *estimatedDistance = S2::kAvgEdge.GetValue(_currentLevel) * kRadiusOfEarthInMeters;
        return PlanStage::IS_EOF;
    } else if (state == PlanStage::ADVANCED) {
        // We found something!
        *estimatedDistance = S2::kAvgEdge.GetValue(_currentLevel) * kRadiusOfEarthInMeters;
        // Clean up working set.
        workingSet->free(workingSetID);
        return PlanStage::IS_EOF;
    } else if (state == PlanStage::NEED_YIELD) {
        *out = workingSetID;
    }

    // Propagate NEED_TIME or errors
    return state;
}


PlanStage::StageState GeoNear2DSphereStage::initialize(OperationContext* opCtx,
                                                       WorkingSet* workingSet,
                                                       WorkingSetID* out) {
    if (!_densityEstimator) {
        _densityEstimator.reset(new DensityEstimator(
            &collection(), &_children, &_nearParams, _indexParams, _fullBounds));
    }

    double estimatedDistance;
    PlanStage::StageState state =
        _densityEstimator->work(expCtx(), workingSet, indexDescriptor(), out, &estimatedDistance);

    if (state == IS_EOF) {
        // We find a document in 4 neighbors at current level, but didn't at previous level.
        //
        // Assuming cell size at current level is d and data is even distributed, the distance
        // between two nearest points are at least d. The following circle with radius of 3 * d
        // covers PI * 9 * d^2, giving at most 30 documents.
        //
        // At the coarsest level, the search area is the whole earth.
        _boundsIncrement = 3 * estimatedDistance;
        tassert(9911925, "", _boundsIncrement > 0.0);

        // Clean up
        _densityEstimator.reset(nullptr);
    }

    return state;
}

std::unique_ptr<NearStage::CoveredInterval> GeoNear2DSphereStage::nextInterval(
    OperationContext* opCtx, WorkingSet* workingSet) {
    // The search is finished if we searched at least once and all the way to the edge
    if (_currBounds.getInner() >= 0 && _currBounds.getOuter() == _fullBounds.getOuter()) {
        return nullptr;
    }

    //
    // Setup the next interval
    //

    if (!_specificStats.intervalStats.empty()) {
        const IntervalStats& lastIntervalStats = _specificStats.intervalStats.back();

        // TODO: Generally we want small numbers of results fast, then larger numbers later
        if (lastIntervalStats.numResultsReturned < 300)
            _boundsIncrement *= 2;
        else if (lastIntervalStats.numResultsReturned > 600)
            _boundsIncrement /= 2;
    }

    tassert(9911926, "", _boundsIncrement > 0.0);

    R2Annulus nextBounds(_currBounds.center(),
                         _currBounds.getOuter(),
                         min(_currBounds.getOuter() + _boundsIncrement, _fullBounds.getOuter()));

    bool isLastInterval = (nextBounds.getOuter() == _fullBounds.getOuter());
    _currBounds = nextBounds;

    //
    // Setup the covering region and stages for this interval
    //

    IndexScanParams scanParams(opCtx, collectionPtr(), indexDescriptor());

    // This does force us to do our own deduping of results.
    scanParams.bounds = _nearParams.baseBounds;

    // Because the planner doesn't yet set up 2D index bounds, do it ourselves here
    const string s2Field = _nearParams.nearQuery->field;
    const int s2FieldPosition = getFieldPosition(indexDescriptor(), s2Field);
    fassert(28678, s2FieldPosition >= 0);
    scanParams.bounds.fields[s2FieldPosition].intervals.clear();
    std::unique_ptr<S2Region> region(buildS2Region(_currBounds));

    std::vector<S2CellId> cover = ExpressionMapping::get2dsphereCovering(*region);

    // Generate a covering that does not intersect with any previous coverings
    S2CellUnion coverUnion;
    coverUnion.InitSwap(&cover);
    tassert(9911910, "", cover.empty());
    S2CellUnion diffUnion;
    diffUnion.GetDifference(&coverUnion, &_scannedCells);
    for (const auto& cellId : diffUnion.cell_ids()) {
        if (region->MayIntersect(S2Cell(cellId))) {
            cover.push_back(cellId);
        }
    }

    // Add the cells in this covering to the _scannedCells union
    _scannedCells.Add(cover);

    OrderedIntervalList* coveredIntervals = &scanParams.bounds.fields[s2FieldPosition];
    ExpressionMapping::S2CellIdsToIntervalsWithParents(cover, _indexParams, coveredIntervals);

    auto scan =
        std::make_unique<IndexScan>(expCtx(), collection(), scanParams, workingSet, nullptr);

    // FetchStage owns index scan
    _children.emplace_back(std::make_unique<FetchStage>(
        expCtx(), workingSet, std::move(scan), _nearParams.filter, collection()));

    return std::make_unique<CoveredInterval>(
        _children.back().get(), nextBounds.getInner(), nextBounds.getOuter(), isLastInterval);
}

double GeoNear2DSphereStage::computeDistance(WorkingSetMember* member) {
    return computeGeoNearDistance(_nearParams, member);
}

}  // namespace mongo
