// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2pointregion.h"

#include "testing/base/public/gunit.h"
#include "util/coding/coder.h"
#include "s2cap.h"
#include "s2cell.h"
#include "s2latlngrect.h"

namespace {

TEST(S2PointRegionTest, Basic) {
  S2Point p(1, 0, 0);
  S2PointRegion r0(p);
  EXPECT_EQ(r0.point(), p);
  EXPECT_TRUE(r0.Contains(p));
  EXPECT_TRUE(r0.VirtualContainsPoint(p));
  EXPECT_TRUE(r0.VirtualContainsPoint(r0.point()));
  EXPECT_FALSE(r0.VirtualContainsPoint(S2Point(1, 0, 1)));
  scoped_ptr<S2PointRegion> r0_clone(r0.Clone());
  EXPECT_EQ(r0_clone->point(), r0.point());
  EXPECT_EQ(r0.GetCapBound(), S2Cap::FromAxisHeight(p, 0));
  S2LatLng ll(p);
  EXPECT_EQ(r0.GetRectBound(), S2LatLngRect(ll, ll));

  // The leaf cell containing a point is still much larger than the point.
  S2Cell cell(p);
  EXPECT_FALSE(r0.Contains(cell));
  EXPECT_TRUE(r0.MayIntersect(cell));
}

TEST(S2PointRegionTest, EncodeAndDecode) {
  S2Point p(.6, .8, 0);
  S2PointRegion r(p);

  Encoder encoder;
  r.Encode(&encoder);

  Decoder decoder(encoder.base(), encoder.length());
  S2PointRegion decoded_r(S2Point(1, 0, 0));
  decoded_r.Decode(&decoder);
  S2Point decoded_p = decoded_r.point();

  EXPECT_EQ(0.6, decoded_p[0]);
  EXPECT_EQ(0.8, decoded_p[1]);
  EXPECT_EQ(0.0, decoded_p[2]);
}

}  // namespace
