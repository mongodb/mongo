// Copyright 2005 Google Inc. All Rights Reserved.

#include <algorithm>
#include <functional>
using namespace std;
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <hash_set>
#include <hash_map>
using __gnu_cxx::hash_set;

#include "s2.h"
#include "base/logging.h"
#include "s2latlng.h"
#include "s2testing.h"
#include "util/math/matrix3x3-inl.h"
#include "gtest/gtest.h"

static inline int SwapAxes(int ij) {
  return ((ij >> 1) & 1) + ((ij & 1) << 1);
}

static inline int InvertBits(int ij) {
  return ij ^ 3;
}

TEST(S2, TraversalOrder) {
  for (int r = 0; r < 4; ++r) {
    for (int i = 0; i < 4; ++i) {
      // Check consistency with respect to swapping axes.
      EXPECT_EQ(S2::kIJtoPos[r][i],
                S2::kIJtoPos[r ^ S2::kSwapMask][SwapAxes(i)]);
      EXPECT_EQ(S2::kPosToIJ[r][i],
                SwapAxes(S2::kPosToIJ[r ^ S2::kSwapMask][i]));

      // Check consistency with respect to reversing axis directions.
      EXPECT_EQ(S2::kIJtoPos[r][i],
                S2::kIJtoPos[r ^ S2::kInvertMask][InvertBits(i)]);
      EXPECT_EQ(S2::kPosToIJ[r][i],
                InvertBits(S2::kPosToIJ[r ^ S2::kInvertMask][i]));

      // Check that the two tables are inverses of each other.
      EXPECT_EQ(S2::kIJtoPos[r][S2::kPosToIJ[r][i]], i);
      EXPECT_EQ(S2::kPosToIJ[r][S2::kIJtoPos[r][i]], i);
    }
  }
}

TEST(S2, ST_UV_Conversions) {
  // Check boundary conditions.
  for (double s = 0; s <= 1; s += 0.5) {
    volatile double u = S2::STtoUV(s);
    EXPECT_EQ(u, 2 * s - 1);
  }
  for (double u = -1; u <= 1; ++u) {
    volatile double s = S2::UVtoST(u);
    EXPECT_EQ(s, 0.5 * (u + 1));
  }
  // Check that UVtoST and STtoUV are inverses.
  for (double x = 0; x <= 1; x += 0.0001) {
    EXPECT_NEAR(S2::UVtoST(S2::STtoUV(x)), x, 1e-15);
    EXPECT_NEAR(S2::STtoUV(S2::UVtoST(2*x-1)), 2*x-1, 1e-15);
  }
}

TEST(S2, FaceUVtoXYZ) {
  // Check that each face appears exactly once.
  S2Point sum;
  for (int face = 0; face < 6; ++face) {
    S2Point center = S2::FaceUVtoXYZ(face, 0, 0);
    EXPECT_EQ(S2::GetNorm(face), center);
    EXPECT_EQ(fabs(center[center.LargestAbsComponent()]), 1);
    sum += center.Fabs();
  }
  EXPECT_EQ(sum, S2Point(2, 2, 2));

  // Check that each face has a right-handed coordinate system.
  for (int face = 0; face < 6; ++face) {
    EXPECT_EQ(S2::GetUAxis(face).CrossProd(S2::GetVAxis(face))
              .DotProd(S2::FaceUVtoXYZ(face, 0, 0)), 1);
  }

  // Check that the Hilbert curves on each face combine to form a
  // continuous curve over the entire cube.
  for (int face = 0; face < 6; ++face) {
    // The Hilbert curve on each face starts at (-1,-1) and terminates
    // at either (1,-1) (if axes not swapped) or (-1,1) (if swapped).
    int sign = (face & S2::kSwapMask) ? -1 : 1;
    EXPECT_EQ(S2::FaceUVtoXYZ(face, sign, -sign),
              S2::FaceUVtoXYZ((face + 1) % 6, -1, -1));
  }
}

TEST(S2, UVNorms) {
  // Check that GetUNorm and GetVNorm compute right-handed normals for
  // an edge in the increasing U or V direction.
  for (int face = 0; face < 6; ++face) {
    for (double x = -1; x <= 1; x += 1/1024.) {
      EXPECT_DOUBLE_EQ(S2::FaceUVtoXYZ(face, x, -1)
                       .CrossProd(S2::FaceUVtoXYZ(face, x, 1))
                       .Angle(S2::GetUNorm(face, x)), 0);
      EXPECT_DOUBLE_EQ(S2::FaceUVtoXYZ(face, -1, x)
                       .CrossProd(S2::FaceUVtoXYZ(face, 1, x))
                       .Angle(S2::GetVNorm(face, x)), 0);
    }
  }
}

