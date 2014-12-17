// Copyright 2005 Google Inc. All Rights Reserved.

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include "s2edgeutil.h"

#include "base/logging.h"

bool S2EdgeUtil::SimpleCrossing(S2Point const& a, S2Point const& b,
                                S2Point const& c, S2Point const& d) {
  // We compute SimpleCCW() for triangles ACB, CBD, BDA, and DAC.  All
  // of these triangles need to have the same orientation (CW or CCW)
  // for an intersection to exist.  Note that this is slightly more
  // restrictive than the corresponding definition for planar edges,
  // since we need to exclude pairs of line segments that would
  // otherwise "intersect" by crossing two antipodal points.

  S2Point ab = a.CrossProd(b);
  double acb = -(ab.DotProd(c));
  double bda = ab.DotProd(d);
  if (acb * bda <= 0) return false;

  S2Point cd = c.CrossProd(d);
  double cbd = -(cd.DotProd(b));
  double dac = cd.DotProd(a);
  return (acb * cbd > 0) && (acb * dac > 0);
}

int S2EdgeUtil::RobustCrossing(S2Point const& a, S2Point const& b,
                               S2Point const& c, S2Point const& d) {
  S2EdgeUtil::EdgeCrosser crosser(&a, &b, &c);
  return crosser.RobustCrossing(&d);
}

bool S2EdgeUtil::VertexCrossing(S2Point const& a, S2Point const& b,
                                S2Point const& c, S2Point const& d) {
  // If A == B or C == D there is no intersection.  We need to check this
  // case first in case 3 or more input points are identical.
  if (a == b || c == d) return false;

  // If any other pair of vertices is equal, there is a crossing if and only
  // if OrderedCCW() indicates that the edge AB is further CCW around the
  // shared vertex O (either A or B) than the edge CD, starting from an
  // arbitrary fixed reference point.
  if (a == d) return S2::OrderedCCW(S2::Ortho(a), c, b, a);
  if (b == c) return S2::OrderedCCW(S2::Ortho(b), d, a, b);
  if (a == c) return S2::OrderedCCW(S2::Ortho(a), d, b, a);
  if (b == d) return S2::OrderedCCW(S2::Ortho(b), c, a, b);

  S2LOG(DFATAL) << "VertexCrossing called with 4 distinct vertices";
  return false;
}

bool S2EdgeUtil::EdgeOrVertexCrossing(S2Point const& a, S2Point const& b,
                                      S2Point const& c, S2Point const& d) {
  int crossing = RobustCrossing(a, b, c, d);
  if (crossing < 0) return false;
  if (crossing > 0) return true;
  return VertexCrossing(a, b, c, d);
}

static void ReplaceIfCloser(S2Point const& x, S2Point const& y,
                            double *dmin2, S2Point* vmin) {
  // If the squared distance from x to y is less than dmin2, then replace
  // vmin by y and update dmin2 accordingly.

  double d2 = (x - y).Norm2();
  if (d2 < *dmin2 || (d2 == *dmin2 && y < *vmin)) {
    *dmin2 = d2;
    *vmin = y;
  }
}

S2Point S2EdgeUtil::GetIntersection(S2Point const& a0, S2Point const& a1,
                                    S2Point const& b0, S2Point const& b1) {
  DCHECK_GT(RobustCrossing(a0, a1, b0, b1), 0);

  // We use RobustCrossProd() to get accurate results even when two endpoints
  // are close together, or when the two line segments are nearly parallel.

  S2Point a_norm = S2::RobustCrossProd(a0, a1).Normalize();
  S2Point b_norm = S2::RobustCrossProd(b0, b1).Normalize();
  S2Point x = S2::RobustCrossProd(a_norm, b_norm).Normalize();

  // Make sure the intersection point is on the correct side of the sphere.
  // Since all vertices are unit length, and edges are less than 180 degrees,
  // (a0 + a1) and (b0 + b1) both have positive dot product with the
  // intersection point.  We use the sum of all vertices to make sure that the
  // result is unchanged when the edges are reversed or exchanged.

  if (x.DotProd((a0 + a1) + (b0 + b1)) < 0) x = -x;

  // The calculation above is sufficient to ensure that "x" is within
  // kIntersectionTolerance of the great circles through (a0,a1) and (b0,b1).
  // However, if these two great circles are very close to parallel, it is
  // possible that "x" does not lie between the endpoints of the given line
  // segments.  In other words, "x" might be on the great circle through
  // (a0,a1) but outside the range covered by (a0,a1).  In this case we do
  // additional clipping to ensure that it does.

  if (S2::OrderedCCW(a0, x, a1, a_norm) && S2::OrderedCCW(b0, x, b1, b_norm))
    return x;

  // Find the acceptable endpoint closest to x and return it.  An endpoint is
  // acceptable if it lies between the endpoints of the other line segment.
  double dmin2 = 10;
  S2Point vmin = x;
  if (S2::OrderedCCW(b0, a0, b1, b_norm)) ReplaceIfCloser(x, a0, &dmin2, &vmin);
  if (S2::OrderedCCW(b0, a1, b1, b_norm)) ReplaceIfCloser(x, a1, &dmin2, &vmin);
  if (S2::OrderedCCW(a0, b0, a1, a_norm)) ReplaceIfCloser(x, b0, &dmin2, &vmin);
  if (S2::OrderedCCW(a0, b1, a1, a_norm)) ReplaceIfCloser(x, b1, &dmin2, &vmin);

  DCHECK(S2::OrderedCCW(a0, vmin, a1, a_norm));
  DCHECK(S2::OrderedCCW(b0, vmin, b1, b_norm));
  return vmin;
}

