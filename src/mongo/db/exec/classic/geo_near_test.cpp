// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/geo_near.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace {

/**
 * Fixture that sets up a WorkingSet with a single member holding a document with a legacy point
 * geometry, along with a FLAT-CRS near query centered at the origin. This exercises the per-
 * document distance computation path in computeGeoNearDistance() without requiring a geo index.
 */
class GeoNearComputeDistanceTest : public unittest::Test {
protected:
    void setUp() override {
        _ws = std::make_unique<WorkingSet>();
        _id = _ws->allocate();
        _member = _ws->get(_id);

        // Store a document whose "geo" field is a legacy point at (3, 4), which is a FLAT
        // geometry at Euclidean distance 5 from the origin.
        _member->doc = {SnapshotId(), Document{BSON("geo" << BSON_ARRAY(3 << 4))}};
        _ws->transitionToOwnedObj(_id);

        _nearExpr = std::make_unique<GeoNearExpression>("geo");
        _nearExpr->centroid = std::make_unique<PointWithCRS>();
        _nearExpr->centroid->crs = FLAT;
        _nearExpr->centroid->oldPoint = Point(0, 0);

        _params.nearQuery = _nearExpr.get();
    }

    std::unique_ptr<WorkingSet> _ws;
    WorkingSetID _id;
    WorkingSetMember* _member;
    std::unique_ptr<GeoNearExpression> _nearExpr;
    GeoNearParams _params;
};

// When point metadata is not requested (the default), no geoNearPoint metadata should be set, but
// the distance is still computed correctly.
TEST_F(GeoNearComputeDistanceTest, NoPointMetadataWhenAddPointMetaFalse) {
    _params.addDistMeta = true;
    _params.addPointMeta = false;

    const double distance = computeGeoNearDistance(_params, _member);

    ASSERT_APPROX_EQUAL(distance, 5.0, 1e-9);
    ASSERT_TRUE(_member->metadata().hasGeoNearDistance());
    ASSERT_APPROX_EQUAL(_member->metadata().getGeoNearDistance(), 5.0, 1e-9);
    ASSERT_FALSE(_member->metadata().hasGeoNearPoint());
}

// When point metadata is requested, the winning geometry's element is emitted unchanged.
TEST_F(GeoNearComputeDistanceTest, PointMetadataMatchesWinningGeometryWhenAddPointMetaTrue) {
    _params.addDistMeta = true;
    _params.addPointMeta = true;

    const double distance = computeGeoNearDistance(_params, _member);

    ASSERT_APPROX_EQUAL(distance, 5.0, 1e-9);
    ASSERT_TRUE(_member->metadata().hasGeoNearPoint());

    // The emitted point metadata is the stored geometry element, i.e. the [3, 4] array.
    const Value expected{BSON("" << BSON_ARRAY(3 << 4)).firstElement()};
    ASSERT_VALUE_EQ(_member->metadata().getGeoNearPoint(), expected);
}

// When there are multiple candidate geometries, the closest one wins and its element is emitted.
TEST_F(GeoNearComputeDistanceTest, PicksClosestOfMultipleGeometries) {
    // "geo" is an array of two legacy points: (3, 4) at distance 5 and (0, 1) at distance 1.
    _member->doc = {SnapshotId(),
                    Document{BSON("geo" << BSON_ARRAY(BSON_ARRAY(3 << 4) << BSON_ARRAY(0 << 1)))}};
    _member->transitionToOwnedObj();
    _params.addDistMeta = true;
    _params.addPointMeta = true;

    const double distance = computeGeoNearDistance(_params, _member);

    ASSERT_APPROX_EQUAL(distance, 1.0, 1e-9);
    ASSERT_TRUE(_member->metadata().hasGeoNearPoint());

    const Value expected{BSON("" << BSON_ARRAY(0 << 1)).firstElement()};
    ASSERT_VALUE_EQ(_member->metadata().getGeoNearPoint(), expected);
}

// A document with no matching geometry yields no distance and no metadata.
TEST_F(GeoNearComputeDistanceTest, NoGeometryReturnsNegativeDistance) {
    _member->doc = {SnapshotId(), Document{BSON("other" << 1)}};
    _params.addDistMeta = true;
    _params.addPointMeta = true;

    const double distance = computeGeoNearDistance(_params, _member);

    ASSERT_LT(distance, 0.0);
    ASSERT_FALSE(_member->metadata().hasGeoNearDistance());
    ASSERT_FALSE(_member->metadata().hasGeoNearPoint());
}

}  // namespace
}  // namespace mongo