TEST(S2, UVAxes) {
  // Check that axes are consistent with FaceUVtoXYZ.
  for (int face = 0; face < 6; ++face) {
    EXPECT_EQ(S2::GetUAxis(face),
              S2::FaceUVtoXYZ(face, 1, 0) - S2::FaceUVtoXYZ(face, 0, 0));
    EXPECT_EQ(S2::GetVAxis(face),
              S2::FaceUVtoXYZ(face, 0, 1) - S2::FaceUVtoXYZ(face, 0, 0));
  }
}

TEST(S2, AnglesAreas) {
  S2Point pz(0, 0, 1);
  S2Point p000(1, 0, 0);
  S2Point p045 = S2Point(1, 1, 0).Normalize();
  S2Point p090(0, 1, 0);
  S2Point p180(-1, 0, 0);

  EXPECT_DOUBLE_EQ(S2::Angle(p000, pz, p045), M_PI_4);
  EXPECT_DOUBLE_EQ(S2::Angle(p045, pz, p180), 3 * M_PI_4);
  EXPECT_DOUBLE_EQ(S2::Angle(p000, pz, p180), M_PI);
  EXPECT_DOUBLE_EQ(S2::Angle(pz, p000, pz), 0);
  EXPECT_DOUBLE_EQ(S2::Angle(pz, p000, p045), M_PI_2);

  EXPECT_DOUBLE_EQ(S2::TurnAngle(p000, pz, p045), -3 * M_PI_4);
  EXPECT_DOUBLE_EQ(S2::TurnAngle(p045, pz, p180), -M_PI_4);
  EXPECT_DOUBLE_EQ(S2::TurnAngle(p180, pz, p045), M_PI_4);
  EXPECT_DOUBLE_EQ(S2::TurnAngle(p000, pz, p180), 0);
  EXPECT_DOUBLE_EQ(fabs(S2::TurnAngle(pz, p000, pz)), M_PI);
  EXPECT_DOUBLE_EQ(S2::TurnAngle(pz, p000, p045), M_PI_2);

  EXPECT_DOUBLE_EQ(S2::Area(p000, p090, pz), M_PI_2);
  EXPECT_DOUBLE_EQ(S2::Area(p045, pz, p180), 3 * M_PI_4);

  // Make sure that Area() has good *relative* accuracy even for
  // very small areas.
  static double const eps = 1e-10;
  S2Point pepsx = S2Point(eps, 0, 1).Normalize();
  S2Point pepsy = S2Point(0, eps, 1).Normalize();
  double expected1 = 0.5 * eps * eps;
  EXPECT_NEAR(S2::Area(pepsx, pepsy, pz), expected1, 1e-14 * expected1);

  // Make sure that it can handle degenerate triangles.
  S2Point pr = S2Point(0.257, -0.5723, 0.112).Normalize();
  S2Point pq = S2Point(-0.747, 0.401, 0.2235).Normalize();
  EXPECT_EQ(S2::Area(pr, pr, pr), 0);
  // TODO: The following test is not exact in optimized mode because the
  // compiler chooses to mix 64-bit and 80-bit intermediate results.
  EXPECT_NEAR(S2::Area(pr, pq, pr), 0, 1e-15);
  EXPECT_EQ(S2::Area(p000, p045, p090), 0);

  double max_girard = 0;
  for (int i = 0; i < 10000; ++i) {
    S2Point p0 = S2Testing::RandomPoint();
    S2Point d1 = S2Testing::RandomPoint();
    S2Point d2 = S2Testing::RandomPoint();
    S2Point p1 = (p0 + 1e-15 * d1).Normalize();
    S2Point p2 = (p0 + 1e-15 * d2).Normalize();
    // The actual displacement can be as much as 1.2e-15 due to roundoff.
    // This yields a maximum triangle area of about 0.7e-30.
    EXPECT_LE(S2::Area(p0, p1, p2), 0.7e-30);
    max_girard = max(max_girard, S2::GirardArea(p0, p1, p2));
  }
  // This check only passes if GirardArea() uses RobustCrossProd().
  LOG(INFO) << "Worst case Girard for triangle area 1e-30: " << max_girard;
  EXPECT_LE(max_girard, 1e-14);

  // Try a very long and skinny triangle.
  S2Point p045eps = S2Point(1, 1, eps).Normalize();
  double expected2 = 5.8578643762690495119753e-11;  // Mathematica.
  EXPECT_NEAR(S2::Area(p000, p045eps, p090), expected2, 1e-9 * expected2);

  // Triangles with near-180 degree edges that sum to a quarter-sphere.
  static double const eps2 = 1e-14;
  S2Point p000eps2 = S2Point(1, 0.1*eps2, eps2).Normalize();
  double quarter_area1 = S2::Area(p000eps2, p000, p045) +
                         S2::Area(p000eps2, p045, p180) +
                         S2::Area(p000eps2, p180, pz) +
                         S2::Area(p000eps2, pz, p000);
  EXPECT_DOUBLE_EQ(quarter_area1, M_PI);

  // Four other triangles that sum to a quarter-sphere.
  S2Point p045eps2 = S2Point(1, 1, eps2).Normalize();
  double quarter_area2 = S2::Area(p045eps2, p000, p045) +
                         S2::Area(p045eps2, p045, p180) +
                         S2::Area(p045eps2, p180, pz) +
                         S2::Area(p045eps2, pz, p000);
  EXPECT_DOUBLE_EQ(quarter_area2, M_PI);

  // Compute the area of a hemisphere using four triangles with one near-180
  // degree edge and one near-degenerate edge.  This test fails in optimized
  // mode unless GirardArea uses RobustCrossProd(), because the compiler
  // doesn't compute all the inlined CrossProd() calls with the same level of
  // accuracy (some intermediate values are spilled to 64-bit temporaries).
  for (int i = 0; i < 100; ++i) {
    double lng = 2 * M_PI * S2Testing::rnd.RandDouble();
    S2Point p0 = S2LatLng::FromRadians(1e-12, lng).Normalized().ToPoint();
    S2Point p1 = S2LatLng::FromRadians(0, lng).Normalized().ToPoint();
    double p2_lng = lng + S2Testing::rnd.RandDouble();
    S2Point p2 = S2LatLng::FromRadians(0, p2_lng).Normalized().ToPoint();
    S2Point p3 = S2LatLng::FromRadians(0, lng + M_PI).Normalized().ToPoint();
    S2Point p4 = S2LatLng::FromRadians(0, lng + 4.0).Normalized().ToPoint();
    double area = (S2::Area(p0, p1, p2) + S2::Area(p0, p2, p3) +
                   S2::Area(p0, p3, p4) + S2::Area(p0, p4, p1));
    EXPECT_NEAR(area, 2 * M_PI, 1e-14);
  }
}

