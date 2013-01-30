// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S2EDGEUTIL_H__
#define UTIL_GEOMETRY_S2EDGEUTIL_H__

#include "base/logging.h"
#include "base/macros.h"
#include "s2.h"
#include "s2latlngrect.h"

class S2LatLngRect;

// This class contains various utility functions related to edges.  It
// collects together common code that is needed to implement polygonal
// geometry such as polylines, loops, and general polygons.
class S2EdgeUtil {
 public:
  // This class allows a vertex chain v0, v1, v2, ... to be efficiently
  // tested for intersection with a given fixed edge AB.
  class EdgeCrosser {
   public:
    // AB is the given fixed edge, and C is the first vertex of the vertex
    // chain.  All parameters must point to fixed storage that persists for
    // the lifetime of the EdgeCrosser object.
    inline EdgeCrosser(S2Point const* a, S2Point const* b, S2Point const* c);

    // Call this function when your chain 'jumps' to a new place.
    inline void RestartAt(S2Point const* c);

    // This method is equivalent to calling the S2EdgeUtil::RobustCrossing()
    // function (defined below) on the edges AB and CD.  It returns +1 if
    // there is a crossing, -1 if there is no crossing, and 0 if two points
    // from different edges are the same.  Returns 0 or -1 if either edge is
    // degenerate.  As a side effect, it saves vertex D to be used as the next
    // vertex C.
    inline int RobustCrossing(S2Point const* d);

    // This method is equivalent to the S2EdgeUtil::EdgeOrVertexCrossing()
    // method defined below.  It is similar to RobustCrossing, but handles
    // cases where two vertices are identical in a way that makes it easy to
    // implement point-in-polygon containment tests.
    inline bool EdgeOrVertexCrossing(S2Point const* d);

   private:
    // This function handles the "slow path" of RobustCrossing(), which does
    // not need to be inlined.
    int RobustCrossingInternal(S2Point const* d);

    // The fields below are all constant.
    S2Point const* const a_;
    S2Point const* const b_;
    S2Point const a_cross_b_;

    // The fields below are updated for each vertex in the chain.
    S2Point const* c_;     // Previous vertex in the vertex chain.
    int acb_;              // The orientation of the triangle ACB.
  };

  // This class computes a bounding rectangle that contains all edges
  // defined by a vertex chain v0, v1, v2, ...  All vertices must be unit
  // length.  Note that the bounding rectangle of an edge can be larger than
  // the bounding rectangle of its endpoints, e.g. consider an edge that
  // passes through the north pole.
  class RectBounder {
   public:
    RectBounder() : bound_(S2LatLngRect::Empty()) {}

    // This method is called to add each vertex to the chain.  'b'
    // must point to fixed storage that persists through the next call
    // to AddPoint.  This means that if you don't store all of your
    // points for the lifetime of the bounder, you must at least store
    // the last two points and alternate which one you use for the
    // next point.
    void AddPoint(S2Point const* b);

    // Return the bounding rectangle of the edge chain that connects the
    // vertices defined so far.
    S2LatLngRect GetBound() const { return bound_; }

   private:
    S2Point const* a_;      // The previous vertex in the chain.
    S2LatLng a_latlng_;     // The corresponding latitude-longitude.
    S2LatLngRect bound_;    // The current bounding rectangle.
  };

  // The purpose of this class is to find edges that intersect a given
  // longitude interval.  It can be used as an efficient rejection test when
  // attempting to find edges that intersect a given region.  It accepts a
  // vertex chain v0, v1, v2, ...  and returns a boolean value indicating
  // whether each edge intersects the specified longitude interval.
  class LongitudePruner {
   public:
    // 'interval' is the longitude interval to be tested against, and
    // 'v0' is the first vertex of edge chain.
    LongitudePruner(S1Interval const& interval, S2Point const& v0);

    // Returns true if the edge (v0, v1) intersects the given longitude
    // interval, and then saves 'v1' to be used as the next 'v0'.
    inline bool Intersects(S2Point const& v1);

