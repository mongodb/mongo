// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S2POLYLINE_H__
#define UTIL_GEOMETRY_S2POLYLINE_H__

#include <vector>
using std::vector;

#include "base/logging.h"
#include "base/macros.h"
#include "s2.h"
#include "s2region.h"
#include "s2latlngrect.h"

class S1Angle;

// An S2Polyline represents a sequence of zero or more vertices connected by
// straight edges (geodesics).  Edges of length 0 and 180 degrees are not
// allowed, i.e. adjacent vertices should not be identical or antipodal.
class S2Polyline : public S2Region {
 public:
  // Creates an empty S2Polyline that should be initialized by calling Init()
  // or Decode().
  S2Polyline();

  // Convenience constructors that call Init() with the given vertices.
  S2Polyline(vector<S2Point> const& vertices);
  S2Polyline(vector<S2LatLng> const& vertices);

  // Initialize a polyline that connects the given vertices. Empty polylines are
  // allowed.  Adjacent vertices should not be identical or antipodal.  All
  // vertices should be unit length.
  void Init(vector<S2Point> const& vertices);

  // Convenience initialization function that accepts latitude-longitude
  // coordinates rather than S2Points.
  void Init(vector<S2LatLng> const& vertices);

  ~S2Polyline();

  // Return true if the given vertices form a valid polyline.
  // Output errors to "err" if it's not NULL.
  static bool IsValid(vector<S2Point> const& vertices, string* err = NULL);

  int num_vertices() const { return num_vertices_; }
  S2Point const& vertex(int k) const {
    DCHECK_GE(k, 0);
    return vertices_[k];
  }

  // Return the length of the polyline.
  S1Angle GetLength() const;

  // Return the true centroid of the polyline multiplied by the length of the
  // polyline (see s2.h for details on centroids).  The result is not unit
  // length, so you may want to normalize it.
  //
  // Prescaling by the polyline length makes it easy to compute the centroid
  // of several polylines (by simply adding up their centroids).
  S2Point GetCentroid() const;

  // Return the point whose distance from vertex 0 along the polyline is the
  // given fraction of the polyline's total length.  Fractions less than zero
  // or greater than one are clamped.  The return value is unit length.  This
  // cost of this function is currently linear in the number of vertices.
  // The polyline must not be empty.
  S2Point Interpolate(double fraction) const;

  // Like Interpolate(), but also return the index of the next polyline
  // vertex after the interpolated point P.  This allows the caller to easily
  // construct a given suffix of the polyline by concatenating P with the
  // polyline vertices starting at "next_vertex".  Note that P is guaranteed
  // to be different than vertex(*next_vertex), so this will never result in
  // a duplicate vertex.
  //
  // The polyline must not be empty.  Note that if "fraction" >= 1.0, then
  // "next_vertex" will be set to num_vertices() (indicating that no vertices
  // from the polyline need to be appended).  The value of "next_vertex" is
  // always between 1 and num_vertices().
  //
  // This method can also be used to construct a prefix of the polyline, by
  // taking the polyline vertices up to "next_vertex - 1" and appending the
  // returned point P if it is different from the last vertex (since in this
  // case there is no guarantee of distinctness).
  S2Point GetSuffix(double fraction, int* next_vertex) const;

  // The inverse operation of GetSuffix/Interpolate.  Given a point on the
  // polyline, returns the ratio of the distance to the point from the
  // beginning of the polyline over the length of the polyline.  The return
  // value is always betwen 0 and 1 inclusive.  See GetSuffix() for the
  // meaning of "next_vertex".
  //
  // The polyline should not be empty.  If it has fewer than 2 vertices, the
  // return value is zero.
  double UnInterpolate(S2Point const& point, int next_vertex) const;

  // Given a point, returns a point on the polyline that is closest to the given
  // point.  See GetSuffix() for the meaning of "next_vertex", which is chosen
  // here w.r.t. the projected point as opposed to the interpolated point in
  // GetSuffix().
  //
  // The polyline must be non-empty.
  S2Point Project(S2Point const& point, int* next_vertex) const;