TEST(S2, TrueCentroid) {
  // Test TrueCentroid() with very small triangles.  This test assumes that
  // the triangle is small enough so that it is nearly planar.
  for (int i = 0; i < 100; ++i) {
    S2Point p, x, y;
    S2Testing::GetRandomFrame(&p, &x, &y);
    double d = 1e-4 * pow(1e-4, S2Testing::rnd.RandDouble());
    S2Point p0 = (p - d * x).Normalize();
    S2Point p1 = (p + d * x).Normalize();
    S2Point p2 = (p + 3 * d * y).Normalize();
    S2Point centroid = S2::TrueCentroid(p0, p1, p2).Normalize();

    // The centroid of a planar triangle is at the intersection of its
    // medians, which is two-thirds of the way along each median.
    S2Point expected_centroid = (p + d * y).Normalize();
    EXPECT_LE(centroid.Angle(expected_centroid), 2e-8);
  }
}

TEST(RobustCCW, ColinearPoints) {
  // The following points happen to be *exactly collinear* along a line that it
  // approximate tangent to the surface of the unit sphere.  In fact, C is the
  // exact midpoint of the line segment AB.  All of these points are close
  // enough to unit length to satisfy S2::IsUnitLength().
  S2Point a(0.72571927877036835, 0.46058825605889098, 0.51106749730504852);
  S2Point b(0.7257192746638208, 0.46058826573818168, 0.51106749441312738);
  S2Point c(0.72571927671709457, 0.46058826089853633, 0.51106749585908795);
  EXPECT_EQ(c - a, b - c);
  EXPECT_NE(0, S2::RobustCCW(a, b, c));
  EXPECT_EQ(S2::RobustCCW(a, b, c), S2::RobustCCW(b, c, a));
  EXPECT_EQ(S2::RobustCCW(a, b, c), -S2::RobustCCW(c, b, a));

  // The points "x1" and "x2" are exactly proportional, i.e. they both lie
  // on a common line through the origin.  Both points are considered to be
  // normalized, and in fact they both satisfy (x == x.Normalize()).
  // Therefore the triangle (x1, x2, -x1) consists of three distinct points
  // that all lie on a common line through the origin.
  S2Point x1(0.99999999999999989, 1.4901161193847655e-08, 0);
  S2Point x2(1, 1.4901161193847656e-08, 0);
  EXPECT_EQ(x1, x1.Normalize());
  EXPECT_EQ(x2, x2.Normalize());
  EXPECT_NE(0, S2::RobustCCW(x1, x2, -x1));
  EXPECT_EQ(S2::RobustCCW(x1, x2, -x1), S2::RobustCCW(x2, -x1, x1));
  EXPECT_EQ(S2::RobustCCW(x1, x2, -x1), -S2::RobustCCW(-x1, x2, x1));

  // Here are two more points that are distinct, exactly proportional, and
  // that satisfy (x == x.Normalize()).
  S2Point x3 = S2Point(1, 1, 1).Normalize();
  S2Point x4 = 0.99999999999999989 * x3;
  EXPECT_EQ(x3, x3.Normalize());
  EXPECT_EQ(x4, x4.Normalize());
  EXPECT_NE(x3, x4);
  EXPECT_NE(0, S2::RobustCCW(x3, x4, -x3));

  // The following two points demonstrate that Normalize() is not idempotent,
  // i.e. y0.Normalize() != y0.Normalize().Normalize().  Both points satisfy
  // S2::IsNormalized(), though, and the two points are exactly proportional.
  S2Point y0 = S2Point(1, 1, 0);
  S2Point y1 = y0.Normalize();
  S2Point y2 = y1.Normalize();
  EXPECT_NE(y1, y2);
  EXPECT_EQ(y2, y2.Normalize());
  EXPECT_NE(0, S2::RobustCCW(y1, y2, -y1));
  EXPECT_EQ(S2::RobustCCW(y1, y2, -y1), S2::RobustCCW(y2, -y1, y1));
  EXPECT_EQ(S2::RobustCCW(y1, y2, -y1), -S2::RobustCCW(-y1, y2, y1));
}