   private:
    S1Interval interval_;    // The interval to be tested against.
    double lng0_;            // The longitude of the next v0.
  };

  // Return true if edge AB crosses CD at a point that is interior
  // to both edges.  Properties:
  //
  //  (1) SimpleCrossing(b,a,c,d) == SimpleCrossing(a,b,c,d)
  //  (2) SimpleCrossing(c,d,a,b) == SimpleCrossing(a,b,c,d)
  static bool SimpleCrossing(S2Point const& a, S2Point const& b,
                             S2Point const& c, S2Point const& d);

  // Like SimpleCrossing, except that points that lie exactly on a line are
  // arbitrarily classified as being on one side or the other (according to
  // the rules of S2::RobustCCW).  It returns +1 if there is a crossing, -1
  // if there is no crossing, and 0 if any two vertices from different edges
  // are the same.  Returns 0 or -1 if either edge is degenerate.
  // Properties of RobustCrossing:
  //
  //  (1) RobustCrossing(b,a,c,d) == RobustCrossing(a,b,c,d)
  //  (2) RobustCrossing(c,d,a,b) == RobustCrossing(a,b,c,d)
  //  (3) RobustCrossing(a,b,c,d) == 0 if a==c, a==d, b==c, b==d
  //  (3) RobustCrossing(a,b,c,d) <= 0 if a==b or c==d
  //
  // Note that if you want to check an edge against a *chain* of other
  // edges, it is much more efficient to use an EdgeCrosser (above).
  static int RobustCrossing(S2Point const& a, S2Point const& b,
                            S2Point const& c, S2Point const& d);

  // Given two edges AB and CD where at least two vertices are identical
  // (i.e. RobustCrossing(a,b,c,d) == 0), this function defines whether the
  // two edges "cross" in a such a way that point-in-polygon containment tests
  // can be implemented by counting the number of edge crossings.  The basic
  // rule is that a "crossing" occurs if AB is encountered after CD during a
  // CCW sweep around the shared vertex starting from a fixed reference point.
  //
  // Note that according to this rule, if AB crosses CD then in general CD
  // does not cross AB.  However, this leads to the correct result when
  // counting polygon edge crossings.  For example, suppose that A,B,C are
  // three consecutive vertices of a CCW polygon.  If we now consider the edge
  // crossings of a segment BP as P sweeps around B, the crossing number
  // changes parity exactly when BP crosses BA or BC.
  //
  // Useful properties of VertexCrossing (VC):
  //
  //  (1) VC(a,a,c,d) == VC(a,b,c,c) == false
  //  (2) VC(a,b,a,b) == VC(a,b,b,a) == true
  //  (3) VC(a,b,c,d) == VC(a,b,d,c) == VC(b,a,c,d) == VC(b,a,d,c)
  //  (3) If exactly one of a,b equals one of c,d, then exactly one of
  //      VC(a,b,c,d) and VC(c,d,a,b) is true
  //
  // It is an error to call this method with 4 distinct vertices.
  static bool VertexCrossing(S2Point const& a, S2Point const& b,
                             S2Point const& c, S2Point const& d);

  // A convenience function that calls RobustCrossing() to handle cases
  // where all four vertices are distinct, and VertexCrossing() to handle
  // cases where two or more vertices are the same.  This defines a crossing
  // function such that point-in-polygon containment tests can be implemented
  // by simply counting edge crossings.
  static bool EdgeOrVertexCrossing(S2Point const& a, S2Point const& b,
                                   S2Point const& c, S2Point const& d);

  // Given two edges AB and CD such that RobustCrossing() is true, return
  // their intersection point.  Useful properties of GetIntersection (GI):
  //
  //  (1) GI(b,a,c,d) == GI(a,b,d,c) == GI(a,b,c,d)
  //  (2) GI(c,d,a,b) == GI(a,b,c,d)
  //
  // The returned intersection point X is guaranteed to be close to the edges
  // AB and CD, but if the edges intersect at a very small angle then X may
  // not be close to the true mathematical intersection point P.  See the
  // description of "kIntersectionTolerance" below for details.
  static S2Point GetIntersection(S2Point const& a, S2Point const& b,
                                 S2Point const& c, S2Point const& d);