// IEEE floating-point operations have a maximum error of 0.5 ULPS (units in
// the last place).  For double-precision numbers, this works out to 2**-53
// (about 1.11e-16) times the magnitude of the result.  It is possible to
// analyze the calculation done by GetIntersection() and work out the
// worst-case rounding error.  I have done a rough version of this, and my
// estimate is that the worst case distance from the intersection point X to
// the great circle through (a0, a1) is about 12 ULPS, or about 1.3e-15.
// This needs to be increased by a factor of (1/0.866) to account for the
// edge_splice_fraction() in S2PolygonBuilder.  Note that the maximum error
// measured by the unittest in 1,000,000 trials is less than 3e-16.
S1Angle const S2EdgeUtil::kIntersectionTolerance = S1Angle::Radians(1.5e-15);

double S2EdgeUtil::GetDistanceFraction(S2Point const& x,
                                       S2Point const& a0, S2Point const& a1) {
  DCHECK_NE(a0, a1);
  double d0 = x.Angle(a0);
  double d1 = x.Angle(a1);
  return d0 / (d0 + d1);
}

S2Point S2EdgeUtil::InterpolateAtDistance(S1Angle const& ax,
                                          S2Point const& a, S2Point const& b,
                                          S1Angle const& ab) {
  DCHECK(S2::IsUnitLength(a));
  DCHECK(S2::IsUnitLength(b));

  // As of crosstool v14, gcc tries to calculate sin(ax_radians),
  // cos(ax_radians), sin(ab_radians), cos(ab_radians) in the
  // following section by two sincos() calls. However, for some
  // inputs, sincos() returns significantly different values between
  // AMD and Intel.
  //
  // As a temporary workaround, "volatile" is added to ax_radians and
  // ab_radians, to prohibit the compiler to use such sincos() call,
  // because sin() and cos() don't seem to have the problem. See
  // b/3088321 for details.
  volatile double ax_radians = ax.radians();
  volatile double ab_radians = ab.radians();

  // The result X is some linear combination X = e*A + f*B of the input
  // points.  The fractions "e" and "f" can be derived by looking at the
  // components of this equation that are parallel and perpendicular to A.
  // Let E = e*A and F = f*B.  Then OEXF is a parallelogram.  You can obtain
  // the distance f = OF by considering the similar triangles produced by
  // dropping perpendiculars from the segments OF and OB to OA.
  double f = sin(ax_radians) / sin(ab_radians);

  // Form the dot product of the first equation with A to obtain
  // A.X = e*A.A + f*A.B.  Since A, B, and X are all unit vectors,
  // cos(ax) = e*1 + f*cos(ab), so
  double e = cos(ax_radians) - f * cos(ab_radians);

  // Mathematically speaking, if "a" and "b" are unit length then the result
  // is unit length as well.  But we normalize it anyway to prevent points
  // from drifting away from unit length when multiple interpolations are done
  // in succession (i.e. the result of one interpolation is fed into another).
  return (e * a + f * b).Normalize();
}

S2Point S2EdgeUtil::InterpolateAtDistance(S1Angle const& ax,
                                          S2Point const& a, S2Point const& b) {
  return InterpolateAtDistance(ax, a, b, S1Angle(a, b));
}

S2Point S2EdgeUtil::Interpolate(double t, S2Point const& a, S2Point const& b) {
  if (t == 0) return a;
  if (t == 1) return b;
  S1Angle ab(a, b);
  return InterpolateAtDistance(t * ab, a, b, ab);
}