// Given 3 points A, B, C that are exactly coplanar with the origin and where
// A < B < C in lexicographic order, verify that ABC is counterclockwise (if
// expected == 1) or clockwise (if expected == -1) using S2::ExpensiveCCW().
//
// This method is intended specifically for checking the cases where
// symbolic perturbations are needed to break ties.
static void CheckSymbolicCCW(int expected, S2Point const& a,
                             S2Point const& b, S2Point const& c) {
  CHECK_LT(a, b);
  CHECK_LT(b, c);
  CHECK_EQ(0, a.DotProd(b.CrossProd(c)));

  // Use ASSERT rather than EXPECT to suppress spurious error messages.
  ASSERT_EQ(expected, S2::ExpensiveCCW(a, b, c));
  ASSERT_EQ(expected, S2::ExpensiveCCW(b, c, a));
  ASSERT_EQ(expected, S2::ExpensiveCCW(c, a, b));
  ASSERT_EQ(-expected, S2::ExpensiveCCW(c, b, a));
  ASSERT_EQ(-expected, S2::ExpensiveCCW(b, a, c));
  ASSERT_EQ(-expected, S2::ExpensiveCCW(a, c, b));
}

TEST(RobustCCW, SymbolicPerturbationCodeCoverage) {
  // The purpose of this test is simply to get code coverage of
  // SymbolicallyPerturbedCCW().  Let M_1, M_2, ... be the sequence of
  // submatrices whose determinant sign is tested by that function.  Then the
  // i-th test below is a 3x3 matrix M (with rows A, B, C) such that:
  //
  //    det(M) = 0
  //    det(M_j) = 0 for j < i
  //    det(M_i) != 0
  //    A < B < C in lexicographic order.
  //
  // I checked that reversing the sign of any of the "return" statements in
  // SymbolicallyPerturbedCCW() will cause this test to fail.

  // det(M_1) = b0*c1 - b1*c0
  CheckSymbolicCCW(1, S2Point(-3, -1, 0), S2Point(-2, 1, 0), S2Point(1, -2, 0));

  // det(M_2) = b2*c0 - b0*c2
  CheckSymbolicCCW(1, S2Point(-6, 3, 3), S2Point(-4, 2, -1), S2Point(-2, 1, 4));

  // det(M_3) = b1*c2 - b2*c1
  CheckSymbolicCCW(1, S2Point(0, -1, -1), S2Point(0, 1, -2), S2Point(0, 2, 1));
  // From this point onward, B or C must be zero, or B is proportional to C.

  // det(M_4) = c0*a1 - c1*a0
  CheckSymbolicCCW(1, S2Point(-1, 2, 7), S2Point(2, 1, -4), S2Point(4, 2, -8));

  // det(M_5) = c0
  CheckSymbolicCCW(1, S2Point(-4, -2, 7), S2Point(2, 1, -4), S2Point(4, 2, -8));

  // det(M_6) = -c1
  CheckSymbolicCCW(1, S2Point(0, -5, 7), S2Point(0, -4, 8), S2Point(0, -2, 4));

  // det(M_7) = c2*a0 - c0*a2
  CheckSymbolicCCW(1, S2Point(-5, -2, 7), S2Point(0, 0, -2), S2Point(0, 0, -1));

  // det(M_8) = c2
  CheckSymbolicCCW(1, S2Point(0, -2, 7), S2Point(0, 0, 1), S2Point(0, 0, 2));
  // From this point onward, C must be zero.

  // det(M_9) = a0*b1 - a1*b0
  CheckSymbolicCCW(1, S2Point(-3, 1, 7), S2Point(-1, -4, 1), S2Point(0, 0, 0));

  // det(M_10) = -b0
  CheckSymbolicCCW(1, S2Point(-6, -4, 7), S2Point(-3, -2, 1), S2Point(0, 0, 0));

  // det(M_11) = b1
  CheckSymbolicCCW(-1, S2Point(0, -4, 7), S2Point(0, -2, 1), S2Point(0, 0, 0));

  // det(M_12) = a0
  CheckSymbolicCCW(-1, S2Point(-1, -4, 5), S2Point(0, 0, -3), S2Point(0, 0, 0));

  // det(M_13) = 1
  CheckSymbolicCCW(1, S2Point(0, -4, 5), S2Point(0, 0, -5), S2Point(0, 0, 0));
}

