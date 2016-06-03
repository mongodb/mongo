
/**
 *    Copyright (C) 2014 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/db/exec/geo_near.h"

// For s2 search
#include "third_party/s2/s2regionintersection.h"

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/expression_index.h"
#include "mongo/db/query/expression_index_knobs.h"
#include "mongo/util/log.h"

#include <algorithm>

namespace mongo {

using std::abs;
using std::unique_ptr;

namespace dps = ::mongo::dotted_path_support;

//
// Shared GeoNear search functionality
//

static const double kCircOfEarthInMeters = 2 * M_PI * kRadiusOfEarthInMeters;
static const double kMaxEarthDistanceInMeters = kCircOfEarthInMeters / 2;
static const double kMetersPerDegreeAtEquator = kCircOfEarthInMeters / 360;

namespace {

/**
 * Structure that holds BSON addresses (BSONElements) and the corresponding geometry parsed
 * at those locations.
 * Used to separate the parsing of geometries from a BSONObj (which must stay in scope) from
 * the computation over those geometries.
 * TODO: Merge with 2D/2DSphere key extraction?
 */
struct StoredGeometry {
    static StoredGeometry* parseFrom(const BSONElement& element) {
        if (!element.isABSONObj())
            return NULL;

        unique_ptr<StoredGeometry> stored(new StoredGeometry);

        // GeoNear stage can only be run with an existing index
        // Therefore, it is always safe to skip geometry validation
        if (!stored->geometry.parseFromStorage(element, true).isOK())
            return NULL;
        stored->element = element;
        return stored.release();
    }

    BSONElement element;
    GeometryContainer geometry;
};
}

/**
 * Find and parse all geometry elements on the appropriate field path from the document.
 */
static void extractGeometries(const BSONObj& doc,
                              const string& path,
                              vector<StoredGeometry*>* geometries) {
    BSONElementSet geomElements;
    // NOTE: Annoyingly, we cannot just expand arrays b/c single 2d points are arrays, we need
    // to manually expand all results to check if they are geometries
    dps::extractAllElementsAlongPath(doc, path, geomElements, false /* expand arrays */);

    for (BSONElementSet::iterator it = geomElements.begin(); it != geomElements.end(); ++it) {
        const BSONElement& el = *it;
        unique_ptr<StoredGeometry> stored(StoredGeometry::parseFrom(el));

        if (stored.get()) {
            // Valid geometry element
            geometries->push_back(stored.release());
        } else if (el.type() == Array) {
            // Many geometries may be in an array
            BSONObjIterator arrIt(el.Obj());
            while (arrIt.more()) {
                const BSONElement nextEl = arrIt.next();
                stored.reset(StoredGeometry::parseFrom(nextEl));

                if (stored.get()) {
                    // Valid geometry element
                    geometries->push_back(stored.release());
                } else {
                    warning() << "geoNear stage read non-geometry element " << nextEl.toString()
                              << " in array " << el.toString();
                }
            }
        } else {
            warning() << "geoNear stage read non-geometry element " << el.toString();
        }
    }
}

static StatusWith<double> computeGeoNearDistance(const GeoNearParams& nearParams,
                                                 WorkingSetMember* member) {
    //
    // Generic GeoNear distance computation
    // Distances are computed by projecting the stored geometry into the query CRS, and
    // computing distance in that CRS.
    //

    // Must have an object in order to get geometry out of it.
    invariant(member->hasObj());

    CRS queryCRS = nearParams.nearQuery->centroid->crs;

    // Extract all the geometries out of this document for the near query
    OwnedPointerVector<StoredGeometry> geometriesOwned;
    vector<StoredGeometry*>& geometries = geometriesOwned.mutableVector();
    extractGeometries(member->obj.value(), nearParams.nearQuery->field, &geometries);

    // Compute the minimum distance of all the geometries in the document
    double minDistance = -1;
    BSONObj minDistanceObj;
    for (vector<StoredGeometry*>::iterator it = geometries.begin(); it != geometries.end(); ++it) {
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
            minDistanceObj = stored.element.Obj();
        }
    }

    if (minDistance < 0) {
        // No distance to report
        return StatusWith<double>(-1);
    }

    if (nearParams.addDistMeta) {
        if (nearParams.nearQuery->unitsAreRadians) {
            // Hack for nearSphere
            // TODO: Remove nearSphere?
            invariant(SPHERE == queryCRS);
            member->addComputed(new GeoDistanceComputedData(minDistance / kRadiusOfEarthInMeters));
        } else {
            member->addComputed(new GeoDistanceComputedData(minDistance));
        }
    }

    if (nearParams.addPointMeta) {
        member->addComputed(new GeoNearPointComputedData(minDistanceObj));
    }

    return StatusWith<double>(minDistance);
}