S1Angle S2EdgeUtil::GetDistance(S2Point const& x,
                                S2Point const& a, S2Point const& b,
                                S2Point const& a_cross_b) {
  DCHECK(S2::IsUnitLength(a));
  DCHECK(S2::IsUnitLength(b));
  DCHECK(S2::IsUnitLength(x));

  // There are three cases.  If X is located in the spherical wedge defined by
  // A, B, and the axis A x B, then the closest point is on the segment AB.
  // Otherwise the closest point is either A or B; the dividing line between
  // these two cases is the great circle passing through (A x B) and the
  // midpoint of AB.

  if (S2::SimpleCCW(a_cross_b, a, x) && S2::SimpleCCW(x, b, a_cross_b)) {
    // The closest point to X lies on the segment AB.  We compute the distance
    // to the corresponding great circle.  The result is accurate for small
    // distances but not necessarily for large distances (approaching Pi/2).

    DCHECK_NE(a, b);  // Due to the guarantees of SimpleCCW().
    double sin_dist = fabs(x.DotProd(a_cross_b)) / a_cross_b.Norm();
    return S1Angle::Radians(asin(min(1.0, sin_dist)));
  }
  // Otherwise, the closest point is either A or B.  The cheapest method is
  // just to compute the minimum of the two linear (as opposed to spherical)
  // distances and convert the result to an angle.  Again, this method is
  // accurate for small but not large distances (approaching Pi).

  double linear_dist2 = min((x-a).Norm2(), (x-b).Norm2());
  return S1Angle::Radians(2 * asin(min(1.0, 0.5 * sqrt(linear_dist2))));
}

S1Angle S2EdgeUtil::GetDistance(S2Point const& x,
                                S2Point const& a, S2Point const& b) {
  return GetDistance(x, a, b, S2::RobustCrossProd(a, b));
}

S2Point S2EdgeUtil::GetClosestPoint(S2Point const& x,
                                    S2Point const& a, S2Point const& b,
                                    S2Point const& a_cross_b) {
  DCHECK(S2::IsUnitLength(a));
  DCHECK(S2::IsUnitLength(b));
  DCHECK(S2::IsUnitLength(x));

  // Find the closest point to X along the great circle through AB.
  S2Point p = x - (x.DotProd(a_cross_b) / a_cross_b.Norm2()) * a_cross_b;

  // If this point is on the edge AB, then it's the closest point.
  if (S2::SimpleCCW(a_cross_b, a, p) && S2::SimpleCCW(p, b, a_cross_b))
    return p.Normalize();

  // Otherwise, the closest point is either A or B.
  return ((x - a).Norm2() <= (x - b).Norm2()) ? a : b;
}

S2Point S2EdgeUtil::GetClosestPoint(S2Point const& x,
                                    S2Point const& a, S2Point const& b) {
  return GetClosestPoint(x, a, b, S2::RobustCrossProd(a, b));
}