// This test repeatedly constructs some number of points that are on or nearly
// on a given great circle.  Then it chooses one of these points as the
// "origin" and sorts the other points in CCW order around it.  Of course,
// since the origin is on the same great circle as the points being sorted,
// nearly all of these tests are degenerate.  It then does various consistency
// checks to verify that the points are indeed sorted in CCW order.
//
// It is easier to think about what this test is doing if you imagine that the
// points are in general position rather than on a great circle.
class RobustCCWTest : public testing::Test {
 protected:
  // The following method is used to sort a collection of points in CCW order
  // around a given origin.  It returns true if A comes before B in the CCW
  // ordering (starting at an arbitrary fixed direction).
  class LessCCW : public binary_function<S2Point const&, S2Point const&, bool> {
   public:
    LessCCW(S2Point const& origin, S2Point const& start)
        : origin_(origin), start_(start) {
    }
    bool operator()(S2Point const& a, S2Point const& b) {
      // OrderedCCW() acts like "<=", so we need to invert the comparison.
      return !S2::OrderedCCW(start_, b, a, origin_);
    }
   private:
    S2Point const origin_;
    S2Point const start_;
  };

  // Given a set of points with no duplicates, first remove "origin" from
  // "points" (if it exists) and then sort the remaining points in CCW order
  // around "origin" putting the result in "sorted".
  static void SortCCW(vector<S2Point> const& points, S2Point const& origin,
                      vector<S2Point>* sorted) {
    // Make a copy of the points with "origin" removed.
    sorted->clear();
    remove_copy(points.begin(), points.end(), back_inserter(*sorted), origin);

    // Sort the points CCW around the origin starting at (*sorted)[0].
    LessCCW less(origin, (*sorted)[0]);
    sort(sorted->begin(), sorted->end(), less);
  }

  // Given a set of points sorted circularly CCW around "origin", and the
  // index "start" of a point A, count the number of CCW triangles OAB over
  // all sorted points B not equal to A.  Also check that the results of the
  // CCW tests are consistent with the hypothesis that the points are sorted.
  static int CountCCW(vector<S2Point> const& sorted, S2Point const& origin,
                      int start) {
    int num_ccw = 0;
    int last_ccw = 1;
    int const n = sorted.size();
    for (int j = 1; j < n; ++j) {
      int ccw = S2::RobustCCW(origin, sorted[start], sorted[(start + j) % n]);
      EXPECT_NE(0, ccw);
      if (ccw > 0) ++num_ccw;

      // Since the points are sorted around the origin, we expect to see a
      // (possibly empty) sequence of CCW triangles followed by a (possibly
      // empty) sequence of CW triangles.
      EXPECT_FALSE(ccw > 0 && last_ccw < 0);
      last_ccw = ccw;
    }
    return num_ccw;
  }

  // Test exhaustively whether the points in "sorted" are sorted circularly
  // CCW around "origin".
  static void TestCCW(vector<S2Point> const& sorted, S2Point const& origin) {
    int const n = sorted.size();
    int total_num_ccw = 0;
    int last_num_ccw = CountCCW(sorted, origin, n - 1);
    for (int start = 0; start < n; ++start) {
      int num_ccw = CountCCW(sorted, origin, start);
      // Each iteration we increase the start index by 1, therefore the number
      // of CCW triangles should decrease by at most 1.
      EXPECT_GE(num_ccw, last_num_ccw - 1);
      total_num_ccw += num_ccw;
      last_num_ccw = num_ccw;
    }
    // We have tested all triangles of the form OAB.  Exactly half of these
    // should be CCW.
    EXPECT_EQ(n * (n-1) / 2, total_num_ccw);
  }
      
  static void AddNormalized(S2Point const& a, vector<S2Point>* points) {
    points->push_back(a.Normalize());
  }

