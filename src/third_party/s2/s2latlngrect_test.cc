// Copyright 2005 Google Inc. All Rights Reserved.
//
// Most of the S2LatLngRect methods have trivial implementations that
// use the R1Interval and S1Interval classes, so most of the testing
// is done in those unit tests.

#include "s2latlngrect.h"
#include "util/coding/coder.h"
#include "s2edgeutil.h"
#include "s2cap.h"
#include "s2cell.h"
#include "s2testing.h"
#include "testing/base/public/gunit.h"

static S2LatLngRect RectFromDegrees(double lat_lo, double lng_lo,
                                    double lat_hi, double lng_hi) {
  // Convenience method to construct a rectangle.  This method is
  // intentionally *not* in the S2LatLngRect interface because the
  // argument order is ambiguous, but hopefully it's not too confusing
  // within the context of this unit test.

  return S2LatLngRect(S2LatLng::FromDegrees(lat_lo, lng_lo).Normalized(),
                      S2LatLng::FromDegrees(lat_hi, lng_hi).Normalized());
}

TEST(S2LatLngRect, EmptyAndFull) {
  // Test basic properties of empty and full rectangles.
  S2LatLngRect empty = S2LatLngRect::Empty();
  S2LatLngRect full = S2LatLngRect::Full();
  EXPECT_TRUE(empty.is_valid());
  EXPECT_TRUE(empty.is_empty());
  EXPECT_FALSE(empty.is_point());
  EXPECT_TRUE(full.is_valid());
  EXPECT_TRUE(full.is_full());
  EXPECT_FALSE(full.is_point());
  // Check that the default S2LatLngRect is identical to Empty().
  S2LatLngRect default_empty;
  EXPECT_TRUE(default_empty.is_valid());
  EXPECT_TRUE(default_empty.is_empty());
  EXPECT_EQ(empty.lat().bounds(), default_empty.lat().bounds());
  EXPECT_EQ(empty.lng().bounds(), default_empty.lng().bounds());
}

TEST(S2LatLngRect, Accessors) {
  // Check various accessor methods.
  S2LatLngRect d1 = RectFromDegrees(-90, 0, -45, 180);
  EXPECT_DOUBLE_EQ(d1.lat_lo().degrees(), -90);
  EXPECT_DOUBLE_EQ(d1.lat_hi().degrees(), -45);
  EXPECT_DOUBLE_EQ(d1.lng_lo().degrees(), 0);
  EXPECT_DOUBLE_EQ(d1.lng_hi().degrees(), 180);
  EXPECT_EQ(d1.lat(), R1Interval(-M_PI_2, -M_PI_4));
  EXPECT_EQ(d1.lng(), S1Interval(0, M_PI));
}

TEST(S2LatLngRect, FromCenterSize) {
  EXPECT_TRUE(S2LatLngRect::FromCenterSize(S2LatLng::FromDegrees(80, 170),
                                           S2LatLng::FromDegrees(40, 60)).
              ApproxEquals(RectFromDegrees(60, 140, 90, -160)));
  EXPECT_TRUE(S2LatLngRect::FromCenterSize(S2LatLng::FromDegrees(10, 40),
                                           S2LatLng::FromDegrees(210, 400)).
              is_full());
  EXPECT_TRUE(S2LatLngRect::FromCenterSize(S2LatLng::FromDegrees(-90, 180),
                                           S2LatLng::FromDegrees(20, 50)).
              ApproxEquals(RectFromDegrees(-90, 155, -80, -155)));
}

TEST(S2LatLngRect, FromPoint) {
  S2LatLng p = S2LatLng::FromDegrees(23, 47);
  EXPECT_EQ(S2LatLngRect::FromPoint(p), S2LatLngRect(p, p));
  EXPECT_TRUE(S2LatLngRect::FromPoint(p).is_point());
}

TEST(S2LatLngRect, FromPointPair) {
  EXPECT_EQ(S2LatLngRect::FromPointPair(S2LatLng::FromDegrees(-35, -140),
                                        S2LatLng::FromDegrees(15, 155)),
            RectFromDegrees(-35, 155, 15, -140));
  EXPECT_EQ(S2LatLngRect::FromPointPair(S2LatLng::FromDegrees(25, -70),
                                        S2LatLng::FromDegrees(-90, 80)),
            RectFromDegrees(-90, -70, 25, 80));
}

TEST(S2LatLngRect, GetCenterSize) {
  S2LatLngRect r1(R1Interval(0, M_PI_2), S1Interval(-M_PI, 0));
  EXPECT_EQ(r1.GetCenter(), S2LatLng::FromRadians(M_PI_4, -M_PI_2));
  EXPECT_EQ(r1.GetSize(), S2LatLng::FromRadians(M_PI_2, M_PI));
  EXPECT_LT(S2LatLngRect::Empty().GetSize().lat().radians(), 0);
  EXPECT_LT(S2LatLngRect::Empty().GetSize().lng().radians(), 0);
}

