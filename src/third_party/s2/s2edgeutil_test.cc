// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2edgeutil.h"

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <string>
using std::string;

#include <vector>
using std::vector;


#include "base/commandlineflags.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "testing/base/public/benchmark.h"
#include "testing/base/public/gunit.h"
#include "s2cap.h"
#include "s2polyline.h"
#include "s2testing.h"

DECLARE_bool(s2debug);

namespace {

const int kDegen = -2;
void CompareResult(int actual, int expected) {
  // HACK ALERT: RobustCrossing() is allowed to return 0 or -1 if either edge
  // is degenerate.  We use the value kDegen to represent this possibility.
  if (expected == kDegen) {
    EXPECT_LE(actual, 0);
  } else {
    EXPECT_EQ(expected, actual);
  }
}

void TestCrossing(S2Point a, S2Point b, S2Point c, S2Point d,
                  int robust, bool edge_or_vertex, bool simple) {
  a = a.Normalize();
  b = b.Normalize();
  c = c.Normalize();
  d = d.Normalize();
  CompareResult(S2EdgeUtil::RobustCrossing(a, b, c, d), robust);
  if (simple) {
    EXPECT_EQ(robust > 0, S2EdgeUtil::SimpleCrossing(a, b, c, d));
  }
  S2EdgeUtil::EdgeCrosser crosser(&a, &b, &c);
  CompareResult(crosser.RobustCrossing(&d), robust);
  CompareResult(crosser.RobustCrossing(&c), robust);

  EXPECT_EQ(edge_or_vertex, S2EdgeUtil::EdgeOrVertexCrossing(a, b, c, d));
  EXPECT_EQ(edge_or_vertex, crosser.EdgeOrVertexCrossing(&d));
  EXPECT_EQ(edge_or_vertex, crosser.EdgeOrVertexCrossing(&c));
}

void TestCrossings(S2Point a, S2Point b, S2Point c, S2Point d,
                   int robust, bool edge_or_vertex, bool simple) {
  TestCrossing(a, b, c, d, robust, edge_or_vertex, simple);
  TestCrossing(b, a, c, d, robust, edge_or_vertex, simple);
  TestCrossing(a, b, d, c, robust, edge_or_vertex, simple);
  TestCrossing(b, a, d, c, robust, edge_or_vertex, simple);
  TestCrossing(a, a, c, d, kDegen, 0, false);
  TestCrossing(a, b, c, c, kDegen, 0, false);
  TestCrossing(a, b, a, b, 0, 1, false);
  TestCrossing(c, d, a, b, robust, edge_or_vertex ^ (robust == 0), simple);
}

TEST(S2EdgeUtil, Crossings) {
  // The real tests of edge crossings are in s2{loop,polygon}_unittest,
  // but we do a few simple tests here.

  // Two regular edges that cross.
  TestCrossings(S2Point(1, 2, 1), S2Point(1, -3, 0.5),
                S2Point(1, -0.5, -3), S2Point(0.1, 0.5, 3), 1, true, true);

  // Two regular edges that cross antipodal points.
  TestCrossings(S2Point(1, 2, 1), S2Point(1, -3, 0.5),
                S2Point(-1, 0.5, 3), S2Point(-0.1, -0.5, -3), -1, false, true);

  // Two edges on the same great circle.
  TestCrossings(S2Point(0, 0, -1), S2Point(0, 1, 0),
                S2Point(0, 1, 1), S2Point(0, 0, 1), -1, false, true);

  // Two edges that cross where one vertex is S2::Origin().
  TestCrossings(S2Point(1, 0, 0), S2::Origin(),
                S2Point(1, -0.1, 1), S2Point(1, 1, -0.1), 1, true, true);

  // Two edges that cross antipodal points where one vertex is S2::Origin().
  TestCrossings(S2Point(1, 0, 0), S2Point(0, 1, 0),
                S2Point(0, 0, -1), S2Point(-1, -1, 1), -1, false, true);

  // Two edges that share an endpoint.  The Ortho() direction is (-4,0,2),
  // and edge CD is further CCW around (2,3,4) than AB.
  TestCrossings(S2Point(2, 3, 4), S2Point(-1, 2, 5),
                S2Point(7, -2, 3), S2Point(2, 3, 4), 0, false, true);

  // Two edges that barely cross each other near the middle of one edge.  The
  // edge AB is approximately in the x=y plane, while CD is approximately
  // perpendicular to it and ends exactly at the x=y plane.
  TestCrossings(S2Point(1, 1, 1), S2Point(1, nextafter(1, 0), -1),
                S2Point(11, -12, -1), S2Point(10, 10, 1), 1, true, false);

  // In this version, the edges are separated by a distance of about 1e-15.
  TestCrossings(S2Point(1, 1, 1), S2Point(1, nextafter(1, 2), -1),
                S2Point(1, -1, 0), S2Point(1, 1, 0), -1, false, false);

  // Two edges that barely cross each other near the end of both edges.  This
  // example cannot be handled using regular double-precision arithmetic due
  // to floating-point underflow.
  TestCrossings(S2Point(0, 0, 1), S2Point(2, -1e-323, 1),
                S2Point(1, -1, 1), S2Point(1e-323, 0, 1), 1, true, false);

  // In this version, the edges are separated by a distance of about 1e-640.
  TestCrossings(S2Point(0, 0, 1), S2Point(2, 1e-323, 1),
                S2Point(1, -1, 1), S2Point(1e-323, 0, 1), -1, false, false);

  // Two edges that barely cross each other near the middle of one edge.
  // Computing the exact determinant of some of the triangles in this test
  // requires more than 2000 bits of precision.
  TestCrossings(S2Point(1, -1e-323, -1e-323), S2Point(1e-323, 1, 1e-323),
                S2Point(1, -1, 1e-323), S2Point(1, 1, 0),
                1, true, false);

  // In this version, the edges are separated by a distance of about 1e-640.
  TestCrossings(S2Point(1, 1e-323, -1e-323), S2Point(-1e-323, 1, 1e-323),
                S2Point(1, -1, 1e-323), S2Point(1, 1, 0),
                -1, false, false);
}

typedef bool CrossingFunction(S2Point const&, S2Point const&,
                              S2Point const&, S2Point const&);
void BenchmarkCrossing(CrossingFunction crossing, int iters) {
  StopBenchmarkTiming();
  // We want to avoid cache effects, so kNumPoints should be small enough so
  // that the points can be in L1 cache.  sizeof(S2Point) == 24, so 400 will
  // only take ~9KiB of 64KiB L1 cache.
  static const int kNumPoints = 400;
  vector<S2Point> p(kNumPoints);
  for (int i = 0; i < kNumPoints; ++i)
    p[i] = S2Testing::RandomPoint();
  StartBenchmarkTiming();

  int num_crossings = 0;
  for (int i = 0; i < iters; ++i) {
    const S2Point& a = p[(i + 0) % kNumPoints];
    const S2Point& b = p[(i + 1) % kNumPoints];
    const S2Point& c = p[(i + 2) % kNumPoints];
    const S2Point& d = p[(i + 3) % kNumPoints];

    if (crossing(a, b, c, d))
      ++num_crossings;
  }
  VLOG(5) << "Fraction crossing = " << 1.0 * num_crossings / iters;
  VLOG(5) << "iters: " << iters;
}

void BM_EdgeOrVertexCrossing(int iters) {
  BenchmarkCrossing(&S2EdgeUtil::EdgeOrVertexCrossing, iters);
}
BENCHMARK(BM_EdgeOrVertexCrossing);

bool RobustCrossingBool(S2Point const& a, S2Point const& b,
                        S2Point const& c, S2Point const& d) {
  return S2EdgeUtil::RobustCrossing(a, b, c, d) > 0;
}
void BM_RobustCrossing(int iters) {
  BenchmarkCrossing(&RobustCrossingBool, iters);
}
BENCHMARK(BM_RobustCrossing);

S2LatLngRect GetEdgeBound(double x1, double y1, double z1,
                          double x2, double y2, double z2) {
  S2EdgeUtil::RectBounder bounder;
  S2Point p1 = S2Point(x1, y1, z1).Normalize();
  S2Point p2 = S2Point(x2, y2, z2).Normalize();
  bounder.AddPoint(&p1);
  bounder.AddPoint(&p2);
  return bounder.GetBound();
}

TEST(S2EdgeUtil, RectBounder) {
  // Check cases where min/max latitude is not at a vertex.
  EXPECT_DOUBLE_EQ(M_PI_4,
                   GetEdgeBound(1,1,1, 1,-1,1).lat().hi());     // Max, CW
  EXPECT_DOUBLE_EQ(M_PI_4,
                   GetEdgeBound(1,-1,1, 1,1,1).lat().hi());     // Max, CCW
  EXPECT_DOUBLE_EQ(-M_PI_4,
                   GetEdgeBound(1,-1,-1, -1,-1,-1).lat().lo()); // Min, CW
  EXPECT_DOUBLE_EQ(-M_PI_4,
                   GetEdgeBound(-1,1,-1, -1,-1,-1).lat().lo()); // Min, CCW

  // Check cases where the edge passes through one of the poles.
  EXPECT_DOUBLE_EQ(M_PI_2, GetEdgeBound(.3,.4,1, -.3,-.4,1).lat().hi());
  EXPECT_DOUBLE_EQ(-M_PI_2, GetEdgeBound(.3,.4,-1, -.3,-.4,-1).lat().lo());

  // Check cases where the min/max latitude is attained at a vertex.
  static double const kCubeLat = asin(sqrt(1./3));  // 35.26 degrees
  EXPECT_TRUE(GetEdgeBound(1,1,1, 1,-1,-1).lat().
              ApproxEquals(R1Interval(-kCubeLat, kCubeLat)));
  EXPECT_TRUE(GetEdgeBound(1,-1,1, 1,1,-1).lat().
              ApproxEquals(R1Interval(-kCubeLat, kCubeLat)));
}

TEST(S2EdgeUtil, LongitudePruner) {
  S2EdgeUtil::LongitudePruner pruner1(S1Interval(0.75 * M_PI, -0.75 * M_PI),
                                      S2Point(0, 1, 2));
  EXPECT_FALSE(pruner1.Intersects(S2Point(1, 1, 3)));
  EXPECT_TRUE(pruner1.Intersects(S2Point(-1 - 1e-15, -1, 0)));
  EXPECT_TRUE(pruner1.Intersects(S2Point(-1, 0, 0)));
  EXPECT_TRUE(pruner1.Intersects(S2Point(-1, 0, 0)));
  EXPECT_TRUE(pruner1.Intersects(S2Point(1, -1, 8)));
  EXPECT_FALSE(pruner1.Intersects(S2Point(1, 0, -2)));
  EXPECT_TRUE(pruner1.Intersects(S2Point(-1, -1e-15, 0)));

  S2EdgeUtil::LongitudePruner pruner2(S1Interval(0.25 * M_PI, 0.25 * M_PI),
                                      S2Point(1, 0, 0));
  EXPECT_FALSE(pruner2.Intersects(S2Point(2, 1, 2)));
  EXPECT_TRUE(pruner2.Intersects(S2Point(1, 2, 3)));
  EXPECT_FALSE(pruner2.Intersects(S2Point(0, 1, 4)));
  EXPECT_FALSE(pruner2.Intersects(S2Point(-1e-15, -1, -1)));
}

void TestWedge(S2Point a0, S2Point ab1, S2Point a2, S2Point b0, S2Point b2,
               bool contains, bool intersects,
               S2EdgeUtil::WedgeRelation wedge_relation) {
  a0 = a0.Normalize();
  ab1 = ab1.Normalize();
  a2 = a2.Normalize();
  b0 = b0.Normalize();
  b2 = b2.Normalize();
  EXPECT_EQ(contains, S2EdgeUtil::WedgeContains(a0, ab1, a2, b0, b2));
  EXPECT_EQ(intersects, S2EdgeUtil::WedgeIntersects(a0, ab1, a2, b0, b2));
  EXPECT_EQ(wedge_relation, S2EdgeUtil::GetWedgeRelation(a0, ab1, a2, b0, b2));
}

TEST(S2EdgeUtil, Wedges) {
  // For simplicity, all of these tests use an origin of (0, 0, 1).
  // This shouldn't matter as long as the lower-level primitives are
  // implemented correctly.

  // Intersection in one wedge.
  TestWedge(S2Point(-1, 0, 10), S2Point(0, 0, 1), S2Point(1, 2, 10),
            S2Point(0, 1, 10), S2Point(1, -2, 10),
            false, true, S2EdgeUtil::WEDGE_PROPERLY_OVERLAPS);
  // Intersection in two wedges.
  TestWedge(S2Point(-1, -1, 10), S2Point(0, 0, 1), S2Point(1, -1, 10),
            S2Point(1, 0, 10), S2Point(-1, 1, 10),
            false, true, S2EdgeUtil::WEDGE_PROPERLY_OVERLAPS);

  // Normal containment.
  TestWedge(S2Point(-1, -1, 10), S2Point(0, 0, 1), S2Point(1, -1, 10),
            S2Point(-1, 0, 10), S2Point(1, 0, 10),
            true, true, S2EdgeUtil::WEDGE_PROPERLY_CONTAINS);
  // Containment with equality on one side.
  TestWedge(S2Point(2, 1, 10), S2Point(0, 0, 1), S2Point(-1, -1, 10),
            S2Point(2, 1, 10), S2Point(1, -5, 10),
            true, true, S2EdgeUtil::WEDGE_PROPERLY_CONTAINS);
  // Containment with equality on the other side.
  TestWedge(S2Point(2, 1, 10), S2Point(0, 0, 1), S2Point(-1, -1, 10),
            S2Point(1, -2, 10), S2Point(-1, -1, 10),
            true, true, S2EdgeUtil::WEDGE_PROPERLY_CONTAINS);

  // Containment with equality on both sides.
  TestWedge(S2Point(-2, 3, 10), S2Point(0, 0, 1), S2Point(4, -5, 10),
            S2Point(-2, 3, 10), S2Point(4, -5, 10),
            true, true, S2EdgeUtil::WEDGE_EQUALS);

  // Disjoint with equality on one side.
  TestWedge(S2Point(-2, 3, 10), S2Point(0, 0, 1), S2Point(4, -5, 10),
            S2Point(4, -5, 10), S2Point(-2, -3, 10),
            false, false, S2EdgeUtil::WEDGE_IS_DISJOINT);
  // Disjoint with equality on the other side.
  TestWedge(S2Point(-2, 3, 10), S2Point(0, 0, 1), S2Point(0, 5, 10),
            S2Point(4, -5, 10), S2Point(-2, 3, 10),
            false, false, S2EdgeUtil::WEDGE_IS_DISJOINT);
  // Disjoint with equality on both sides.
  TestWedge(S2Point(-2, 3, 10), S2Point(0, 0, 1), S2Point(4, -5, 10),
            S2Point(4, -5, 10), S2Point(-2, 3, 10),
            false, false, S2EdgeUtil::WEDGE_IS_DISJOINT);

  // B contains A with equality on one side.
  TestWedge(S2Point(2, 1, 10), S2Point(0, 0, 1), S2Point(1, -5, 10),
            S2Point(2, 1, 10), S2Point(-1, -1, 10),
            false, true, S2EdgeUtil::WEDGE_IS_PROPERLY_CONTAINED);
  // B contains A with equality on the other side.
  TestWedge(S2Point(2, 1, 10), S2Point(0, 0, 1), S2Point(1, -5, 10),
            S2Point(-2, 1, 10), S2Point(1, -5, 10),
            false, true, S2EdgeUtil::WEDGE_IS_PROPERLY_CONTAINED);
}

// Given a point X and an edge AB, check that the distance from X to AB is
// "distance_radians" and the closest point on AB is "expected_closest".
void CheckDistance(S2Point x, S2Point a, S2Point b,
                   double distance_radians, S2Point expected_closest) {
  x = x.Normalize();
  a = a.Normalize();
  b = b.Normalize();
  expected_closest = expected_closest.Normalize();
  EXPECT_DOUBLE_EQ(distance_radians,
                   S2EdgeUtil::GetDistance(x, a, b).radians());
  S2Point closest = S2EdgeUtil::GetClosestPoint(x, a, b);
  if (expected_closest == S2Point(0, 0, 0)) {
    // This special value says that the result should be A or B.
    EXPECT_TRUE(closest == a || closest == b);
  } else {
    EXPECT_TRUE(S2::ApproxEquals(closest, expected_closest));
  }
}

TEST(S2EdgeUtil, Distance) {
  CheckDistance(S2Point(1, 0, 0), S2Point(1, 0, 0), S2Point(0, 1, 0),
                0, S2Point(1, 0, 0));
  CheckDistance(S2Point(0, 1, 0), S2Point(1, 0, 0), S2Point(0, 1, 0),
                0, S2Point(0, 1, 0));
  CheckDistance(S2Point(1, 3, 0), S2Point(1, 0, 0), S2Point(0, 1, 0),
                0, S2Point(1, 3, 0));
  CheckDistance(S2Point(0, 0, 1), S2Point(1, 0, 0), S2Point(0, 1, 0),
                M_PI_2, S2Point(1, 0, 0));
  CheckDistance(S2Point(0, 0, -1), S2Point(1, 0, 0), S2Point(0, 1, 0),
                M_PI_2, S2Point(1, 0, 0));
  CheckDistance(S2Point(-1, -1, 0), S2Point(1, 0, 0), S2Point(0, 1, 0),
                0.75 * M_PI, S2Point(0, 0, 0));

  CheckDistance(S2Point(0, 1, 0), S2Point(1, 0, 0), S2Point(1, 1, 0),
                M_PI_4, S2Point(1, 1, 0));
  CheckDistance(S2Point(0, -1, 0), S2Point(1, 0, 0), S2Point(1, 1, 0),
                M_PI_2, S2Point(1, 0, 0));

  CheckDistance(S2Point(0, -1, 0), S2Point(1, 0, 0), S2Point(-1, 1, 0),
                M_PI_2, S2Point(1, 0, 0));
  CheckDistance(S2Point(-1, -1, 0), S2Point(1, 0, 0), S2Point(-1, 1, 0),
                M_PI_2, S2Point(-1, 1, 0));

  CheckDistance(S2Point(1, 1, 1), S2Point(1, 0, 0), S2Point(0, 1, 0),
                asin(sqrt(1./3)), S2Point(1, 1, 0));
  CheckDistance(S2Point(1, 1, -1), S2Point(1, 0, 0), S2Point(0, 1, 0),
                asin(sqrt(1./3)), S2Point(1, 1, 0));

  CheckDistance(S2Point(-1, 0, 0), S2Point(1, 1, 0), S2Point(1, 1, 0),
                0.75 * M_PI, S2Point(1, 1, 0));
  CheckDistance(S2Point(0, 0, -1), S2Point(1, 1, 0), S2Point(1, 1, 0),
                M_PI_2, S2Point(1, 1, 0));
  CheckDistance(S2Point(-1, 0, 0), S2Point(1, 0, 0), S2Point(1, 0, 0),
                M_PI, S2Point(1, 0, 0));
}

void CheckInterpolate(double t, S2Point a, S2Point b, S2Point expected) {
  a = a.Normalize();
  b = b.Normalize();
  expected = expected.Normalize();
  S2Point actual = S2EdgeUtil::Interpolate(t, a, b);

  // We allow a bit more than the usual 1e-15 error tolerance because
  // Interpolate() uses trig functions.
  EXPECT_TRUE(S2::ApproxEquals(expected, actual, 3e-15))
      << "Expected: " << expected << ", actual: " << actual;
}

TEST(S2EdgeUtil, Interpolate) {
  // A zero-length edge.
  CheckInterpolate(0, S2Point(1, 0, 0), S2Point(1, 0, 0), S2Point(1, 0, 0));
  CheckInterpolate(1, S2Point(1, 0, 0), S2Point(1, 0, 0), S2Point(1, 0, 0));

  // Start, end, and middle of a medium-length edge.
  CheckInterpolate(0, S2Point(1, 0, 0), S2Point(0, 1, 0), S2Point(1, 0, 0));
  CheckInterpolate(1, S2Point(1, 0, 0), S2Point(0, 1, 0), S2Point(0, 1, 0));
  CheckInterpolate(0.5, S2Point(1, 0, 0), S2Point(0, 1, 0), S2Point(1, 1, 0));

  // Test that interpolation is done using distances on the sphere rather than
  // linear distances.
  CheckInterpolate(1./3, S2Point(1, 0, 0), S2Point(0, 1, 0),
                   S2Point(sqrt(3), 1, 0));
  CheckInterpolate(2./3, S2Point(1, 0, 0), S2Point(0, 1, 0),
                   S2Point(1, sqrt(3), 0));

  // Test that interpolation is accurate on a long edge (but not so long that
  // the definition of the edge itself becomes too unstable).
  {
    double const kLng = M_PI - 1e-2;
    S2Point a = S2LatLng::FromRadians(0, 0).ToPoint();
    S2Point b = S2LatLng::FromRadians(0, kLng).ToPoint();
    for (double f = 0.4; f > 1e-15; f *= 0.1) {
      CheckInterpolate(f, a, b,
                       S2LatLng::FromRadians(0, f * kLng).ToPoint());
      CheckInterpolate(1 - f, a, b,
                       S2LatLng::FromRadians(0, (1 - f) * kLng).ToPoint());
    }
  }
}

TEST(S2EdgeUtil, InterpolateCanExtrapolate) {
  const S2Point i(1, 0, 0);
  const S2Point j(0, 1, 0);
  // Initial vectors at 90 degrees.
  CheckInterpolate(0, i, j, S2Point(1, 0, 0));
  CheckInterpolate(1, i, j ,S2Point(0, 1, 0));
  CheckInterpolate(1.5, i, j ,S2Point(-1, 1, 0));
  CheckInterpolate(2, i, j, S2Point(-1, 0, 0));
  CheckInterpolate(3, i, j, S2Point(0, -1, 0));
  CheckInterpolate(4, i, j, S2Point(1, 0, 0));

  // Negative values of t.
  CheckInterpolate(-1, i, j, S2Point(0, -1, 0));
  CheckInterpolate(-2, i, j, S2Point(-1, 0, 0));
  CheckInterpolate(-3, i, j ,S2Point(0, 1, 0));
  CheckInterpolate(-4, i, j, S2Point(1, 0, 0));

  // Initial vectors at 45 degrees.
  CheckInterpolate(2, i, S2Point(1, 1, 0), S2Point(0, 1, 0));
  CheckInterpolate(3, i, S2Point(1, 1, 0), S2Point(-1, 1, 0));
  CheckInterpolate(4, i, S2Point(1, 1, 0), S2Point(-1, 0, 0));

  // Initial vectors at 135 degrees.
  CheckInterpolate(2, i, S2Point(-1, 1, 0), S2Point(0, -1, 0));

  // Take a small fraction along the curve.
  S2Point p(S2EdgeUtil::Interpolate(0.001, i, j));
  // We should get back where we started.
  CheckInterpolate(1000, i, p, j);

}


TEST(S2EdgeUtil, RepeatedInterpolation) {
  // Check that points do not drift away from unit length when repeated
  // interpolations are done.
  for (int i = 0; i < 100; ++i) {
    S2Point a = S2Testing::RandomPoint();
    S2Point b = S2Testing::RandomPoint();
    for (int j = 0; j < 1000; ++j) {
      a = S2EdgeUtil::Interpolate(0.01, a, b);
    }
    EXPECT_TRUE(S2::IsUnitLength(a));
  }
}

TEST(S2EdgeUtil, IntersectionTolerance) {
  // We repeatedly construct two edges that cross near a random point "p",
  // and measure the distance from the actual intersection point "x" to the
  // the expected intersection point "p" and also to the edges that cross
  // near "p".
  //
  // Note that GetIntersection() does not guarantee that "x" and "p" will be
  // close together (since the intersection point is numerically unstable
  // when the edges cross at a very small angle), but it does guarantee that
  // "x" will be close to both of the edges that cross.

  S1Angle max_point_dist, max_edge_dist;
  S2Testing::Random* rnd = &S2Testing::rnd;
  for (int i = 0; i < 1000; ++i) {
    // We construct two edges AB and CD that intersect near "p".  The angle
    // between AB and CD (expressed as a slope) is chosen randomly between
    // 1e-15 and 1.0 such that its logarithm is uniformly distributed.  This
    // implies that small values are much more likely to be chosen.
    //
    // Once the slope is chosen, the four points ABCD must be offset from P
    // by at least (1e-15 / slope) so that the points are guaranteed to have
    // the correct circular ordering around P.  This is the distance from P
    // at which the two edges are separated by about 1e-15, which is
    // approximately the minimum distance at which we can expect computed
    // points on the two lines to be distinct and have the correct ordering.
    //
    // The actual offset distance from P is chosen randomly in the range
    // [1e-15 / slope, 1.0], again uniformly distributing the logarithm.
    // This ensures that we test both long and very short segments that
    // intersect at both large and very small angles.

    S2Point p, d1, d2;
    S2Testing::GetRandomFrame(&p, &d1, &d2);
    double slope = pow(1e-15, rnd->RandDouble());
    d2 = d1 + slope * d2;
    S2Point a = (p + pow(1e-15 / slope, rnd->RandDouble()) * d1).Normalize();
    S2Point b = (p - pow(1e-15 / slope, rnd->RandDouble()) * d1).Normalize();
    S2Point c = (p + pow(1e-15 / slope, rnd->RandDouble()) * d2).Normalize();
    S2Point d = (p - pow(1e-15 / slope, rnd->RandDouble()) * d2).Normalize();
    S2Point x = S2EdgeUtil::GetIntersection(a, b, c, d);
    S1Angle dist_ab = S2EdgeUtil::GetDistance(x, a, b);
    S1Angle dist_cd = S2EdgeUtil::GetDistance(x, c, d);
    EXPECT_LE(dist_ab, S2EdgeUtil::kIntersectionTolerance);
    EXPECT_LE(dist_cd, S2EdgeUtil::kIntersectionTolerance);
    max_edge_dist = max(max_edge_dist, max(dist_ab, dist_cd));
    max_point_dist = max(max_point_dist, S1Angle(p, x));
  }
  LOG(INFO) << "Max distance to either edge being intersected: "
            << max_edge_dist.radians();
  LOG(INFO) << "Maximum distance to expected intersection point: "
            << max_point_dist.radians();
}

bool IsEdgeBNearEdgeA(string const& a_str, const string& b_str,
                      double max_error_degrees) {
  scoped_ptr<S2Polyline> a(S2Testing::MakePolyline(a_str));
  EXPECT_EQ(2, a->num_vertices());
  scoped_ptr<S2Polyline> b(S2Testing::MakePolyline(b_str));
  EXPECT_EQ(2, b->num_vertices());
  return S2EdgeUtil::IsEdgeBNearEdgeA(a->vertex(0), a->vertex(1),
                                      b->vertex(0), b->vertex(1),
                                      S1Angle::Degrees(max_error_degrees));
}

TEST(S2EdgeUtil, EdgeBNearEdgeA) {
  // Edge is near itself.
  EXPECT_TRUE(IsEdgeBNearEdgeA("5:5, 10:-5", "5:5, 10:-5", 1e-6));

  // Edge is near its reverse
  EXPECT_TRUE(IsEdgeBNearEdgeA("5:5, 10:-5", "10:-5, 5:5", 1e-6));

  // Short edge is near long edge.
  EXPECT_TRUE(IsEdgeBNearEdgeA("10:0, -10:0", "2:1, -2:1", 1.0));

  // Long edges cannot be near shorter edges.
  EXPECT_FALSE(IsEdgeBNearEdgeA("2:1, -2:1", "10:0, -10:0", 1.0));

  // Orthogonal crossing edges are not near each other...
  EXPECT_FALSE(IsEdgeBNearEdgeA("10:0, -10:0", "0:1.5, 0:-1.5", 1.0));

  // ... unless all points on B are within tolerance of A.
  EXPECT_TRUE(IsEdgeBNearEdgeA("10:0, -10:0", "0:1.5, 0:-1.5", 2.0));

  // Very long edges whose endpoints are close may have interior points that are
  // far apart.  An implementation that only considers the vertices of polylines
  // will incorrectly consider such edges as "close" when they are not.
  // Consider, for example, two consecutive lines of longitude.  As they
  // approach the poles, they become arbitrarily close together, but along the
  // equator they bow apart.
  EXPECT_FALSE(IsEdgeBNearEdgeA("89:1, -89:1", "89:2, -89:2", 0.5));
  EXPECT_TRUE(IsEdgeBNearEdgeA("89:1, -89:1", "89:2, -89:2", 1.5));

  // The two arcs here are nearly as long as S2 edges can be (just shy of 180
  // degrees), and their endpoints are less than 1 degree apart.  Their
  // midpoints, however, are at opposite ends of the sphere along its equator.
  EXPECT_FALSE(IsEdgeBNearEdgeA(
                   "0:-179.75, 0:-0.25", "0:179.75, 0:0.25", 1.0));

  // At the equator, the second arc here is 9.75 degrees from the first, and
  // closer at all other points.  However, the southern point of the second arc
  // (-1, 9.75) is too far from the first arc for the short-circuiting logic in
  // IsEdgeBNearEdgeA to apply.
  EXPECT_TRUE(IsEdgeBNearEdgeA("40:0, -5:0", "39:0.975, -1:0.975", 1.0));

  // Same as above, but B's orientation is reversed, causing the angle between
  // the normal vectors of circ(B) and circ(A) to be (180-9.75) = 170.5 degrees,
  // not 9.75 degrees.  The greatest separation between the planes is still 9.75
  // degrees.
  EXPECT_TRUE(IsEdgeBNearEdgeA("10:0, -10:0", "-.4:0.975, 0.4:0.975", 1.0));

  // A and B are on the same great circle, A and B partially overlap, but the
  // only part of B that does not overlap A is shorter than tolerance.
  EXPECT_TRUE(IsEdgeBNearEdgeA("0:0, 1:0", "0.9:0, 1.1:0", 0.25));

  // A and B are on the same great circle, all points on B are close to A at its
  // second endpoint, (1,0).
  EXPECT_TRUE(IsEdgeBNearEdgeA("0:0, 1:0", "1.1:0, 1.2:0", 0.25));

  // Same as above, but B's orientation is reversed.  This case is special
  // because the projection of the normal defining A onto the plane containing B
  // is the null vector, and must be handled by a special case.
  EXPECT_TRUE(IsEdgeBNearEdgeA("0:0, 1:0", "1.2:0, 1.1:0", 0.25));
}


TEST(S2EdgeUtil, CollinearEdgesThatDontTouch) {
  const int kIters = 1000;
  for (int iter = 0; iter < kIters; ++iter) {
    S2Point a = S2Testing::RandomPoint();
    S2Point d = S2Testing::RandomPoint();
    S2Point b = S2EdgeUtil::Interpolate(0.05, a, d);
    S2Point c = S2EdgeUtil::Interpolate(0.95, a, d);
    EXPECT_GT(0, S2EdgeUtil::RobustCrossing(a, b, c, d));
    EXPECT_GT(0, S2EdgeUtil::RobustCrossing(a, b, c, d));
    S2EdgeUtil::EdgeCrosser crosser(&a, &b, &c);
    EXPECT_GT(0, crosser.RobustCrossing(&d));
    EXPECT_GT(0, crosser.RobustCrossing(&c));
  }
}


TEST(S2EdgeUtil, CoincidentZeroLengthEdgesThatDontTouch) {
  // Since normalization is not perfect, and vertices are not always perfectly
  // normalized anyway for various reasons, it is important that the edge
  // primitives can handle vertices that exactly coincident when projected
  // onto the unit sphere but that are not identical.
  //
  // This test checks pairs of edges AB and CD where A,B,C,D are exactly
  // coincident on the sphere and the norms of A,B,C,D are monotonically
  // increasing.  Such edge pairs should never intersect.  (This is not
  // obvious, since it depends on the particular symbolic perturbations used
  // by S2::RobustCCW().  It would be better to replace this with a test that
  // says that the CCW results must be consistent with each other.)
  const int kIters = 1000;
  for (int iter = 0; iter < kIters; ++iter) {
    // Construct a point P where every component is zero or a power of 2.
    S2Point p;
    for (int i = 0; i < 2; ++i) {
      int binary_exp = S2Testing::rnd.Skewed(11);
      p[i] = (binary_exp > 1022) ? 0 : pow(2, -binary_exp);
    }
    // If all components were zero, try again.  Note that normalization may
    // convert a non-zero point into a zero one due to underflow (!)
    p = p.Normalize();
    if (p == S2Point(0, 0, 0)) { --iter; continue; }

    // Now every non-zero component should have exactly the same mantissa.
    // This implies that if we scale the point by an arbitrary factor, every
    // non-zero component will still have the same mantissa.  Scale the points
    // so that they are all distinct and yet still satisfy S2::IsNormalized().
    S2Point a = (1-3e-16) * p;
    S2Point b = (1-1e-16) * p;
    S2Point c = p;
    S2Point d = (1+2e-16) * p;

    // Verify that the expected edges do not cross.
    EXPECT_GT(0, S2EdgeUtil::RobustCrossing(a, b, c, d));
    S2EdgeUtil::EdgeCrosser crosser(&a, &b, &c);
    EXPECT_GT(0, crosser.RobustCrossing(&d));
    EXPECT_GT(0, crosser.RobustCrossing(&c));
  }
}


static void BM_RobustCrosserEdgesCross(int iters) {
  // Copied from BenchmarkCrossing() above.
  StopBenchmarkTiming();
  static const int kNumPoints = 400;
  vector<S2Point> p(kNumPoints);
  for (int i = 0; i < kNumPoints; ++i) p[i] = S2Testing::RandomPoint();
  // 1/4th of the points will cross ab.
  const S2Point& a = S2Testing::RandomPoint();
  const S2Point& b = (-a + S2Point(0.1, 0.1, 0.1)).Normalize();
  StartBenchmarkTiming();

  S2EdgeUtil::EdgeCrosser crosser(&a, &b, &p[0]);
  int num_crossings = 0;
  for (int i = 0; i < iters; ++i) {
    const S2Point& d = p[i % kNumPoints];

    if (crosser.RobustCrossing(&d) > 0)
      ++num_crossings;
  }
  VLOG(5) << "Fraction crossing = " << 1.0 * num_crossings / iters;
  VLOG(5) << "num_crossings = " << num_crossings;
}
BENCHMARK(BM_RobustCrosserEdgesCross);


}  // namespace