  // This distance is an upper bound on the distance from the intersection
  // point returned by GetIntersection() to either of the two edges that were
  // intersected.  In particular, if "x" is the intersection point, then
  // GetDistance(x, a, b) and GetDistance(x, c, d) will both be smaller than
  // this value.  The intersection tolerance is also large enough such if it
  // is passed as the "vertex_merge_radius" of an S2PolygonBuilder, then the
  // intersection point will be spliced into the edges AB and/or CD if they
  // are also supplied to the S2PolygonBuilder.
  static S1Angle const kIntersectionTolerance;

  // Given a point X and an edge AB, return the distance ratio AX / (AX + BX).
  // If X happens to be on the line segment AB, this is the fraction "t" such
  // that X == Interpolate(A, B, t).  Requires that A and B are distinct.
  static double GetDistanceFraction(S2Point const& x,
                                    S2Point const& a, S2Point const& b);

  // Return the point X along the line segment AB whose distance from A is the
  // given fraction "t" of the distance AB.  Does NOT require that "t" be
  // between 0 and 1.  Note that all distances are measured on the surface of
  // the sphere, so this is more complicated than just computing (1-t)*a + t*b
  // and normalizing the result.
  static S2Point Interpolate(double t, S2Point const& a, S2Point const& b);

  // Like Interpolate(), except that the parameter "ax" represents the desired
  // distance from A to the result X rather than a fraction between 0 and 1.
  static S2Point InterpolateAtDistance(S1Angle const& ax,
                                       S2Point const& a, S2Point const& b);

  // A slightly more efficient version of InterpolateAtDistance() that can be
  // used when the distance AB is already known.
  static S2Point InterpolateAtDistance(S1Angle const& ax,
                                       S2Point const& a, S2Point const& b,
                                       S1Angle const& ab);

  // Return the minimum distance from X to any point on the edge AB.  All
  // arguments should be unit length.  The result is very accurate for small
  // distances but may have some numerical error if the distance is large
  // (approximately Pi/2 or greater).  The case A == B is handled correctly.
  static S1Angle GetDistance(S2Point const& x,
                             S2Point const& a, S2Point const& b);

  // A slightly more efficient version of GetDistance() where the cross
  // product of the two endpoints has been precomputed.  The cross product
  // does not need to be normalized, but should be computed using
  // S2::RobustCrossProd() for the most accurate results.
  static S1Angle GetDistance(S2Point const& x,
                             S2Point const& a, S2Point const& b,
                             S2Point const& a_cross_b);

  // Return the point along the edge AB that is closest to the point X.
  // The fractional distance of this point along the edge AB can be obtained
  // using GetDistanceFraction() above.
  static S2Point GetClosestPoint(S2Point const& x,
                                 S2Point const& a, S2Point const& b);

  // A slightly more efficient version of GetClosestPoint() where the cross
  // product of the two endpoints has been precomputed.  The cross product
  // does not need to be normalized, but should be computed using
  // S2::RobustCrossProd() for the most accurate results.
  static S2Point GetClosestPoint(S2Point const& x,
                                 S2Point const& a, S2Point const& b,
                                 S2Point const& a_cross_b);

  // Return true if every point on edge B=b0b1 is no further than "tolerance"
  // from some point on edge A=a0a1.
  // Requires that tolerance is less than 90 degrees.
  static bool IsEdgeBNearEdgeA(S2Point const& a0, S2Point const& a1,
                               S2Point const& b0, S2Point const& b1,
                               S1Angle const& tolerance);

  // For an edge chain (x0, x1, x2), a wedge is the region to the left
  // of the edges. More precisely, it is the union of all the rays
  // from x1x0 to x1x2, clockwise.
  // The following are Wedge comparison functions for two wedges A =
  // (a0, ab1, a2) and B = (b0, a12, b2). These are used in S2Loops.