TEST(S2LatLngRect, GetVertex) {
  S2LatLngRect r1(R1Interval(0, M_PI_2), S1Interval(-M_PI, 0));
  EXPECT_EQ(r1.GetVertex(0), S2LatLng::FromRadians(0, M_PI));
  EXPECT_EQ(r1.GetVertex(1), S2LatLng::FromRadians(0, 0));
  EXPECT_EQ(r1.GetVertex(2), S2LatLng::FromRadians(M_PI_2, 0));
  EXPECT_EQ(r1.GetVertex(3), S2LatLng::FromRadians(M_PI_2, M_PI));

  // Make sure that GetVertex() returns vertices in CCW order.
  for (int i = 0; i < 4; ++i) {
    double lat = M_PI_4 * (i - 2);
    double lng = M_PI_2 * (i - 2) + 0.2;
    S2LatLngRect r(R1Interval(lat, lat + M_PI_4),
                   S1Interval(drem(lng, 2*M_PI), drem(lng + M_PI_2, 2*M_PI)));
    for (int k = 0; k < 4; ++k) {
      EXPECT_TRUE(S2::SimpleCCW(r.GetVertex((k - 1) & 3).ToPoint(),
                                r.GetVertex(k).ToPoint(),
                                r.GetVertex((k + 1) & 3).ToPoint()));
    }
  }
}

TEST(S2LatLngRect, Contains) {
  // Contains(S2LatLng), InteriorContains(S2LatLng), VirtualContainsPoint()
  S2LatLng eq_m180 = S2LatLng::FromRadians(0, -M_PI);
  S2LatLng north_pole = S2LatLng::FromRadians(M_PI_2, 0);
  S2LatLngRect r1(eq_m180, north_pole);

  EXPECT_TRUE(r1.Contains(S2LatLng::FromDegrees(30, -45)));
  EXPECT_TRUE(r1.InteriorContains(S2LatLng::FromDegrees(30, -45)));
  EXPECT_FALSE(r1.Contains(S2LatLng::FromDegrees(30, 45)));
  EXPECT_FALSE(r1.InteriorContains(S2LatLng::FromDegrees(30, 45)));
  EXPECT_TRUE(r1.Contains(eq_m180));
  EXPECT_FALSE(r1.InteriorContains(eq_m180));
  EXPECT_TRUE(r1.Contains(north_pole));
  EXPECT_FALSE(r1.InteriorContains(north_pole));
  EXPECT_TRUE(r1.Contains(S2Point(0.5, -0.3, 0.1)));
  EXPECT_TRUE(r1.VirtualContainsPoint(S2Point(0.5, -0.3, 0.1)));
  EXPECT_FALSE(r1.Contains(S2Point(0.5, 0.2, 0.1)));
  EXPECT_FALSE(r1.VirtualContainsPoint(S2Point(0.5, 0.2, 0.1)));
}

static void TestIntervalOps(S2LatLngRect const& x, S2LatLngRect const& y,
                            const char* expected_relation,
                            S2LatLngRect const& expected_union,
                            S2LatLngRect const& expected_intersection) {
  // Test all of the interval operations on the given pair of intervals.
  // "expected_relation" is a sequence of "T" and "F" characters corresponding
  // to the expected results of Contains(), InteriorContains(), Intersects(),
  // and InteriorIntersects() respectively.

  EXPECT_EQ(x.Contains(y), expected_relation[0] == 'T');
  EXPECT_EQ(x.InteriorContains(y), expected_relation[1] == 'T');
  EXPECT_EQ(x.Intersects(y), expected_relation[2] == 'T');
  EXPECT_EQ(x.InteriorIntersects(y), expected_relation[3] == 'T');

  EXPECT_EQ(x.Contains(y), x.Union(y) == x);
  EXPECT_EQ(x.Intersects(y), !x.Intersection(y).is_empty());

  EXPECT_EQ(x.Union(y), expected_union);
  EXPECT_EQ(x.Intersection(y), expected_intersection);

  if (y.GetSize() == S2LatLng::FromRadians(0, 0)) {
    S2LatLngRect r = x;
    r.AddPoint(y.lo());
    EXPECT_EQ(r, expected_union);
  }
}