  // Add two points A1 and A2 that are slightly offset from A along the
  // tangent toward B, and such that A, A1, and A2 are exactly collinear
  // (i.e. even with infinite-precision arithmetic).
  static void AddTangentPoints(S2Point const& a, S2Point const& b,
                               vector<S2Point>* points) {
    S2Point dir = S2::RobustCrossProd(a, b).CrossProd(a).Normalize();
    if (dir == S2Point(0, 0, 0)) return;
    for (;;) {
      S2Point delta = 1e-15 * S2Testing::rnd.RandDouble() * dir;
      if ((a + delta) != a && (a + delta) - a == a - (a - delta) &&
          S2::IsUnitLength(a + delta) && S2::IsUnitLength(a - delta)) {
        points->push_back(a + delta);
        points->push_back(a - delta);
        return;
      }
    }
  }

  // Add zero or more (but usually one) point that is likely to trigger
  // RobustCCW() degeneracies among the given points.
  static void AddDegeneracy(vector<S2Point>* points) {
    S2Testing::Random* rnd = &S2Testing::rnd;
    S2Point a = (*points)[rnd->Uniform(points->size())];
    S2Point b = (*points)[rnd->Uniform(points->size())];
    int coord = rnd->Uniform(3);
    switch (rnd->Uniform(8)) {
      case 0:
        // Add a random point (not uniformly distributed) along the great
        // circle AB.
        AddNormalized((2 * rnd->RandDouble() - 1) * a +
                      (2 * rnd->RandDouble() - 1) * b, points);
        break;
      case 1:
        // Perturb one coordinate by the minimum amount possible.
        a[coord] = nextafter(a[coord], rnd->OneIn(2) ? 2 : -2);
        AddNormalized(a, points);
        break;
      case 2:
        // Perturb one coordinate by up to 1e-15.
        a[coord] += 1e-15 * (2 * rnd->RandDouble() - 1);
        AddNormalized(a, points);
        break;
      case 3:
        // Scale a point just enough so that it is different while still being
        // considered normalized.
        a *= rnd->OneIn(2) ? (1 + 2e-16) : (1 - 1e-16);
        if (S2::IsUnitLength(a)) points->push_back(a);
        break;
      case 4: {
        // Add the intersection point of AB with X=0, Y=0, or Z=0.
        S2Point dir(0, 0, 0);
        dir[coord] = rnd->OneIn(2) ? 1 : -1;
        S2Point norm = S2::RobustCrossProd(a, b).Normalize();
        if (norm.Norm2() > 0) {
          AddNormalized(S2::RobustCrossProd(dir, norm), points);
        }
        break;
      }
      case 5:
        // Add two closely spaced points along the tangent at A to the great
        // circle through AB.
        AddTangentPoints(a, b, points);
        break;
      case 6:
        // Add two closely spaced points along the tangent at A to the great
        // circle through A and the X-axis.
        AddTangentPoints(a, S2Point(1, 0, 0), points);
        break;
      case 7:
        // Add the negative of a point.
        points->push_back(-a);
        break;
    }
  }

  // Sort the points around the given origin, and then do some consistency
  // checks to verify that they are actually sorted.
  static void SortAndTest(vector<S2Point> const& points,
                          S2Point const& origin) {
    vector<S2Point> sorted;
    SortCCW(points, origin, &sorted);
    TestCCW(sorted, origin);
  }

  // Construct approximately "n" points near the great circle through A and B,
  // then sort them and test whether they are sorted.
  static void TestGreatCircle(S2Point a, S2Point b, int n) {
    a = a.Normalize();
    b = b.Normalize();
    vector<S2Point> points;
    points.push_back(a);
    points.push_back(b);
    while (points.size() < (size_t)n) {
      AddDegeneracy(&points);
    }
    // Remove any (0, 0, 0) points that were accidentically created, then sort
    // the points and remove duplicates.
    points.erase(remove(points.begin(), points.end(), S2Point(0, 0, 0)),
                 points.end());
    sort(points.begin(), points.end());
    points.erase(unique(points.begin(), points.end()), points.end());
    EXPECT_GE((int)points.size(), n / 2);

    SortAndTest(points, a);
    SortAndTest(points, b);
    for (size_t k = 0; k < points.size(); ++k) {
      SortAndTest(points, points[k]);
    }
  }
};