static R2Annulus geoNearDistanceBounds(const GeoNearExpression& query) {
    const CRS queryCRS = query.centroid->crs;

    if (FLAT == queryCRS) {
        return R2Annulus(query.centroid->oldPoint, query.minDistance, query.maxDistance);
    }

    invariant(SPHERE == queryCRS);

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
        // Reset the full bounds based on our index bounds
        GeoHashConverter::Parameters hashParams;
        Status status = GeoHashConverter::parseParameters(twoDIndex->infoObj(), &hashParams);
        invariant(status.isOK());  // The index status should always be valid

        // The biggest distance possible in this indexed collection is the diagonal of the
        // square indexed region.
        const double sqrt2Approx = 1.5;
        const double diagonalDist = sqrt2Approx * (hashParams.max - hashParams.min);

        fullBounds = R2Annulus(
            fullBounds.center(), fullBounds.getInner(), min(fullBounds.getOuter(), diagonalDist));
    } else {
        // Spherical queries have upper bounds set by the earth - no-op
        // TODO: Wrapping errors would creep in here if nearSphere wasn't defined to not wrap
        invariant(SPHERE == queryCRS);
        invariant(!nearParams.nearQuery->isWrappingQuery);
    }

    return fullBounds;
}

class GeoNear2DStage::DensityEstimator {
public:
    DensityEstimator(PlanStage::Children* children,
                     const IndexDescriptor* twoDindex,
                     const GeoNearParams* nearParams)
        : _children(children), _twoDIndex(twoDindex), _nearParams(nearParams), _currentLevel(0) {
        GeoHashConverter::Parameters hashParams;
        Status status = GeoHashConverter::parseParameters(_twoDIndex->infoObj(), &hashParams);
        // The index status should always be valid.
        invariant(status.isOK());

        _converter.reset(new GeoHashConverter(hashParams));
        _centroidCell = _converter->hash(_nearParams->nearQuery->centroid->oldPoint);

        // Since appendVertexNeighbors(level, output) requires level < hash.getBits(),
        // we have to start to find documents at most GeoHash::kMaxBits - 1. Thus the finest
        // search area is 16 * finest cell area at GeoHash::kMaxBits.
        _currentLevel = std::max(0u, hashParams.bits - 1u);
    }

    PlanStage::StageState work(OperationContext* txn,
                               WorkingSet* workingSet,
                               Collection* collection,
                               WorkingSetID* out,
                               double* estimatedDistance);

private:
    void buildIndexScan(OperationContext* txn, WorkingSet* workingSet, Collection* collection);

    PlanStage::Children* _children;     // Points to PlanStage::_children in the NearStage.
    const IndexDescriptor* _twoDIndex;  // Not owned here.
    const GeoNearParams* _nearParams;   // Not owned here.
    IndexScan* _indexScan = nullptr;    // Owned in PlanStage::_children.
    unique_ptr<GeoHashConverter> _converter;
    GeoHash _centroidCell;
    unsigned _currentLevel;
};

// Initialize the internal states
void GeoNear2DStage::DensityEstimator::buildIndexScan(OperationContext* txn,
                                                      WorkingSet* workingSet,
                                                      Collection* collection) {
    IndexScanParams scanParams;
    scanParams.descriptor = _twoDIndex;
    scanParams.direction = 1;
    scanParams.doNotDedup = true;

    // Scan bounds on 2D indexes are only over the 2D field - other bounds aren't applicable.
    // This is handled in query planning.
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
        oil.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(builder.obj(), true, true));
    }

    invariant(oil.isValidFor(1));

    // Intersect the $near bounds we just generated into the bounds we have for anything else
    // in the scan (i.e. $within)
    IndexBoundsBuilder::intersectize(oil, &scanParams.bounds.fields[twoDFieldPosition]);

    invariant(!_indexScan);
    _indexScan = new IndexScan(txn, scanParams, workingSet, NULL);
    _children->emplace_back(_indexScan);
}