TEST(S2LatLngRect, IntervalOps) {
  // Contains(S2LatLngRect), InteriorContains(S2LatLngRect),
  // Intersects(), InteriorIntersects(), Union(), Intersection().
  //
  // Much more testing of these methods is done in s1interval_unittest
  // and r1interval_unittest.

  // Rectangle "r1" covers one-quarter of the sphere.
  S2LatLngRect r1 = RectFromDegrees(0, -180, 90, 0);

  // Test operations where one rectangle consists of a single point.
  S2LatLngRect r1_mid = RectFromDegrees(45, -90, 45, -90);
  TestIntervalOps(r1, r1_mid, "TTTT", r1, r1_mid);

  S2LatLngRect req_m180 = RectFromDegrees(0, -180, 0, -180);
  TestIntervalOps(r1, req_m180, "TFTF", r1, req_m180);

  S2LatLngRect rnorth_pole = RectFromDegrees(90, 0, 90, 0);
  TestIntervalOps(r1, rnorth_pole, "TFTF", r1, rnorth_pole);

  TestIntervalOps(r1, RectFromDegrees(-10, -1, 1, 20), "FFTT",
                  RectFromDegrees(-10, 180, 90, 20),
                  RectFromDegrees(0, -1, 1, 0));
  TestIntervalOps(r1, RectFromDegrees(-10, -1, 0, 20), "FFTF",
                  RectFromDegrees(-10, 180, 90, 20),
                  RectFromDegrees(0, -1, 0, 0));
  TestIntervalOps(r1, RectFromDegrees(-10, 0, 1, 20), "FFTF",
                  RectFromDegrees(-10, 180, 90, 20),
                  RectFromDegrees(0, 0, 1, 0));

  TestIntervalOps(RectFromDegrees(-15, -160, -15, -150),
                  RectFromDegrees(20, 145, 25, 155), "FFFF",
                  RectFromDegrees(-15, 145, 25, -150),
                  S2LatLngRect::Empty());
  TestIntervalOps(RectFromDegrees(70, -10, 90, -140),
                  RectFromDegrees(60, 175, 80, 5), "FFTT",
                  RectFromDegrees(60, -180, 90, 180),
                  RectFromDegrees(70, 175, 80, 5));

  // Check that the intersection of two rectangles that overlap in latitude
  // but not longitude is valid, and vice versa.
  TestIntervalOps(RectFromDegrees(12, 30, 60, 60),
                  RectFromDegrees(0, 0, 30, 18), "FFFF",
                  RectFromDegrees(0, 0, 60, 60), S2LatLngRect::Empty());
  TestIntervalOps(RectFromDegrees(0, 0, 18, 42),
                  RectFromDegrees(30, 12, 42, 60), "FFFF",
                  RectFromDegrees(0, 0, 42, 60), S2LatLngRect::Empty());
}

TEST(S2LatLngRect, AddPoint) {
  S2LatLngRect p = S2LatLngRect::Empty();
  p.AddPoint(S2LatLng::FromDegrees(0, 0));
  EXPECT_TRUE(p.is_point());
  p.AddPoint(S2LatLng::FromRadians(0, -M_PI_2));
  EXPECT_FALSE(p.is_point());
  p.AddPoint(S2LatLng::FromRadians(M_PI_4, -M_PI));
  p.AddPoint(S2Point(0, 0, 1));
  EXPECT_EQ(p, RectFromDegrees(0, -180, 90, 0));
}

TEST(S2LatLngRect, Expanded) {
  EXPECT_TRUE(RectFromDegrees(70, 150, 80, 170).
              Expanded(S2LatLng::FromDegrees(20, 30)).
              ApproxEquals(RectFromDegrees(50, 120, 90, -160)));
  EXPECT_TRUE(S2LatLngRect::Empty().Expanded(S2LatLng::FromDegrees(20, 30)).
              is_empty());
  EXPECT_TRUE(S2LatLngRect::Full().Expanded(S2LatLng::FromDegrees(20, 30)).
              is_full());
  EXPECT_TRUE(RectFromDegrees(-90, 170, 10, 20).
              Expanded(S2LatLng::FromDegrees(30, 80)).
              ApproxEquals(RectFromDegrees(-90, -180, 40, 180)));
}

TEST(S2LatLngRect, ConvolveWithCap) {
  EXPECT_TRUE(RectFromDegrees(0, 170, 0, -170).
              ConvolveWithCap(S1Angle::Degrees(15)).ApproxEquals(
                  RectFromDegrees(-15, 155, 15, -155)));
  EXPECT_TRUE(RectFromDegrees(60, 150, 80, 10).
              ConvolveWithCap(S1Angle::Degrees(15)).ApproxEquals(
                  RectFromDegrees(45, -180, 90, 180)));
}

TEST(S2LatLngRect, GetCapBound) {
  // Bounding cap at center is smaller:
  EXPECT_TRUE(RectFromDegrees(-45, -45, 45, 45).GetCapBound().
              ApproxEquals(S2Cap::FromAxisHeight(S2Point(1, 0, 0), 0.5)));

  // Bounding cap at north pole is smaller:
  EXPECT_TRUE(RectFromDegrees(88, -80, 89, 80).GetCapBound().
              ApproxEquals(S2Cap::FromAxisAngle(S2Point(0, 0, 1),
                                                S1Angle::Degrees(2))));

  // Longitude span > 180 degrees:
  EXPECT_TRUE(RectFromDegrees(-30, -150, -10, 50).GetCapBound().
              ApproxEquals(S2Cap::FromAxisAngle(S2Point(0, 0, -1),
                                                S1Angle::Degrees(80))));
}

static void TestCellOps(S2LatLngRect const& r, S2Cell const& cell,
                        int level) {
  // Test the relationship between the given rectangle and cell:
  // 0 == no intersection, 1 == MayIntersect, 2 == Intersects,
  // 3 == Vertex Containment, 4 == Contains

  bool vertex_contained = false;
  for (int i = 0; i < 4; ++i) {
    if (r.Contains(cell.GetVertexRaw(i)) ||
        (!r.is_empty() && cell.Contains(r.GetVertex(i).ToPoint())))
      vertex_contained = true;
  }
  EXPECT_EQ(r.MayIntersect(cell), level >= 1);
  EXPECT_EQ(r.Intersects(cell), level >= 2);
  EXPECT_EQ(vertex_contained, level >= 3);
  EXPECT_EQ(r.Contains(cell), level >= 4);
}