TEST_F(RobustCCWTest, StressTest) {
  // The run time of this test is *cubic* in the parameter below.
  static int const kNumPointsPerCircle = 20;

  // This test is randomized, so it is beneficial to run it several times.
  for (int iter = 0; iter < 3; ++iter) {
    // The most difficult great circles are the ones in the X-Y, Y-Z, and X-Z
    // planes, for two reasons.  First, when one or more coordinates are close
    // to zero then the perturbations can be much smaller, since floating
    // point numbers are spaced much more closely together near zero.  (This
    // tests the handling of things like underflow.)  The second reason is
    // that most of the cases of SymbolicallyPerturbedCCW() can only be
    // reached when one or more input point coordinates are zero.
    TestGreatCircle(S2Point(1, 0, 0), S2Point(0, 1, 0), kNumPointsPerCircle);
    TestGreatCircle(S2Point(1, 0, 0), S2Point(0, 0, 1), kNumPointsPerCircle);
    TestGreatCircle(S2Point(0, -1, 0), S2Point(0, 0, 1), kNumPointsPerCircle);

    // This tests a great circle where at least some points have X, Y, and Z
    // coordinates with exactly the same mantissa.  One useful property of
    // such points is that when they are scaled (e.g. multiplying by 1+eps),
    // all such points are exactly collinear with the origin.
    TestGreatCircle(S2Point(1 << 25, 1, -8), S2Point(-4, -(1 << 20), 1),
                    kNumPointsPerCircle);
  }
}

// Note: obviously, I could have defined a bundle of metrics like this in the
// S2 class itself rather than just for testing.  However, it's not clear that
// this is useful other than for testing purposes, and I find
// S2::kMinWidth.GetMaxLevel(width) to be slightly more readable than
// than S2::kWidth.min().GetMaxLevel(width).  Also, there is no fundamental
// reason that we need to analyze the minimum, maximum, and average values of
// every metric; it would be perfectly reasonable to just define one of these.

template<int dim>
class MetricBundle {
 public:
  typedef S2::Metric<dim> Metric;
  MetricBundle(Metric const& min, Metric const& max, Metric const& avg) :
    min_(min), max_(max), avg_(avg) {}
  Metric const& min_;
  Metric const& max_;
  Metric const& avg_;
};

template<int dim>
static void CheckMinMaxAvg(MetricBundle<dim> const& bundle) {
  EXPECT_LE(bundle.min_.deriv(), bundle.avg_.deriv());
  EXPECT_LE(bundle.avg_.deriv(), bundle.max_.deriv());
}

template<int dim>
static void CheckLessOrEqual(MetricBundle<dim> const& a,
                             MetricBundle<dim> const& b) {
  EXPECT_LE(a.min_.deriv(), b.min_.deriv());
  EXPECT_LE(a.max_.deriv(), b.max_.deriv());
  EXPECT_LE(a.avg_.deriv(), b.avg_.deriv());
}

TEST(S2, Metrics) {
  MetricBundle<1> angle_span(S2::kMinAngleSpan, S2::kMaxAngleSpan,
                             S2::kAvgAngleSpan);
  MetricBundle<1> width(S2::kMinWidth, S2::kMaxWidth, S2::kAvgWidth);
  MetricBundle<1> edge(S2::kMinEdge, S2::kMaxEdge, S2::kAvgEdge);
  MetricBundle<1> diag(S2::kMinDiag, S2::kMaxDiag, S2::kAvgDiag);
  MetricBundle<2> area(S2::kMinArea, S2::kMaxArea, S2::kAvgArea);

  // First, check that min <= avg <= max for each metric.
  CheckMinMaxAvg(angle_span);
  CheckMinMaxAvg(width);
  CheckMinMaxAvg(edge);
  CheckMinMaxAvg(diag);
  CheckMinMaxAvg(area);

  // Check that the maximum aspect ratio of an individual cell is consistent
  // with the global minimums and maximums.
  EXPECT_GE(S2::kMaxEdgeAspect, 1);
  EXPECT_LE(S2::kMaxEdgeAspect, S2::kMaxEdge.deriv() / S2::kMinEdge.deriv());
  EXPECT_GE(S2::kMaxDiagAspect, 1);
  EXPECT_LE(S2::kMaxDiagAspect, S2::kMaxDiag.deriv() / S2::kMinDiag.deriv());

  // Check various conditions that are provable mathematically.
  CheckLessOrEqual(width, angle_span);
  CheckLessOrEqual(width, edge);
  CheckLessOrEqual(edge, diag);

  EXPECT_GE(S2::kMinArea.deriv(),
            S2::kMinWidth.deriv() * S2::kMinEdge.deriv() - 1e-15);
  EXPECT_LE(S2::kMaxArea.deriv(),
            S2::kMaxWidth.deriv() * S2::kMaxEdge.deriv() + 1e-15);

  // GetMinLevelForLength() and friends have built-in assertions, we just need
  // to call these functions to test them.
  //
  // We don't actually check that the metrics are correct here, e.g. that
  // GetMinWidth(10) is a lower bound on the width of cells at level 10.
  // It is easier to check these properties in s2cell_unittest, since
  // S2Cell has methods to compute the cell vertices, etc.

  for (int level = -2; level <= S2CellId::kMaxLevel + 3; ++level) {
    double width = S2::kMinWidth.deriv() * pow(2, -level);
    if (level >= S2CellId::kMaxLevel + 3) width = 0;

    // Check boundary cases (exactly equal to a threshold value).
    int expected_level = max(0, min(S2CellId::kMaxLevel, level));
    cout << "width " << width << " has expected level " << expected_level << endl;
    EXPECT_EQ(S2::kMinWidth.GetMinLevel(width), expected_level);
    EXPECT_EQ(S2::kMinWidth.GetMaxLevel(width), expected_level);
    EXPECT_EQ(S2::kMinWidth.GetClosestLevel(width), expected_level);

    // Also check non-boundary cases.
    EXPECT_EQ(S2::kMinWidth.GetMinLevel(1.2 * width), expected_level);
    EXPECT_EQ(S2::kMinWidth.GetMaxLevel(0.8 * width), expected_level);
    EXPECT_EQ(S2::kMinWidth.GetClosestLevel(1.2 * width), expected_level);
    EXPECT_EQ(S2::kMinWidth.GetClosestLevel(0.8 * width), expected_level);

    // Same thing for area.
    double area = S2::kMinArea.deriv() * pow(4, -level);
    if (level <= -3) area = 0;
    EXPECT_EQ(S2::kMinArea.GetMinLevel(area), expected_level);
    EXPECT_EQ(S2::kMinArea.GetMaxLevel(area), expected_level);
    EXPECT_EQ(S2::kMinArea.GetClosestLevel(area), expected_level);
    EXPECT_EQ(S2::kMinArea.GetMinLevel(1.2 * area), expected_level);
    EXPECT_EQ(S2::kMinArea.GetMaxLevel(0.8 * area), expected_level);
    EXPECT_EQ(S2::kMinArea.GetClosestLevel(1.2 * area), expected_level);
    EXPECT_EQ(S2::kMinArea.GetClosestLevel(0.8 * area), expected_level);
  }
}