  // Returns true if wedge A fully contains or is equal to wedge B.
  static bool WedgeContains(S2Point const& a0, S2Point const& ab1,
                            S2Point const& a2, S2Point const& b0,
                            S2Point const& b2);

  // Returns true if the intersection of the two wedges is not empty.
  static bool WedgeIntersects(S2Point const& a0, S2Point const& ab1,
                              S2Point const& a2, S2Point const& b0,
                              S2Point const& b2);

  // Detailed relation from wedges A to wedge B.
  enum WedgeRelation {
    WEDGE_EQUALS,
    WEDGE_PROPERLY_CONTAINS,  // A is a strict superset of B.
    WEDGE_IS_PROPERLY_CONTAINED,  // A is a strict subset of B.
    WEDGE_PROPERLY_OVERLAPS,  // All of A intsect B, A-B and B-A are non-empty.
    WEDGE_IS_DISJOINT,  // A is disjoint from B
  };

  // Return the relation from wedge A to B.
  static WedgeRelation GetWedgeRelation(
      S2Point const& a0, S2Point const& ab1, S2Point const& a2,
      S2Point const& b0, S2Point const& b2);

  DISALLOW_IMPLICIT_CONSTRUCTORS(S2EdgeUtil);  // Contains only static methods.
};

inline S2EdgeUtil::EdgeCrosser::EdgeCrosser(
    S2Point const* a, S2Point const* b, S2Point const* c)
    : a_(a), b_(b), a_cross_b_(a_->CrossProd(*b_)) {
  RestartAt(c);
}

inline void S2EdgeUtil::EdgeCrosser::RestartAt(S2Point const* c) {
  c_ = c;
  acb_ = -S2::RobustCCW(*a_, *b_, *c_, a_cross_b_);
}

inline int S2EdgeUtil::EdgeCrosser::RobustCrossing(S2Point const* d) {
  // For there to be an edge crossing, the triangles ACB, CBD, BDA, DAC must
  // all be oriented the same way (CW or CCW).  We keep the orientation of ACB
  // as part of our state.  When each new point D arrives, we compute the
  // orientation of BDA and check whether it matches ACB.  This checks whether
  // the points C and D are on opposite sides of the great circle through AB.

  // Recall that RobustCCW is invariant with respect to rotating its
  // arguments, i.e. ABC has the same orientation as BDA.
  int bda = S2::RobustCCW(*a_, *b_, *d, a_cross_b_);
  int result;
  if (bda == -acb_ && bda != 0) {
    result = -1;  // Most common case -- triangles have opposite orientations.
  } else if ((bda & acb_) == 0) {
    result = 0;  // At least one value is zero -- two vertices are identical.
  } else {  // Slow path.
    DCHECK_EQ(acb_, bda);
    DCHECK_NE(0, bda);
    result = RobustCrossingInternal(d);
  }
  // Now save the current vertex D as the next vertex C, and also save the
  // orientation of the new triangle ACB (which is opposite to the current
  // triangle BDA).
  c_ = d;
  acb_ = -bda;
  return result;
}

inline bool S2EdgeUtil::EdgeCrosser::EdgeOrVertexCrossing(S2Point const* d) {
  // We need to copy c_ since it is clobbered by RobustCrossing().
  S2Point const* c = c_;
  int crossing = RobustCrossing(d);
  if (crossing < 0) return false;
  if (crossing > 0) return true;
  return VertexCrossing(*a_, *b_, *c, *d);
}

inline bool S2EdgeUtil::LongitudePruner::Intersects(S2Point const& v1) {
  double lng1 = S2LatLng::Longitude(v1).radians();
  bool result = interval_.Intersects(S1Interval::FromPointPair(lng0_, lng1));
  lng0_ = lng1;
  return result;
}

#endif  // UTIL_GEOMETRY_S2EDGEUTIL_H__