TEST(S2LatLngRect, CellOps) {
  // Contains(S2Cell), MayIntersect(S2Cell), Intersects(S2Cell)

  // Special cases.
  TestCellOps(S2LatLngRect::Empty(), S2Cell::FromFacePosLevel(3, 0, 0), 0);
  TestCellOps(S2LatLngRect::Full(), S2Cell::FromFacePosLevel(2, 0, 0), 4);
  TestCellOps(S2LatLngRect::Full(), S2Cell::FromFacePosLevel(5, 0, 25), 4);

  // This rectangle includes the first quadrant of face 0.  It's expanded
  // slightly because cell bounding rectangles are slightly conservative.
  S2LatLngRect r4 = RectFromDegrees(-45.1, -45.1, 0.1, 0.1);
  TestCellOps(r4, S2Cell::FromFacePosLevel(0, 0, 0), 3);
  TestCellOps(r4, S2Cell::FromFacePosLevel(0, 0, 1), 4);
  TestCellOps(r4, S2Cell::FromFacePosLevel(1, 0, 1), 0);

  // This rectangle intersects the first quadrant of face 0.
  S2LatLngRect r5 = RectFromDegrees(-10, -45, 10, 0);
  TestCellOps(r5, S2Cell::FromFacePosLevel(0, 0, 0), 3);
  TestCellOps(r5, S2Cell::FromFacePosLevel(0, 0, 1), 3);
  TestCellOps(r5, S2Cell::FromFacePosLevel(1, 0, 1), 0);

  // Rectangle consisting of a single point.
  TestCellOps(RectFromDegrees(4, 4, 4, 4),
              S2Cell::FromFacePosLevel(0, 0, 0), 3);

  // Rectangles that intersect the bounding rectangle of a face
  // but not the face itself.
  TestCellOps(RectFromDegrees(41, -87, 42, -79),
              S2Cell::FromFacePosLevel(2, 0, 0), 1);
  TestCellOps(RectFromDegrees(-41, 160, -40, -160),
              S2Cell::FromFacePosLevel(5, 0, 0), 1);

  // This is the leaf cell at the top right hand corner of face 0.
  // It has two angles of 60 degrees and two of 120 degrees.
  S2Cell cell0tr(S2Point(1 + 1e-12, 1, 1));
  S2LatLngRect bound0tr = cell0tr.GetRectBound();
  S2LatLng v0(cell0tr.GetVertexRaw(0));
  TestCellOps(RectFromDegrees(v0.lat().degrees() - 1e-8,
                              v0.lng().degrees() - 1e-8,
                              v0.lat().degrees() - 2e-10,
                              v0.lng().degrees() + 1e-10),
              cell0tr, 1);

  // Rectangles that intersect a face but where no vertex of one region
  // is contained by the other region.  The first one passes through
  // a corner of one of the face cells.
  TestCellOps(RectFromDegrees(-37, -70, -36, -20),
              S2Cell::FromFacePosLevel(5, 0, 0), 2);

  // These two intersect like a diamond and a square.
  S2Cell cell202 = S2Cell::FromFacePosLevel(2, 0, 2);
  S2LatLngRect bound202 = cell202.GetRectBound();
  TestCellOps(RectFromDegrees(bound202.lo().lat().degrees() + 3,
                              bound202.lo().lng().degrees() + 3,
                              bound202.hi().lat().degrees() - 3,
                              bound202.hi().lng().degrees() - 3),
              cell202, 2);
}

TEST(S2LatLngRect, EncodeDecode) {
  S2LatLngRect r = RectFromDegrees(-20, -80, 10, 20);
  Encoder encoder;
  r.Encode(&encoder);
  Decoder decoder(encoder.base(), encoder.length());
  S2LatLngRect decoded_rect = S2LatLngRect::Empty();
  EXPECT_TRUE(decoded_rect.Decode(&decoder));
  EXPECT_EQ(r, decoded_rect);
}

TEST(S2LatLngRect, Area) {
  EXPECT_EQ(S2LatLngRect::Empty().Area(), 0.0);
  EXPECT_DOUBLE_EQ(S2LatLngRect::Full().Area(), 4 * M_PI);
  EXPECT_DOUBLE_EQ(RectFromDegrees(0, 0, 90, 90).Area(), M_PI / 2);
}

// Returns the minimum distance from X to the latitude line segment defined by
// the given latitude and longitude interval.
S1Angle GetDistance(const S2LatLng& x,
                    const S1Angle& lat,
                    const S1Interval& interval) {
  EXPECT_TRUE(x.is_valid());
  EXPECT_TRUE(interval.is_valid());

  // Is X inside the longitude interval?
  if (interval.Contains(x.lng().radians()))
    return (x.lat() - lat).abs();

  // Return the distance to the closer endpoint.
  return min(x.GetDistance(S2LatLng(lat, S1Angle::Radians(interval.lo()))),
             x.GetDistance(S2LatLng(lat, S1Angle::Radians(interval.hi()))));
}