bool S2EdgeUtil::IsEdgeBNearEdgeA(S2Point const& a0, S2Point const& a1,
                                  S2Point const& b0, S2Point const& b1,
                                  S1Angle const& tolerance) {
  DCHECK_LT(tolerance.radians(), M_PI / 2);
  DCHECK_GT(tolerance.radians(), 0);
  // The point on edge B=b0b1 furthest from edge A=a0a1 is either b0, b1, or
  // some interior point on B.  If it is an interior point on B, then it must be
  // one of the two points where the great circle containing B (circ(B)) is
  // furthest from the great circle containing A (circ(A)).  At these points,
  // the distance between circ(B) and circ(A) is the angle between the planes
  // containing them.

  S2Point a_ortho = S2::RobustCrossProd(a0, a1).Normalize();
  S2Point const a_nearest_b0 = GetClosestPoint(b0, a0, a1, a_ortho);
  S2Point const a_nearest_b1 = GetClosestPoint(b1, a0, a1, a_ortho);
  // If a_nearest_b0 and a_nearest_b1 have opposite orientation from a0 and a1,
  // we invert a_ortho so that it points in the same direction as a_nearest_b0 x
  // a_nearest_b1.  This helps us handle the case where A and B are oppositely
  // oriented but otherwise might be near each other.  We check orientation and
  // invert rather than computing a_nearest_b0 x a_nearest_b1 because those two
  // points might be equal, and have an unhelpful cross product.
  if (S2::RobustCCW(a_ortho, a_nearest_b0, a_nearest_b1) < 0)
    a_ortho *= -1;

  // To check if all points on B are within tolerance of A, we first check to
  // see if the endpoints of B are near A.  If they are not, B is not near A.
  S1Angle const b0_distance(b0, a_nearest_b0);
  S1Angle const b1_distance(b1, a_nearest_b1);
  if (b0_distance > tolerance || b1_distance > tolerance)
    return false;

  // If b0 and b1 are both within tolerance of A, we check to see if the angle
  // between the planes containing B and A is greater than tolerance.  If it is
  // not, no point on B can be further than tolerance from A (recall that we
  // already know that b0 and b1 are close to A, and S2Edges are all shorter
  // than 180 degrees).  The angle between the planes containing circ(A) and
  // circ(B) is the angle between their normal vectors.
  S2Point const b_ortho = S2::RobustCrossProd(b0, b1).Normalize();
  S1Angle const planar_angle(a_ortho, b_ortho);
  if (planar_angle <= tolerance)
    return true;


  // As planar_angle approaches M_PI, the projection of a_ortho onto the plane
  // of B approaches the null vector, and normalizing it is numerically
  // unstable.  This makes it unreliable or impossible to identify pairs of
  // points where circ(A) is furthest from circ(B).  At this point in the
  // algorithm, this can only occur for two reasons:
  //
  //  1.) b0 and b1 are closest to A at distinct endpoints of A, in which case
  //      the opposite orientation of a_ortho and b_ortho means that A and B are
  //      in opposite hemispheres and hence not close to each other.
  //
  //  2.) b0 and b1 are closest to A at the same endpoint of A, in which case
  //      the orientation of a_ortho was chosen arbitrarily to be that of a0
  //      cross a1.  B must be shorter than 2*tolerance and all points in B are
  //      close to one endpoint of A, and hence to A.
  //
  // The logic applies when planar_angle is robustly greater than M_PI/2, but
  // may be more computationally expensive than the logic beyond, so we choose a
  // value close to M_PI.
  if (planar_angle >= S1Angle::Radians(M_PI - 0.01)) {
    return (S1Angle(b0, a0) < S1Angle(b0, a1)) ==
        (S1Angle(b1, a0) < S1Angle(b1, a1));
  }

  // Finally, if either of the two points on circ(B) where circ(B) is furthest
  // from circ(A) lie on edge B, edge B is not near edge A.
  //
  // The normalized projection of a_ortho onto the plane of circ(B) is one of
  // the two points along circ(B) where it is furthest from circ(A).  The other
  // is -1 times the normalized projection.
  S2Point furthest = (a_ortho - a_ortho.DotProd(b_ortho) * b_ortho).Normalize();
  DCHECK(S2::IsUnitLength(furthest));
  S2Point furthest_inv = -1 * furthest;

  // A point p lies on B if you can proceed from b_ortho to b0 to p to b1 and
  // back to b_ortho without ever turning right.  We test this for furthest and
  // furthest_inv, and return true if neither point lies on B.
  return !((S2::RobustCCW(b_ortho, b0, furthest) > 0 &&
            S2::RobustCCW(furthest, b1, b_ortho) > 0) ||
           (S2::RobustCCW(b_ortho, b0, furthest_inv) > 0 &&
            S2::RobustCCW(furthest_inv, b1, b_ortho) > 0));
}


bool S2EdgeUtil::WedgeContains(S2Point const& a0, S2Point const& ab1,
                               S2Point const& a2, S2Point const& b0,
                               S2Point const& b2) {
  // For A to contain B (where each loop interior is defined to be its left
  // side), the CCW edge order around ab1 must be a2 b2 b0 a0.  We split
  // this test into two parts that test three vertices each.
  return S2::OrderedCCW(a2, b2, b0, ab1) && S2::OrderedCCW(b0, a0, a2, ab1);
}

bool S2EdgeUtil::WedgeIntersects(S2Point const& a0, S2Point const& ab1,
                                 S2Point const& a2, S2Point const& b0,
                                 S2Point const& b2) {
  // For A not to intersect B (where each loop interior is defined to be
  // its left side), the CCW edge order around ab1 must be a0 b2 b0 a2.
  // Note that it's important to write these conditions as negatives
  // (!OrderedCCW(a,b,c,o) rather than Ordered(c,b,a,o)) to get correct
  // results when two vertices are the same.
  return !(S2::OrderedCCW(a0, b2, b0, ab1) && S2::OrderedCCW(b0, a2, a0, ab1));
}