  // Returns true if the point given is on the right hand side of the polyline,
  // using a naive definition of "right-hand-sideness" where the point is on
  // the RHS of the polyline iff the point is on the RHS of the line segment in
  // the polyline which it is closest to.
  //
  // The polyline must have at least 2 vertices.
  bool IsOnRight(S2Point const& point) const;

  // Return true if this polyline intersects the given polyline. If the
  // polylines share a vertex they are considered to be intersecting. When a
  // polyline endpoint is the only intersection with the other polyline, the
  // function may return true or false arbitrarily.
  //
  // The running time is quadratic in the number of vertices.
  bool Intersects(S2Polyline const* line) const;

  // Reverse the order of the polyline vertices.
  void Reverse();

  // Return a subsequence of vertex indices such that the polyline connecting
  // these vertices is never further than "tolerance" from the original
  // polyline.  The first and last vertices are always preserved.
  //
  // Some useful properties of the algorithm:
  //
  //  - It runs in linear time.
  //
  //  - The output is always a valid polyline.  In particular, adjacent
  //    output vertices are never identical or antipodal.
  //
  //  - The method is not optimal, but it tends to produce 2-3% fewer
  //    vertices than the Douglas-Peucker algorithm with the same tolerance.
  //
  //  - The output is *parametrically* equivalent to the original polyline to
  //    within the given tolerance.  For example, if a polyline backtracks on
  //    itself and then proceeds onwards, the backtracking will be preserved
  //    (to within the given tolerance).  This is different than the
  //    Douglas-Peucker algorithm used in maps/util/geoutil-inl.h, which only
  //    guarantees geometric equivalence.
  void SubsampleVertices(S1Angle const& tolerance, vector<int>* indices) const;

  // Return true if two polylines have the same number of vertices, and
  // corresponding vertex pairs are separated by no more than "max_error".
  // (For testing purposes.)
  bool ApproxEquals(S2Polyline const* b, double max_error = 1e-15) const;

  // Return true if "covered" is within "max_error" of a contiguous subpath of
  // this polyline over its entire length.  Specifically, this method returns
  // true if this polyline has parameterization a:[0,1] -> S^2, "covered" has
  // parameterization b:[0,1] -> S^2, and there is a non-decreasing function
  // f:[0,1] -> [0,1] such that distance(a(f(t)), b(t)) <= max_error for all t.
  //
  // You can think of this as testing whether it is possible to drive a car
  // along "covered" and a car along some subpath of this polyline such that no
  // car ever goes backward, and the cars are always within "max_error" of each
  // other.
  bool NearlyCoversPolyline(S2Polyline const& covered,
                            S1Angle const& max_error) const;

  ////////////////////////////////////////////////////////////////////////
  // S2Region interface (see s2region.h for details):

  virtual S2Polyline* Clone() const;
  virtual S2Cap GetCapBound() const;
  virtual S2LatLngRect GetRectBound() const;
  virtual bool Contains(S2Cell const& cell) const { return false; }
  virtual bool MayIntersect(S2Cell const& cell) const;

  // Polylines do not have a Contains(S2Point) method, because "containment"
  // is not numerically well-defined except at the polyline vertices.
  virtual bool VirtualContainsPoint(S2Point const& p) const { return false; }

  virtual void Encode(Encoder* const encoder) const;
  virtual bool Decode(Decoder* const decoder);

 private:
  // Internal constructor used only by Clone() that makes a deep copy of
  // its argument.
  S2Polyline(S2Polyline const* src);

  // We store the vertices in an array rather than a vector because we don't
  // need any STL methods, and computing the number of vertices using size()
  // would be relatively expensive (due to division by sizeof(S2Point) == 24).
  int num_vertices_;
  S2Point* vertices_;

  DISALLOW_EVIL_CONSTRUCTORS(S2Polyline);
};

#endif  // UTIL_GEOMETRY_S2POLYLINE_H__