// Return IS_EOF is we find a document in it's ancestor cells and set estimated distance
// from the nearest document.
PlanStage::StageState GeoNear2DStage::DensityEstimator::work(OperationContext* txn,
                                                             WorkingSet* workingSet,
                                                             Collection* collection,
                                                             WorkingSetID* out,
                                                             double* estimatedDistance) {
    if (!_indexScan) {
        // Setup index scan stage for current level.
        buildIndexScan(txn, workingSet, collection);
    }

    WorkingSetID workingSetID;
    PlanStage::StageState state = _indexScan->work(&workingSetID);

    if (state == PlanStage::IS_EOF) {
        // We ran through the neighbors but found nothing.
        if (_currentLevel > 0u) {
            // Advance to the next level and search again.
            _currentLevel--;
            // Reset index scan for the next level.
            invariant(_children->back().get() == _indexScan);
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

PlanStage::StageState GeoNear2DStage::initialize(OperationContext* txn,
                                                 WorkingSet* workingSet,
                                                 Collection* collection,
                                                 WorkingSetID* out) {
    if (!_densityEstimator) {
        _densityEstimator.reset(new DensityEstimator(&_children, _twoDIndex, &_nearParams));
    }

    double estimatedDistance;
    PlanStage::StageState state =
        _densityEstimator->work(txn, workingSet, collection, out, &estimatedDistance);

    if (state == PlanStage::IS_EOF) {
        // 2d index only works with legacy points as centroid. $nearSphere will project
        // the point into SPHERE CRS and calculate distance based on that.
        // STRICT_SPHERE is impossible here, as GeoJSON centroid is not allowed for 2d index.

        // Estimator finished its work, we need to finish initialization too.
        if (SPHERE == _nearParams.nearQuery->centroid->crs) {
            // Estimated distance is in degrees, convert it to meters.
            _boundsIncrement = deg2rad(estimatedDistance) * kRadiusOfEarthInMeters * 3;
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
        invariant(_boundsIncrement > 0.0);

        // Clean up
        _densityEstimator.reset(NULL);
    }

    return state;
}

static const string kTwoDIndexNearStage("GEO_NEAR_2D");

GeoNear2DStage::GeoNear2DStage(const GeoNearParams& nearParams,
                               OperationContext* txn,
                               WorkingSet* workingSet,
                               Collection* collection,
                               IndexDescriptor* twoDIndex)
    : NearStage(txn, kTwoDIndexNearStage.c_str(), STAGE_GEO_NEAR_2D, workingSet, collection),
      _nearParams(nearParams),
      _twoDIndex(twoDIndex),
      _fullBounds(twoDDistanceBounds(nearParams, twoDIndex)),
      _currBounds(_fullBounds.center(), -1, _fullBounds.getInner()),
      _boundsIncrement(0.0) {
    _specificStats.keyPattern = twoDIndex->keyPattern();
    _specificStats.indexName = twoDIndex->indexName();
    _specificStats.indexVersion = twoDIndex->version();
}


namespace {

/**
 * Expression which checks whether a legacy 2D index point is contained within our near
 * search annulus.  See nextInterval() below for more discussion.
 * TODO: Make this a standard type of GEO match expression
 */
class TwoDPtInAnnulusExpression : public LeafMatchExpression {
public:
    TwoDPtInAnnulusExpression(const R2Annulus& annulus, StringData twoDPath)
        : LeafMatchExpression(INTERNAL_2D_POINT_IN_ANNULUS), _annulus(annulus) {
        setPath(twoDPath);
    }

    void serialize(BSONObjBuilder* out) const final {
        out->append("TwoDPtInAnnulusExpression", true);
    }

    bool matchesSingleElement(const BSONElement& e) const final {
        if (!e.isABSONObj())
            return false;

        PointWithCRS point;
        if (!GeoParser::parseStoredPoint(e, &point).isOK())
            return false;

        return _annulus.contains(point.oldPoint);
    }

    //
    // These won't be called.
    //

    void debugString(StringBuilder& debug, int level = 0) const final {
        invariant(false);
    }

    bool equivalent(const MatchExpression* other) const final {
        invariant(false);
        return false;
    }

    unique_ptr<MatchExpression> shallowClone() const final {
        invariant(false);
        return NULL;
    }

private:
    R2Annulus _annulus;
};

// Helper class to maintain ownership of a match expression alongside an index scan
class FetchStageWithMatch final : public FetchStage {
public:
    FetchStageWithMatch(OperationContext* txn,
                        WorkingSet* ws,
                        PlanStage* child,
                        MatchExpression* filter,
                        const Collection* collection)
        : FetchStage(txn, ws, child, filter, collection), _matcher(filter) {}

private:
    // Owns matcher
    const unique_ptr<MatchExpression> _matcher;
};
}

static double min2DBoundsIncrement(const GeoNearExpression& query, IndexDescriptor* twoDIndex) {
    GeoHashConverter::Parameters hashParams;
    Status status = GeoHashConverter::parseParameters(twoDIndex->infoObj(), &hashParams);
    invariant(status.isOK());  // The index status should always be valid
    GeoHashConverter hasher(hashParams);

    // The hasher error is the diagonal of a 2D hash region - it's generally not helpful
    // to change region size such that a search radius is smaller than the 2D hash region
    // max radius.  This is slightly conservative for now (box diagonal vs circle radius).
    double minBoundsIncrement = hasher.getError() / 2;

    const CRS queryCRS = query.centroid->crs;
    if (FLAT == queryCRS)
        return minBoundsIncrement;

    invariant(SPHERE == queryCRS);

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

StatusWith<NearStage::CoveredInterval*>  //
    GeoNear2DStage::nextInterval(OperationContext* txn,
                                 WorkingSet* workingSet,
                                 Collection* collection) {
    // The search is finished if we searched at least once and all the way to the edge
    if (_currBounds.getInner() >= 0 && _currBounds.getOuter() == _fullBounds.getOuter()) {
        return StatusWith<CoveredInterval*>(NULL);
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
        max(_boundsIncrement, min2DBoundsIncrement(*_nearParams.nearQuery, _twoDIndex));

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
        invariant(SPHERE == queryCRS);
        // TODO: As above, make this consistent with $within : $centerSphere

        // Our intervals aren't in the same CRS as our index, so we need to adjust them
        coverRegion.reset(new R2Annulus(projectBoundsToTwoDDegrees(nextBounds)));
    }

    //
    // Setup the stages for this interval
    //

    IndexScanParams scanParams;
    scanParams.descriptor = _twoDIndex;
    scanParams.direction = 1;

    // This does force us to do our own deduping of results.
    scanParams.doNotDedup = true;

    // Scan bounds on 2D indexes are only over the 2D field - other bounds aren't applicable.
    // This is handled in query planning.
    scanParams.bounds = _nearParams.baseBounds;

    // The "2d" field is always the first in the index
    const string twoDFieldName = _nearParams.nearQuery->field;
    const int twoDFieldPosition = 0;

    std::vector<GeoHash> unorderedCovering = ExpressionMapping::get2dCovering(
        *coverRegion, _twoDIndex->infoObj(), internalGeoNearQuery2DMaxCoveringCells);

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
    GeoHashConverter::Parameters hashParams;
    GeoHashConverter::parseParameters(_twoDIndex->infoObj(), &hashParams);

    // 2D indexes support covered search over additional fields they contain
    IndexScan* scan = new IndexScan(txn, scanParams, workingSet, _nearParams.filter);

    MatchExpression* docMatcher = nullptr;

    // FLAT searches need to add an additional annulus $within matcher, see above
    // TODO: Find out if this matcher is actually needed
    if (FLAT == queryCRS) {
        docMatcher = new TwoDPtInAnnulusExpression(_fullBounds, twoDFieldName);
    }

    // FetchStage owns index scan
    _children.emplace_back(new FetchStageWithMatch(txn, workingSet, scan, docMatcher, collection));

    return StatusWith<CoveredInterval*>(new CoveredInterval(_children.back().get(),
                                                            true,
                                                            nextBounds.getInner(),
                                                            nextBounds.getOuter(),
                                                            isLastInterval));
}

StatusWith<double> GeoNear2DStage::computeDistance(WorkingSetMember* member) {
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
                                           OperationContext* txn,
                                           WorkingSet* workingSet,
                                           Collection* collection,
                                           IndexDescriptor* s2Index)
    : NearStage(txn, kS2IndexNearStage.c_str(), STAGE_GEO_NEAR_2DSPHERE, workingSet, collection),
      _nearParams(nearParams),
      _s2Index(s2Index),
      _fullBounds(geoNearDistanceBounds(*nearParams.nearQuery)),
      _currBounds(_fullBounds.center(), -1, _fullBounds.getInner()),
      _boundsIncrement(0.0) {
    _specificStats.keyPattern = s2Index->keyPattern();
    _specificStats.indexName = s2Index->indexName();
    _specificStats.indexVersion = s2Index->version();

    // initialize2dsphereParams() does not require the collator during the GEO_NEAR_2DSPHERE stage.
    // It only requires the collator for index key generation. For query execution,
    // _nearParams.baseBounds should have collator-generated comparison keys in place of raw
    // strings, and _nearParams.filter should have the collator.
    const CollatorInterface* collator = nullptr;
    ExpressionParams::initialize2dsphereParams(s2Index->infoObj(), collator, &_indexParams);
}

GeoNear2DSphereStage::~GeoNear2DSphereStage() {}

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

    // We only need to max bound if this is not a full search of the Earth
    // Using the constant here is important since we use the min of kMaxEarthDistance
    // and the actual bounds passed in to set up the search area.
    if (outer < kMaxEarthDistanceInMeters) {
        S2Cap outerCap = S2Cap::FromAxisAngle(latLng.ToPoint(),
                                              S1Angle::Radians(outer / kRadiusOfEarthInMeters));
        regions.push_back(new S2Cap(outerCap));
    }

    // if annulus is entire world, return a full cap
    if (regions.empty()) {
        regions.push_back(new S2Cap(S2Cap::Full()));
    }

    // Takes ownership of caps
    return new S2RegionIntersection(&regions);
}
}

// Estimate the density of data by search the nearest cells level by level around center.
class GeoNear2DSphereStage::DensityEstimator {
public:
    DensityEstimator(PlanStage::Children* children,
                     const IndexDescriptor* s2Index,
                     const GeoNearParams* nearParams,
                     const S2IndexingParams& indexParams)
        : _children(children),
          _s2Index(s2Index),
          _nearParams(nearParams),
          _indexParams(indexParams),
          _currentLevel(0) {
        // cellId.AppendVertexNeighbors(level, output) requires level < finest,
        // so we use the minimum of max_level - 1 and the user specified finest
        int level = std::min(S2::kMaxCellLevel - 1, internalQueryS2GeoFinestLevel.load());
        _currentLevel = std::max(0, level);
    }

    // Search for a document in neighbors at current level.
    // Return IS_EOF is such document exists and set the estimated distance to the nearest doc.
    PlanStage::StageState work(OperationContext* txn,
                               WorkingSet* workingSet,
                               Collection* collection,
                               WorkingSetID* out,
                               double* estimatedDistance);

private:
    void buildIndexScan(OperationContext* txn, WorkingSet* workingSet, Collection* collection);

    PlanStage::Children* _children;    // Points to PlanStage::_children in the NearStage.
    const IndexDescriptor* _s2Index;   // Not owned here.
    const GeoNearParams* _nearParams;  // Not owned here.
    const S2IndexingParams _indexParams;
    int _currentLevel;
    IndexScan* _indexScan = nullptr;  // Owned in PlanStage::_children.
};

// Setup the index scan stage for neighbors at this level.
void GeoNear2DSphereStage::DensityEstimator::buildIndexScan(OperationContext* txn,
                                                            WorkingSet* workingSet,
                                                            Collection* collection) {
    IndexScanParams scanParams;
    scanParams.descriptor = _s2Index;
    scanParams.direction = 1;
    scanParams.doNotDedup = true;
    scanParams.bounds = _nearParams->baseBounds;

    // Because the planner doesn't yet set up 2D index bounds, do it ourselves here
    const string s2Field = _nearParams->nearQuery->field;
    const int s2FieldPosition = getFieldPosition(_s2Index, s2Field);
    fassert(28677, s2FieldPosition >= 0);
    OrderedIntervalList* coveredIntervals = &scanParams.bounds.fields[s2FieldPosition];
    coveredIntervals->intervals.clear();

    // Find 4 neighbors (3 neighbors at face vertex) at current level.
    const S2CellId& centerId = _nearParams->nearQuery->centroid->cell.id();
    vector<S2CellId> neighbors;

    // The search area expands 4X each time.
    // Return the neighbors of closest vertex to this cell at the given level.
    invariant(_currentLevel < centerId.level());
    centerId.AppendVertexNeighbors(_currentLevel, &neighbors);

    ExpressionMapping::S2CellIdsToIntervals(neighbors, _indexParams.indexVersion, coveredIntervals);

    // Index scan
    invariant(!_indexScan);
    _indexScan = new IndexScan(txn, scanParams, workingSet, NULL);
    _children->emplace_back(_indexScan);
}

PlanStage::StageState GeoNear2DSphereStage::DensityEstimator::work(OperationContext* txn,
                                                                   WorkingSet* workingSet,
                                                                   Collection* collection,
                                                                   WorkingSetID* out,
                                                                   double* estimatedDistance) {
    if (!_indexScan) {
        // Setup index scan stage for current level.
        buildIndexScan(txn, workingSet, collection);
    }

    WorkingSetID workingSetID;
    PlanStage::StageState state = _indexScan->work(&workingSetID);

    if (state == PlanStage::IS_EOF) {
        // We ran through the neighbors but found nothing.
        if (_currentLevel > 0) {
            // Advance to the next level and search again.
            _currentLevel--;
            // Reset index scan for the next level.
            invariant(_children->back().get() == _indexScan);
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


PlanStage::StageState GeoNear2DSphereStage::initialize(OperationContext* txn,
                                                       WorkingSet* workingSet,
                                                       Collection* collection,
                                                       WorkingSetID* out) {
    if (!_densityEstimator) {
        _densityEstimator.reset(
            new DensityEstimator(&_children, _s2Index, &_nearParams, _indexParams));
    }

    double estimatedDistance;
    PlanStage::StageState state =
        _densityEstimator->work(txn, workingSet, collection, out, &estimatedDistance);

    if (state == IS_EOF) {
        // We find a document in 4 neighbors at current level, but didn't at previous level.
        //
        // Assuming cell size at current level is d and data is even distributed, the distance
        // between two nearest points are at least d. The following circle with radius of 3 * d
        // covers PI * 9 * d^2, giving at most 30 documents.
        //
        // At the coarsest level, the search area is the whole earth.
        _boundsIncrement = 3 * estimatedDistance;
        invariant(_boundsIncrement > 0.0);

        // Clean up
        _densityEstimator.reset(NULL);
    }

    return state;
}

StatusWith<NearStage::CoveredInterval*>  //
    GeoNear2DSphereStage::nextInterval(OperationContext* txn,
                                       WorkingSet* workingSet,
                                       Collection* collection) {
    // The search is finished if we searched at least once and all the way to the edge
    if (_currBounds.getInner() >= 0 && _currBounds.getOuter() == _fullBounds.getOuter()) {
        return StatusWith<CoveredInterval*>(NULL);
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

    invariant(_boundsIncrement > 0.0);

    R2Annulus nextBounds(_currBounds.center(),
                         _currBounds.getOuter(),
                         min(_currBounds.getOuter() + _boundsIncrement, _fullBounds.getOuter()));

    bool isLastInterval = (nextBounds.getOuter() == _fullBounds.getOuter());
    _currBounds = nextBounds;

    //
    // Setup the covering region and stages for this interval
    //

    IndexScanParams scanParams;
    scanParams.descriptor = _s2Index;
    scanParams.direction = 1;

    // This does force us to do our own deduping of results.
    scanParams.doNotDedup = true;
    scanParams.bounds = _nearParams.baseBounds;

    // Because the planner doesn't yet set up 2D index bounds, do it ourselves here
    const string s2Field = _nearParams.nearQuery->field;
    const int s2FieldPosition = getFieldPosition(_s2Index, s2Field);
    fassert(28678, s2FieldPosition >= 0);
    scanParams.bounds.fields[s2FieldPosition].intervals.clear();
    std::unique_ptr<S2Region> region(buildS2Region(_currBounds));

    std::vector<S2CellId> cover = ExpressionMapping::get2dsphereCovering(*region);

    // Generate a covering that does not intersect with any previous coverings
    S2CellUnion coverUnion;
    coverUnion.InitSwap(&cover);
    invariant(cover.empty());
    S2CellUnion diffUnion;
    diffUnion.GetDifference(&coverUnion, &_scannedCells);
    for (auto cellId : diffUnion.cell_ids()) {
        if (region->MayIntersect(S2Cell(cellId))) {
            cover.push_back(cellId);
        }
    }

    // Add the cells in this covering to the _scannedCells union
    _scannedCells.Add(cover);

    OrderedIntervalList* coveredIntervals = &scanParams.bounds.fields[s2FieldPosition];
    ExpressionMapping::S2CellIdsToIntervalsWithParents(cover, _indexParams, coveredIntervals);

    IndexScan* scan = new IndexScan(txn, scanParams, workingSet, nullptr);

    // FetchStage owns index scan
    _children.emplace_back(new FetchStage(txn, workingSet, scan, _nearParams.filter, collection));

    return StatusWith<CoveredInterval*>(new CoveredInterval(_children.back().get(),
                                                            true,
                                                            nextBounds.getInner(),
                                                            nextBounds.getOuter(),
                                                            isLastInterval));
}

StatusWith<double> GeoNear2DSphereStage::computeDistance(WorkingSetMember* member) {
    return computeGeoNearDistance(_nearParams, member);
}

}  // namespace mongo