TEST(S2, Frames) {
  Matrix3x3_d m;
  S2Point z = S2Point(0.2, 0.5, -3.3).Normalize();
  S2::GetFrame(z, &m);
  EXPECT_TRUE(S2::ApproxEquals(m.Col(2), z));
  EXPECT_TRUE(S2::IsUnitLength(m.Col(0)));
  EXPECT_TRUE(S2::IsUnitLength(m.Col(1)));
  EXPECT_DOUBLE_EQ(m.Det(), 1);

  EXPECT_TRUE(S2::ApproxEquals(S2::ToFrame(m, m.Col(0)), S2Point(1, 0, 0)));
  EXPECT_TRUE(S2::ApproxEquals(S2::ToFrame(m, m.Col(1)), S2Point(0, 1, 0)));
  EXPECT_TRUE(S2::ApproxEquals(S2::ToFrame(m, m.Col(2)), S2Point(0, 0, 1)));

  EXPECT_TRUE(S2::ApproxEquals(S2::FromFrame(m, S2Point(1, 0, 0)), m.Col(0)));
  EXPECT_TRUE(S2::ApproxEquals(S2::FromFrame(m, S2Point(0, 1, 0)), m.Col(1)));
  EXPECT_TRUE(S2::ApproxEquals(S2::FromFrame(m, S2Point(0, 0, 1)), m.Col(2)));
}

#if 0
TEST(S2, S2PointHashSpreads) {
  int kTestPoints = 1 << 16;
  hash_set<size_t> set;
  hash_set<S2Point> points;
  hash<S2Point> hasher;
  S2Point base = S2Point(1, 1, 1);
  for (int i = 0; i < kTestPoints; ++i) {
    // All points in a tiny cap to test avalanche property of hash
    // function (the cap would be of radius 1mm on Earth (4*10^9/2^35).
    S2Point perturbed = base + S2Testing::RandomPoint() / (1ULL << 35);
    perturbed = perturbed.Normalize();
    set.insert(hasher(perturbed));
    points.insert(perturbed);
  }
  // A real collision is extremely unlikely.
  EXPECT_EQ((size_t)0, kTestPoints - points.size());
  // Allow a few for the hash.
  EXPECT_GE((size_t)10, kTestPoints - set.size());
}
#endif

TEST(S2, S2PointHashCollapsesZero) {
  double zero = 0;
  double minus_zero = -zero;
  EXPECT_NE(*reinterpret_cast<uint64 const*>(&zero),
            *reinterpret_cast<uint64 const*>(&minus_zero));
  hash_map<S2Point, int> map;
  S2Point zero_pt(zero, zero, zero);
  S2Point minus_zero_pt(minus_zero, minus_zero, minus_zero);

  map[zero_pt] = 1;
  map[minus_zero_pt] = 2;
  ASSERT_EQ((size_t)1, map.size());
}