static S1Angle BruteForceDistance(const S2LatLngRect& a,
                                  const S2LatLngRect& b) {
  if (a.Intersects(b))
    return S1Angle::Radians(0);

  // Compare every point in 'a' against every latitude edge and longitude edge
  // in 'b', and vice-versa, for a total of 16 point-vs-latitude-edge tests and
  // 16 point-vs-longitude-edge tests.
  S2LatLng pnt_a[4], pnt_b[4];
  pnt_a[0] = S2LatLng(a.lat_lo(), a.lng_lo());
  pnt_a[1] = S2LatLng(a.lat_lo(), a.lng_hi());
  pnt_a[2] = S2LatLng(a.lat_hi(), a.lng_hi());
  pnt_a[3] = S2LatLng(a.lat_hi(), a.lng_lo());
  pnt_b[0] = S2LatLng(b.lat_lo(), b.lng_lo());
  pnt_b[1] = S2LatLng(b.lat_lo(), b.lng_hi());
  pnt_b[2] = S2LatLng(b.lat_hi(), b.lng_hi());
  pnt_b[3] = S2LatLng(b.lat_hi(), b.lng_lo());

  // Make arrays containing the lo/hi latitudes and the lo/hi longitude edges.
  S1Angle lat_a[2] = { a.lat_lo(), a.lat_hi() };
  S1Angle lat_b[2] = { b.lat_lo(), b.lat_hi() };
  S2Point lng_edge_a[2][2] = { { pnt_a[0].ToPoint(), pnt_a[3].ToPoint() },
                               { pnt_a[1].ToPoint(), pnt_a[2].ToPoint() } };
  S2Point lng_edge_b[2][2] = { { pnt_b[0].ToPoint(), pnt_b[3].ToPoint() },
                               { pnt_b[1].ToPoint(), pnt_b[2].ToPoint() } };

  S1Angle min_distance = S1Angle::Degrees(180.0);
  for (int i = 0; i < 4; ++i) {
    // For each point in a and b.
    const S2LatLng& current_a = pnt_a[i];
    const S2LatLng& current_b = pnt_b[i];

    for (int j = 0; j < 2; ++j) {
      // Get distances to latitude and longitude edges.
      S1Angle a_to_lat = GetDistance(current_a, lat_b[j], b.lng());
      S1Angle b_to_lat = GetDistance(current_b, lat_a[j], a.lng());
      S1Angle a_to_lng = S2EdgeUtil::GetDistance(
          current_a.ToPoint(), lng_edge_b[j][0], lng_edge_b[j][1]);
      S1Angle b_to_lng = S2EdgeUtil::GetDistance(
          current_b.ToPoint(), lng_edge_a[j][0], lng_edge_a[j][1]);

      min_distance = min(min_distance,
          min(a_to_lat, min(b_to_lat, min(a_to_lng, b_to_lng))));
    }
  }
  return min_distance;
}

static S1Angle BruteForceRectPointDistance(const S2LatLngRect& a,
                                           const S2LatLng& b) {
  if (a.Contains(b)) {
    return S1Angle::Radians(0);
  }

  S1Angle b_to_lo_lat = GetDistance(b, a.lat_lo(), a.lng());
  S1Angle b_to_hi_lat = GetDistance(b, a.lat_hi(), a.lng());
  S1Angle b_to_lo_lng = S2EdgeUtil::GetDistance(
      b.ToPoint(),
      S2LatLng(a.lat_lo(), a.lng_lo()).ToPoint(),
      S2LatLng(a.lat_hi(), a.lng_lo()).ToPoint());
  S1Angle b_to_hi_lng = S2EdgeUtil::GetDistance(
      b.ToPoint(),
      S2LatLng(a.lat_lo(), a.lng_hi()).ToPoint(),
      S2LatLng(a.lat_hi(), a.lng_hi()).ToPoint());
  return min(b_to_lo_lat, min(b_to_hi_lat, min(b_to_lo_lng, b_to_hi_lng)));
}

// This method verifies a.GetDistance(b) by comparing its result against a
// brute-force implementation. The correctness of the brute-force version is
// much easier to verify by inspection.
static void VerifyGetDistance(const S2LatLngRect& a, const S2LatLngRect& b) {
  S1Angle distance1 = BruteForceDistance(a, b);
  S1Angle distance2 = a.GetDistance(b);
  EXPECT_NEAR(distance1.radians() - distance2.radians(), 0, 1e-10)
      << a << ":" << b;
}

static S2LatLngRect PointRectFromDegrees(double lat, double lng) {
  return S2LatLngRect::FromPoint(
      S2LatLng::FromDegrees(lat, lng).Normalized());
}

// This method verifies a.GetDistance(b), where b is a S2LatLng, by comparing
// its result against a.GetDistance(c), c being the point rectangle created
// from b.
static void VerifyGetRectPointDistance(
    const S2LatLngRect& a, const S2LatLng& p) {
  S1Angle distance1 = BruteForceRectPointDistance(a, p.Normalized());
  S1Angle distance2 = a.GetDistance(p.Normalized());
  EXPECT_NEAR(fabs(distance1.radians() - distance2.radians()), 0, 1e-10)
      << a << ":" << p;
}