S2EdgeUtil::WedgeRelation S2EdgeUtil::GetWedgeRelation(
    S2Point const& a0, S2Point const& ab1, S2Point const& a2,
    S2Point const& b0, S2Point const& b2) {
  // There are 6 possible edge orderings at a shared vertex (all
  // of these orderings are circular, i.e. abcd == bcda):
  //
  //  (1) a2 b2 b0 a0: A contains B
  //  (2) a2 a0 b0 b2: B contains A
  //  (3) a2 a0 b2 b0: A and B are disjoint
  //  (4) a2 b0 a0 b2: A and B intersect in one wedge
  //  (5) a2 b2 a0 b0: A and B intersect in one wedge
  //  (6) a2 b0 b2 a0: A and B intersect in two wedges
  //
  // We do not distinguish between 4, 5, and 6.
  // We pay extra attention when some of the edges overlap.When edges
  // overlap, several of these orderings can be satisfied, and we take
  // the most specific.
  if (a0 == b0 && a2 == b2) return WEDGE_EQUALS;

  if (S2::OrderedCCW(a0, a2, b2, ab1)) {
    // The cases with this vertex ordering are 1, 5, and 6,
    // although case 2 is also possible if a2 == b2.
    if (S2::OrderedCCW(b2, b0, a0, ab1)) return WEDGE_PROPERLY_CONTAINS;

    // We are in case 5 or 6, or case 2 if a2 == b2.
    return (a2 == b2) ? WEDGE_IS_PROPERLY_CONTAINED : WEDGE_PROPERLY_OVERLAPS;
  }

  // We are in case 2, 3, or 4.
  if (S2::OrderedCCW(a0, b0, b2, ab1)) return WEDGE_IS_PROPERLY_CONTAINED;
  return S2::OrderedCCW(a0, b0, a2, ab1) ?
      WEDGE_IS_DISJOINT : WEDGE_PROPERLY_OVERLAPS;
}

int S2EdgeUtil::EdgeCrosser::RobustCrossingInternal(S2Point const* d) {
  // ACB and BDA have the appropriate orientations, so now we check the
  // triangles CBD and DAC.
  S2Point c_cross_d = c_->CrossProd(*d);
  int cbd = -S2::RobustCCW(*c_, *d, *b_, c_cross_d);
  if (cbd != acb_) return -1;

  int dac = S2::RobustCCW(*c_, *d, *a_, c_cross_d);
  return (dac == acb_) ? 1 : -1;
}

void S2EdgeUtil::RectBounder::AddPoint(S2Point const* b) {
  DCHECK(S2::IsUnitLength(*b));
  S2LatLng b_latlng(*b);
  if (bound_.is_empty()) {
    bound_.AddPoint(b_latlng);
  } else {
    // We can't just call bound_.AddPoint(b_latlng) here, since we need to
    // ensure that all the longitudes between "a" and "b" are included.
    bound_ = bound_.Union(S2LatLngRect::FromPointPair(a_latlng_, b_latlng));

    // Check whether the min/max latitude occurs in the edge interior.  We find
    // the normal to the plane containing AB, and then a vector "dir" in this
    // plane that also passes through the equator.  We use RobustCrossProd to
    // ensure that the edge normal is accurate even when the two points are very
    // close together.
    S2Point a_cross_b = S2::RobustCrossProd(*a_, *b);
    S2Point dir = a_cross_b.CrossProd(S2Point(0, 0, 1));
    double da = dir.DotProd(*a_);
    double db = dir.DotProd(*b);
    if (da * db < 0) {
      // Minimum/maximum latitude occurs in the edge interior.
      double abs_lat = acos(fabs(a_cross_b[2] / a_cross_b.Norm()));
      if (da < 0) {
        // It's possible that abs_lat < lat_.lo() due to numerical errors.
        bound_.mutable_lat()->set_hi(max(abs_lat, bound_.lat().hi()));
      } else {
        bound_.mutable_lat()->set_lo(min(-abs_lat, bound_.lat().lo()));
      }

      // If the edge comes very close to the north or south pole then we may
      // not be certain which side of the pole it is on.  We handle this by
      // expanding the longitude bounds if the maximum absolute latitude is
      // approximately Pi/2.
      if (abs_lat >= M_PI_2 - 1e-15) {
        *bound_.mutable_lng() = S1Interval::Full();
      }
    }
  }
  a_ = b;
  a_latlng_ = b_latlng;
}

S2EdgeUtil::LongitudePruner::LongitudePruner(S1Interval const& interval,
                                             S2Point const& v0)
  : interval_(interval), lng0_(S2LatLng::Longitude(v0).radians()) {
}
