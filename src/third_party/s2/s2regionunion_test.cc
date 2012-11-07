// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2regionunion.h"

#include <vector>
using std::vector;


#include "base/scoped_ptr.h"
#include "testing/base/public/gunit.h"
#include "s2cap.h"
#include "s2cell.h"
#include "s2latlngrect.h"
#include "s2pointregion.h"
#include "s2regioncoverer.h"

namespace {

TEST(S2RegionUnionTest, Basic) {
  vector<S2Region*> regions;
  S2RegionUnion ru_empty(&regions);
  EXPECT_EQ(0, ru_empty.num_regions());
  EXPECT_EQ(S2Cap::Empty(), ru_empty.GetCapBound());
  EXPECT_EQ(S2LatLngRect::Empty(), ru_empty.GetRectBound());
  scoped_ptr<S2Region> empty_clone(ru_empty.Clone());

  regions.push_back(new S2PointRegion(S2LatLng::FromDegrees(35, 40)
                                      .ToPoint()));
  regions.push_back(new S2PointRegion(S2LatLng::FromDegrees(-35, -40)
                                      .ToPoint()));

  // Check that Clone() returns a deep copy.
  S2RegionUnion* two_points_orig = new S2RegionUnion(&regions);
  EXPECT_TRUE(regions.empty());

  scoped_ptr<S2RegionUnion> two_points(two_points_orig->Clone());
  delete two_points_orig;
  EXPECT_EQ(S2LatLngRect(S2LatLng::FromDegrees(-35, -40),
                         S2LatLng::FromDegrees(35, 40)),
            two_points->GetRectBound());

  S2Cell face0 = S2Cell::FromFacePosLevel(0, 0, 0);
  EXPECT_TRUE(two_points->MayIntersect(face0));
  EXPECT_FALSE(two_points->Contains(face0));

  EXPECT_TRUE(two_points->Contains(S2LatLng::FromDegrees(35, 40).ToPoint()));
  EXPECT_TRUE(two_points->Contains(S2LatLng::FromDegrees(-35, -40).ToPoint()));
  EXPECT_FALSE(two_points->Contains(S2LatLng::FromDegrees(0, 0).ToPoint()));

  // Check that we can Add() another region.
  scoped_ptr<S2RegionUnion> three_points(two_points->Clone());
  EXPECT_FALSE(three_points->Contains(S2LatLng::FromDegrees(10, 10).ToPoint()));
  three_points->Add(new S2PointRegion(S2LatLng::FromDegrees(10, 10)
                                          .ToPoint()));
  EXPECT_TRUE(three_points->Contains(S2LatLng::FromDegrees(10, 10).ToPoint()));

  S2RegionCoverer coverer;
  coverer.set_max_cells(1);
  vector<S2CellId> covering;
  coverer.GetCovering(*two_points.get(), &covering);
  EXPECT_EQ(1, covering.size());
  EXPECT_EQ(face0.id(), covering[0]);
}

}  // namespace