TEST(S2LatLngRect, GetDistanceOverlapping) {
  // Check pairs of rectangles that overlap: (should all return 0):
  S2LatLngRect a = RectFromDegrees(0, 0, 2, 2);
  S2LatLngRect b = PointRectFromDegrees(0, 0);
  EXPECT_EQ(S1Angle::Radians(0), a.GetDistance(a));
  EXPECT_EQ(S1Angle::Radians(0), a.GetDistance(b));
  EXPECT_EQ(S1Angle::Radians(0), b.GetDistance(b));
  EXPECT_EQ(S1Angle::Radians(0), a.GetDistance(S2LatLng::FromDegrees(0, 0)));
  EXPECT_EQ(S1Angle::Radians(0), a.GetDistance(RectFromDegrees(0, 1, 2, 3)));
  EXPECT_EQ(S1Angle::Radians(0), a.GetDistance(RectFromDegrees(0, 2, 2, 4)));
  EXPECT_EQ(S1Angle::Radians(0), a.GetDistance(RectFromDegrees(1, 0, 3, 2)));
  EXPECT_EQ(S1Angle::Radians(0), a.GetDistance(RectFromDegrees(2, 0, 4, 2)));
  EXPECT_EQ(S1Angle::Radians(0), a.GetDistance(RectFromDegrees(1, 1, 3, 3)));
  EXPECT_EQ(S1Angle::Radians(0), a.GetDistance(RectFromDegrees(2, 2, 4, 4)));
}

TEST(S2LatLngRect, GetDistanceRectVsPoint) {
  // Rect that spans 180.
  S2LatLngRect a = RectFromDegrees(-1, -1, 2, 1);
  VerifyGetDistance(a, PointRectFromDegrees(-2, -1));
  VerifyGetDistance(a, PointRectFromDegrees(1, 2));

  VerifyGetDistance(PointRectFromDegrees(-2, -1), a);
  VerifyGetDistance(PointRectFromDegrees(1, 2), a);

  VerifyGetRectPointDistance(a, S2LatLng::FromDegrees(-2, -1));
  VerifyGetRectPointDistance(a, S2LatLng::FromDegrees(1, 2));

  // Tests near the north pole.
  S2LatLngRect b = RectFromDegrees(86, 0, 88, 2);
  VerifyGetDistance(b, PointRectFromDegrees(87, 3));
  VerifyGetDistance(b, PointRectFromDegrees(87, -1));
  VerifyGetDistance(b, PointRectFromDegrees(89, 1));
  VerifyGetDistance(b, PointRectFromDegrees(89, 181));
  VerifyGetDistance(b, PointRectFromDegrees(85, 1));
  VerifyGetDistance(b, PointRectFromDegrees(85, 181));
  VerifyGetDistance(b, PointRectFromDegrees(90, 0));

  VerifyGetDistance(PointRectFromDegrees(87, 3), b);
  VerifyGetDistance(PointRectFromDegrees(87, -1), b);
  VerifyGetDistance(PointRectFromDegrees(89, 1), b);
  VerifyGetDistance(PointRectFromDegrees(89, 181), b);
  VerifyGetDistance(PointRectFromDegrees(85, 1), b);
  VerifyGetDistance(PointRectFromDegrees(85, 181), b);
  VerifyGetDistance(PointRectFromDegrees(90, 0), b);

  VerifyGetRectPointDistance(b, S2LatLng::FromDegrees(87, 3));
  VerifyGetRectPointDistance(b, S2LatLng::FromDegrees(87, -1));
  VerifyGetRectPointDistance(b, S2LatLng::FromDegrees(89, 1));
  VerifyGetRectPointDistance(b, S2LatLng::FromDegrees(89, 181));
  VerifyGetRectPointDistance(b, S2LatLng::FromDegrees(85, 1));
  VerifyGetRectPointDistance(b, S2LatLng::FromDegrees(85, 181));
  VerifyGetRectPointDistance(b, S2LatLng::FromDegrees(90, 0));

  // Rect that touches the north pole.
  S2LatLngRect c = RectFromDegrees(88, 0, 90, 2);
  VerifyGetDistance(c, PointRectFromDegrees(89, 3));
  VerifyGetDistance(c, PointRectFromDegrees(89, 90));
  VerifyGetDistance(c, PointRectFromDegrees(89, 181));
  VerifyGetDistance(PointRectFromDegrees(89, 3), c);
  VerifyGetDistance(PointRectFromDegrees(89, 90), c);
  VerifyGetDistance(PointRectFromDegrees(89, 181), c);
}

TEST(S2LatLngRect, GetDistanceRectVsRect) {
  // Rect that spans 180.
  S2LatLngRect a = RectFromDegrees(-1, -1, 2, 1);
  VerifyGetDistance(a, RectFromDegrees(0, 2, 1, 3));
  VerifyGetDistance(a, RectFromDegrees(-2, -3, -1, -2));

  // Tests near the south pole.
  S2LatLngRect b = RectFromDegrees(-87, 0, -85, 3);
  VerifyGetDistance(b, RectFromDegrees(-89, 1, -88, 2));
  VerifyGetDistance(b, RectFromDegrees(-84, 1, -83, 2));
  VerifyGetDistance(b, RectFromDegrees(-88, 90, -86, 91));
  VerifyGetDistance(b, RectFromDegrees(-84, -91, -83, -90));
  VerifyGetDistance(b, RectFromDegrees(-90, 181, -89, 182));
  VerifyGetDistance(b, RectFromDegrees(-84, 181, -83, 182));
}

