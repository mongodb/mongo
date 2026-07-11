// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/index_scan.h"
#include "mongo/db/exec/classic/near.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/geo/r2_region_coverer.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/index/geo/s2_common.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/util/modules.h"

#include <memory>

#include <s2cellunion.h>


namespace mongo {

/**
 * Generic parameters for a GeoNear search
 */
struct GeoNearParams {
    GeoNearParams()
        : filter(nullptr), nearQuery(nullptr), addPointMeta(false), addDistMeta(false) {}

    // MatchExpression to apply to the index keys and fetched documents
    // Not owned here, owned by solution nodes
    MatchExpression* filter;
    // Index scan bounds, not including the geo bounds
    IndexBounds baseBounds;

    // Not owned here
    const GeoNearExpression* nearQuery;
    bool addPointMeta;
    bool addDistMeta;
};

/**
 * Implementation of GeoNear on top of a 2D index
 */
class GeoNear2DStage final : public NearStage {
public:
    GeoNear2DStage(const GeoNearParams& nearParams,
                   ExpressionContext* expCtx,
                   WorkingSet* workingSet,
                   CollectionAcquisition collection,
                   const IndexCatalogEntry* twoDIndex);

protected:
    std::unique_ptr<CoveredInterval> nextInterval(OperationContext* opCtx,
                                                  WorkingSet* workingSet) final;

    double computeDistance(WorkingSetMember* member) final;

    PlanStage::StageState initialize(OperationContext* opCtx,
                                     WorkingSet* workingSet,
                                     WorkingSetID* out) final;

private:
    class DensityEstimator {
    public:
        DensityEstimator(const CollectionAcquisition* collection,
                         PlanStage::Children* children,
                         BSONObj infoObj,
                         const GeoNearParams* nearParams,
                         const R2Annulus& fullBounds);

        PlanStage::StageState work(ExpressionContext* expCtx,
                                   WorkingSet* workingSet,
                                   const IndexCatalogEntry* twoDIndex,
                                   WorkingSetID* out,
                                   double* estimatedDistance);

    private:
        void buildIndexScan(ExpressionContext* expCtx,
                            WorkingSet* workingSet,
                            const IndexCatalogEntry* twoDIndex);

        const CollectionAcquisition* _collection;  // Points to the internal stage _collection.
        PlanStage::Children* _children;    // Points to PlanStage::_children in the NearStage.
        const GeoNearParams* _nearParams;  // Not owned here.
        const R2Annulus& _fullBounds;
        IndexScan* _indexScan = nullptr;  // Owned in PlanStage::_children.
        std::unique_ptr<GeoHashConverter> _converter;
        GeoHash _centroidCell;
        unsigned _currentLevel;
    };

    const GeoNearParams _nearParams;

    // The total search annulus
    const R2Annulus _fullBounds;

    // The current search annulus
    R2Annulus _currBounds;

    // Amount to increment the next bounds by
    double _boundsIncrement;

    // Keeps track of the region that has already been scanned
    R2CellUnion _scannedCells;

    std::unique_ptr<DensityEstimator> _densityEstimator;
};

/**
 * Implementation of GeoNear on top of a 2DSphere (S2) index
 */
class GeoNear2DSphereStage final : public NearStage {
public:
    GeoNear2DSphereStage(const GeoNearParams& nearParams,
                         ExpressionContext* expCtx,
                         WorkingSet* workingSet,
                         CollectionAcquisition collection,
                         const IndexCatalogEntry* s2Index);

protected:
    std::unique_ptr<CoveredInterval> nextInterval(OperationContext* opCtx,
                                                  WorkingSet* workingSet) final;

    double computeDistance(WorkingSetMember* member) final;

    PlanStage::StageState initialize(OperationContext* opCtx,
                                     WorkingSet* workingSet,
                                     WorkingSetID* out) final;

private:
    // Estimate the density of data by search the nearest cells level by level around center.
    class DensityEstimator {
    public:
        DensityEstimator(const CollectionAcquisition* collection,
                         PlanStage::Children* children,
                         const GeoNearParams* nearParams,
                         const S2IndexingParams& indexParams,
                         const R2Annulus& fullBounds);

        // Search for a document in neighbors at current level.
        // Return IS_EOF is such document exists and set the estimated distance to the nearest doc.
        PlanStage::StageState work(ExpressionContext* expCtx,
                                   WorkingSet* workingSet,
                                   const IndexCatalogEntry* s2Index,
                                   WorkingSetID* out,
                                   double* estimatedDistance);

    private:
        void buildIndexScan(ExpressionContext* expCtx,
                            WorkingSet* workingSet,
                            const IndexCatalogEntry* s2Index);

        const CollectionAcquisition* _collection;  // Points to the internal stage _collection
        PlanStage::Children* _children;    // Points to PlanStage::_children in the NearStage.
        const GeoNearParams* _nearParams;  // Not owned here.
        const S2IndexingParams _indexParams;
        const R2Annulus& _fullBounds;
        int _currentLevel;
        IndexScan* _indexScan = nullptr;  // Owned in PlanStage::_children.
    };

    const GeoNearParams _nearParams;

    S2IndexingParams _indexParams;

    // The total search annulus
    const R2Annulus _fullBounds;

    // The current search annulus
    R2Annulus _currBounds;

    // Amount to increment the next bounds by
    double _boundsIncrement;

    // Keeps track of the region that has already been scanned
    S2CellUnion _scannedCells;

    std::unique_ptr<DensityEstimator> _densityEstimator;
};

}  // namespace mongo