TEST(S2LatLngRect, GetDistanceRandomPairs) {
  // Test random pairs.
  for (int i = 0; i < 10000; ++i) {
    S2LatLngRect a =
        S2LatLngRect::FromPointPair(S2LatLng(S2Testing::RandomPoint()),
                                    S2LatLng(S2Testing::RandomPoint()));
    S2LatLngRect b =
        S2LatLngRect::FromPointPair(S2LatLng(S2Testing::RandomPoint()),
                                    S2LatLng(S2Testing::RandomPoint()));
    VerifyGetDistance(a, b);


    S2LatLng c(S2Testing::RandomPoint());
    VerifyGetRectPointDistance(a, c);
    VerifyGetRectPointDistance(b, c);
  }
}

// This function assumes that GetDirectedHausdorffDistance() always returns
// a distance from some point in a to b. So the function mainly tests whether
// the returned distance is large enough, and only does a weak test on whether
// it is small enough.
static void VerifyGetDirectedHausdorffDistance(const S2LatLngRect& a,
                                               const S2LatLngRect& b) {
  S1Angle hausdorff_distance = a.GetDirectedHausdorffDistance(b);

  static const double kResolution = 0.1;
  // Record the max sample distance as well as the sample point realizing the
  // max for easier debugging.
  S1Angle max_distance;
  double lat_max, lng_max;

  int sample_size_on_lat =
      static_cast<int>(a.lat().GetLength() / kResolution) + 1;
  int sample_size_on_lng =
      static_cast<int>(a.lng().GetLength() / kResolution) + 1;
  double delta_on_lat = a.lat().GetLength() / sample_size_on_lat;
  double delta_on_lng = a.lng().GetLength() / sample_size_on_lng;

  double lng = a.lng().lo();
  for (int i = 0; i <= sample_size_on_lng; ++i, lng += delta_on_lng) {
    double lat = a.lat().lo();
    for (int j = 0; j <= sample_size_on_lat; ++j, lat += delta_on_lat) {
      S2LatLng latlng = S2LatLng::FromRadians(lat, lng).Normalized();
      S1Angle distance_to_b = b.GetDistance(latlng);

      if (distance_to_b >= max_distance) {
        max_distance = distance_to_b;
        lat_max = lat;
        lng_max = lng;
      }
    }
  }

  EXPECT_LE(max_distance.radians(), hausdorff_distance.radians() + 1e-10)
      << a << ":" << b;
  EXPECT_GE(max_distance.radians(), hausdorff_distance.radians() - kResolution)
      << a << ":" << b;
}


TEST(S2LatLngRect, GetDirectedHausdorffDistanceRandomPairs) {
  // Test random pairs.
  for (int i = 0; i < 5000; ++i) {
    S2LatLngRect a =
        S2LatLngRect::FromPointPair(S2LatLng(S2Testing::RandomPoint()),
                                    S2LatLng(S2Testing::RandomPoint()));
    S2LatLngRect b =
        S2LatLngRect::FromPointPair(S2LatLng(S2Testing::RandomPoint()),
                                    S2LatLng(S2Testing::RandomPoint()));
    // a and b are *minimum* bounding rectangles of two random points, in
    // particular, their Voronoi diagrams are always of the same topology. We
    // take the "complements" of a and b for more thorough testing.
    S2LatLngRect a2(a.lat(), a.lng().Complement());
    S2LatLngRect b2(b.lat(), b.lng().Complement());

    VerifyGetDirectedHausdorffDistance(a, b);
    VerifyGetDirectedHausdorffDistance(b, a);

    VerifyGetDirectedHausdorffDistance(a, b2);
    VerifyGetDirectedHausdorffDistance(b2, a);

    VerifyGetDirectedHausdorffDistance(a2, b);
    VerifyGetDirectedHausdorffDistance(b, a2);

    VerifyGetDirectedHausdorffDistance(a2, b2);
    VerifyGetDirectedHausdorffDistance(b2, a2);
  }
}

TEST(S2LatLngRect, GetDirectedHausdorffDistanceContained) {
  // Caller rect is contained in callee rect. Should return 0.
  S2LatLngRect a = RectFromDegrees(-10, 20, -5, 90);
  EXPECT_EQ(S1Angle::Radians(0),
            a.GetDirectedHausdorffDistance(RectFromDegrees(-10, 20, -5, 90)));
  EXPECT_EQ(S1Angle::Radians(0),
            a.GetDirectedHausdorffDistance(RectFromDegrees(-10, 19, -5, 91)));
  EXPECT_EQ(S1Angle::Radians(0),
            a.GetDirectedHausdorffDistance(RectFromDegrees(-11, 20, -4, 90)));
  EXPECT_EQ(S1Angle::Radians(0),
            a.GetDirectedHausdorffDistance(RectFromDegrees(-11, 19, -4, 91)));
}

TEST(S2LatLngRect, GetDirectHausdorffDistancePointToRect) {
  // The Hausdorff distance from a point to a rect should be the same as its
  // distance to the rect.
  S2LatLngRect a1 = PointRectFromDegrees(5, 8);
  S2LatLngRect a2 = PointRectFromDegrees(90, 10);  // north pole

  S2LatLngRect b = RectFromDegrees(-85, -50, -80, 10);
  EXPECT_DOUBLE_EQ(a1.GetDirectedHausdorffDistance(b).radians(),
                   a1.GetDistance(b).radians());
  EXPECT_DOUBLE_EQ(a2.GetDirectedHausdorffDistance(b).radians(),
                   a2.GetDistance(b).radians());

  b = RectFromDegrees(4, -10, 80, 10);
  EXPECT_DOUBLE_EQ(a1.GetDirectedHausdorffDistance(b).radians(),
                   a1.GetDistance(b).radians());
  EXPECT_DOUBLE_EQ(a2.GetDirectedHausdorffDistance(b).radians(),
                   a2.GetDistance(b).radians());

  b = RectFromDegrees(70, 170, 80, -170);
  EXPECT_DOUBLE_EQ(a1.GetDirectedHausdorffDistance(b).radians(),
                   a1.GetDistance(b).radians());
  EXPECT_DOUBLE_EQ(a2.GetDirectedHausdorffDistance(b).radians(),
                   a2.GetDistance(b).radians());
}

TEST(S2LatLngRect, GetDirectedHausdorffDistanceRectToPoint) {
  S2LatLngRect a = RectFromDegrees(1, -8, 10, 20);
  VerifyGetDirectedHausdorffDistance(a, PointRectFromDegrees(5, 8));
  VerifyGetDirectedHausdorffDistance(a, PointRectFromDegrees(-6, -100));
  // south pole
  VerifyGetDirectedHausdorffDistance(a, PointRectFromDegrees(-90, -20));
  // north pole
  VerifyGetDirectedHausdorffDistance(a, PointRectFromDegrees(90, 0));
}

TEST(S2LatLngRect, GetDirectedHausdorffDistanceRectToRectNearPole) {
  // Tests near south pole.
  S2LatLngRect a = RectFromDegrees(-87, 0, -85, 3);
  VerifyGetDirectedHausdorffDistance(a, RectFromDegrees(-89, 1, -88, 2));
  VerifyGetDirectedHausdorffDistance(a, RectFromDegrees(-84, 1, -83, 2));
  VerifyGetDirectedHausdorffDistance(a, RectFromDegrees(-88, 90, -86, 91));
  VerifyGetDirectedHausdorffDistance(a, RectFromDegrees(-84, -91, -83, -90));
  VerifyGetDirectedHausdorffDistance(a, RectFromDegrees(-90, 181, -89, 182));
  VerifyGetDirectedHausdorffDistance(a, RectFromDegrees(-84, 181, -83, 182));
}

TEST(S2LatLngRect, GetDirectedHausdorffDistanceRectToRectDegenerateCases) {
  // Rectangles that contain poles.
  VerifyGetDirectedHausdorffDistance(
      RectFromDegrees(0, 10, 90, 20), RectFromDegrees(-4, -10, 4, 0));
  VerifyGetDirectedHausdorffDistance(
      RectFromDegrees(-4, -10, 4, 0), RectFromDegrees(0, 10, 90, 20));

  // Two rectangles share same or complement longitudinal intervals.
  S2LatLngRect a = RectFromDegrees(-50, -10, 50, 10);
  S2LatLngRect b = RectFromDegrees(30, -10, 60, 10);
  VerifyGetDirectedHausdorffDistance(a, b);
  S2LatLngRect c(a.lat(), a.lng().Complement());
  VerifyGetDirectedHausdorffDistance(c, b);

  // rectangle a touches b_opposite_lng.
  VerifyGetDirectedHausdorffDistance(
      RectFromDegrees(10, 170, 30, 180), RectFromDegrees(-50, -10, 50, 10));
  VerifyGetDirectedHausdorffDistance(
      RectFromDegrees(10, -180, 30, -170), RectFromDegrees(-50, -10, 50, 10));

  // rectangle b's Voronoi diagram is degenerate (lng interval spans 180
  // degrees), and a touches the degenerate Voronoi vertex.
  VerifyGetDirectedHausdorffDistance(
      RectFromDegrees(-30, 170, 30, 180), RectFromDegrees(-10, -90, 10, 90));
  VerifyGetDirectedHausdorffDistance(
      RectFromDegrees(-30, -180, 30, -170), RectFromDegrees(-10, -90, 10, 90));

  // rectangle a touches a voronoi vertex of rectangle b.
  VerifyGetDirectedHausdorffDistance(
      RectFromDegrees(-20, 105, 20, 110), RectFromDegrees(-30, 5, 30, 15));
  VerifyGetDirectedHausdorffDistance(
      RectFromDegrees(-20, 95, 20, 105), RectFromDegrees(-30, 5, 30, 15));
}
