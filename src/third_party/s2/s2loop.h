// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S2LOOP_H__
#define UTIL_GEOMETRY_S2LOOP_H__

#include <map>
using std::map;
using std::multimap;

#include <vector>
using std::vector;


#include "base/logging.h"
#include "base/macros.h"
#include "s2edgeindex.h"
#include "s2region.h"
#include "s2latlngrect.h"
#include "s2edgeutil.h"

class S2Loop;
// Defined in the cc file. A helper class for AreBoundariesCrossing.
class WedgeProcessor;

// Indexing structure to efficiently compute intersections.
class S2LoopIndex: public S2EdgeIndex {
 public:
  explicit S2LoopIndex(S2Loop const* loop): loop_(loop) {}
  virtual ~S2LoopIndex() {}

  // There is no need to overwrite Reset(), as the only data that's
  // used to implement this class is all contained in the loop data.
  // void Reset();

  virtual S2Point const* edge_from(int index) const;
  virtual S2Point const* edge_to(int index) const;
  virtual int num_edges() const;

 private:
  S2Loop const* loop_;
};

// An S2Loop represents a simple spherical polygon.  It consists of a single
// chain of vertices where the first vertex is implicitly connected to the
// last.  All loops are defined to have a CCW orientation, i.e. the interior
// of the polygon is on the left side of the edges.  This implies that a
// clockwise loop enclosing a small area is interpreted to be a CCW loop
// enclosing a very large area.
//
// Loops are not allowed to have any duplicate vertices (whether adjacent or
// not), and non-adjacent edges are not allowed to intersect.  Loops must have
// at least 3 vertices.  Although these restrictions are not enforced in
// optimized code, you may get unexpected results if they are violated.
//
// Point containment is defined such that if the sphere is subdivided into
// faces (loops), every point is contained by exactly one face.  This implies
// that loops do not necessarily contain all (or any) of their vertices.
//
// TODO(user): When doing operations on two loops, always create the
// edgeindex for the bigger of the two.  Same for polygons.
class S2Loop : public S2Region {
 public:
  // Create an empty S2Loop that should be initialized by calling Init() or
  // Decode().
  S2Loop();

  // Convenience constructor that calls Init() with the given vertices.
  explicit S2Loop(vector<S2Point> const& vertices);

  // Initialize a loop connecting the given vertices.  The last vertex is
  // implicitly connected to the first.  All points should be unit length.
  // Loops must have at least 3 vertices.
  void Init(vector<S2Point> const& vertices);

  // This parameter should be removed as soon as people stop using the
  // deprecated version of IsValid.
  static int const kDefaultMaxAdjacent = 0;

  // Check whether this loop is valid.  Note that in debug mode, validity
  // is checked at loop creation time, so IsValid()
  // should always return true.
  // If err is not NULL, output errors into it.
  bool IsValid(string* err = NULL) const;


  // These two versions are deprecated and ignore max_adjacent.
  // DEPRECATED.
  static bool IsValid(vector<S2Point> const& vertices, int max_adjacent);
  // DEPRECATED.
  bool IsValid(int max_adjacent) const;

  // Initialize a loop corresponding to the given cell.
  explicit S2Loop(S2Cell const& cell);

  ~S2Loop();

  // The depth of a loop is defined as its nesting level within its containing
  // polygon.  "Outer shell" loops have depth 0, holes within those loops have
  // depth 1, shells within those holes have depth 2, etc.  This field is only
  // used by the S2Polygon implementation.
  int depth() const { return depth_; }
  void set_depth(int depth) { depth_ = depth; }

  // Return true if this loop represents a hole in its containing polygon.
  bool is_hole() const { return (depth_ & 1) != 0; }

  // The sign of a loop is -1 if the loop represents a hole in its containing
  // polygon, and +1 otherwise.
  int sign() const { return is_hole() ? -1 : 1; }

  int num_vertices() const { return num_vertices_; }

  // For convenience, we make two entire copies of the vertex list available:
  // vertex(n..2*n-1) is mapped to vertex(0..n-1), where n == num_vertices().
  S2Point const& vertex(int i) const {
    DCHECK_GE(i, 0);
    DCHECK_LT(i, (2 * num_vertices_));
    int j = i - num_vertices();
    return vertices_[j >= 0 ? j : i];
  }

  // Return true if the loop area is at most 2*Pi.  Degenerate loops are
  // handled consistently with S2::RobustCCW(), i.e., if a loop can be
  // expressed as the union of degenerate or nearly-degenerate CCW triangles,
  // then it will always be considered normalized.
  bool IsNormalized() const;

  // Invert the loop if necessary so that the area enclosed by the loop is at
  // most 2*Pi.
  void Normalize();

  // Reverse the order of the loop vertices, effectively complementing
  // the region represented by the loop.
  void Invert();

  // Return the area of the loop interior, i.e. the region on the left side of
  // the loop.  The return value is between 0 and 4*Pi.  (Note that the return
  // value is not affected by whether this loop is a "hole" or a "shell".)
  double GetArea() const;

  // Return the true centroid of the loop multiplied by the area of the loop
  // (see s2.h for details on centroids).  The result is not unit length, so
  // you may want to normalize it.  Also note that in general, the centroid
  // may not be contained by the loop.
  //
  // We prescale by the loop area for two reasons: (1) it is cheaper to
  // compute this way, and (2) it makes it easier to compute the centroid of
  // more complicated shapes (by splitting them into disjoint regions and
  // adding their centroids).
  //
  // Note that the return value is not affected by whether this loop is a
  // "hole" or a "shell".
  S2Point GetCentroid() const;

  // Return the sum of the turning angles at each vertex.  The return value is
  // positive if the loop is counter-clockwise, negative if the loop is
  // clockwise, and zero if the loop is a great circle.  Degenerate and
  // nearly-degenerate loops are handled consistently with S2::RobustCCW().
  // So for example, if a loop has zero area (i.e., it is a very small CCW
  // loop) then the turning angle will always be negative.
  //
  // This quantity is also called the "geodesic curvature" of the loop.
  double GetTurningAngle() const;

  // Return true if the region contained by this loop is a superset of the
  // region contained by the given other loop.
  bool Contains(S2Loop const* b) const;

  // Return true if the region contained by this loop intersects the region
  // contained by the given other loop.
  bool Intersects(S2Loop const* b) const;

  // Given two loops of a polygon (see s2polygon.h for requirements), return
  // true if A contains B.  This version of Contains() is much cheaper since
  // it does not need to check whether the boundaries of the two loops cross.
  bool ContainsNested(S2Loop const* b) const;

  // Return +1 if A contains B (i.e. the interior of B is a subset of the
  // interior of A), -1 if the boundaries of A and B cross, and 0 otherwise.
  // Requires that A does not properly contain the complement of B, i.e.
  // A and B do not contain each other's boundaries.  This method is used
  // for testing whether multi-loop polygons contain each other.
  //
  // WARNING: there is a bug in this function - it does not detect loop
  // crossings in certain situations involving shared edges.  CL 2926852 works
  // around this bug for polygon intersection, but it probably effects other
  // tests.  TODO: fix ContainsOrCrosses() and revert CL 2926852.
  int ContainsOrCrosses(S2Loop const* b) const;

  // Return true if two loops have the same boundary.  This is true if and
  // only if the loops have the same vertices in the same cyclic order.
  // (For testing purposes.)
  bool BoundaryEquals(S2Loop const* b) const;

  // Return true if two loops have the same boundary except for vertex
  // perturbations.  More precisely, the vertices in the two loops must be in
  // the same cyclic order, and corresponding vertex pairs must be separated
  // by no more than "max_error".  (For testing purposes.)
  bool BoundaryApproxEquals(S2Loop const* b, double max_error = 1e-15) const;

  // Return true if the two loop boundaries are within "max_error" of each
  // other along their entire lengths.  The two loops may have different
  // numbers of vertices.  More precisely, this method returns true if the two
  // loops have parameterizations a:[0,1] -> S^2, b:[0,1] -> S^2 such that
  // distance(a(t), b(t)) <= max_error for all t.  You can think of this as
  // testing whether it is possible to drive two cars all the way around the
  // two loops such that no car ever goes backward and the cars are always
  // within "max_error" of each other.  (For testing purposes.)
  bool BoundaryNear(S2Loop const* b, double max_error = 1e-15) const;

  // This method computes the oriented surface integral of some quantity f(x)
  // over the loop interior, given a function f_tri(A,B,C) that returns the
  // corresponding integral over the spherical triangle ABC.  Here "oriented
  // surface integral" means:
  //
  // (1) f_tri(A,B,C) must be the integral of f if ABC is counterclockwise,
  //     and the integral of -f if ABC is clockwise.
  //
  // (2) The result of this function is *either* the integral of f over the
  //     loop interior, or the integral of (-f) over the loop exterior.
  //
  // Note that there are at least two common situations where it easy to work
  // around property (2) above:
  //
  //  - If the integral of f over the entire sphere is zero, then it doesn't
  //    matter which case is returned because they are always equal.
  //
  //  - If f is non-negative, then it is easy to detect when the integral over
  //    the loop exterior has been returned, and the integral over the loop
  //    interior can be obtained by adding the integral of f over the entire
  //    unit sphere (a constant) to the result.
  //
  // Also requires that the default constructor for T must initialize the
  // value to zero.  (This is true for built-in types such as "double".)
  template <class T>
  T GetSurfaceIntegral(T f_tri(S2Point const&, S2Point const&, S2Point const&))
      const;

  ////////////////////////////////////////////////////////////////////////
  // S2Region interface (see s2region.h for details):

  // GetRectBound() is guaranteed to return exact results, while GetCapBound()
  // is conservative.
  virtual S2Loop* Clone() const;
  virtual S2Cap GetCapBound() const;
  virtual S2LatLngRect GetRectBound() const { return bound_; }

  virtual bool Contains(S2Cell const& cell) const;
  virtual bool MayIntersect(S2Cell const& cell) const;
  virtual bool VirtualContainsPoint(S2Point const& p) const {
    return Contains(p);  // The same as Contains() below, just virtual.
  }

  // The point 'p' does not need to be normalized.
  bool Contains(S2Point const& p) const;

  virtual void Encode(Encoder* const encoder) const;
  virtual bool Decode(Decoder* const decoder);
  virtual bool DecodeWithinScope(Decoder* const decoder);

 private:
  // Internal constructor used only by Clone() that makes a deep copy of
  // its argument.
  explicit S2Loop(S2Loop const* src);

  void InitOrigin();
  void InitBound();

  // Internal implementation of the Decode and DecodeWithinScope methods above.
  // If within_scope is true, memory is allocated for vertices_ and data
  // is copied from the decoder using memcpy. If it is false, vertices_
  // will point to the memory area inside the decoder, and the field
  // owns_vertices_ is set to false.
  bool DecodeInternal(Decoder* const decoder,
                      bool within_scope);

  // Internal implementation of the Intersects() method above.
  bool IntersectsInternal(S2Loop const* b) const;

  // Return an index "first" and a direction "dir" (either +1 or -1) such that
  // the vertex sequence (first, first+dir, ..., first+(n-1)*dir) does not
  // change when the loop vertex order is rotated or inverted.  This allows
  // the loop vertices to be traversed in a canonical order.  The return
  // values are chosen such that (first, ..., first+n*dir) are in the range
  // [0, 2*n-1] as expected by the vertex() method.
  int GetCanonicalFirstVertex(int* dir) const;

  // Return the index of a vertex at point "p", or -1 if not found.
  // The return value is in the range 1..num_vertices_ if found.
  int FindVertex(S2Point const& p) const;

  // This method checks all edges of this loop (A) for intersection
  // against all edges of B.  If there is any shared vertex , the
  // wedges centered at this vertex are sent to wedge_processor.
  //
  // Returns true only when the edges intersect in the sense of
  // S2EdgeUtil::RobustCrossing, returns false immediately when the
  // wedge_processor returns true: this means the wedge processor
  // knows the value of the property that the caller wants to compute,
  // and no further inspection is needed.  For instance, if the
  // property is "loops intersect", then a wedge intersection is all
  // it takes to return true.
  //
  // See Contains(), Intersects() and ContainsOrCrosses() for the
  // three uses of this function.
  bool AreBoundariesCrossing(
      S2Loop const* b, WedgeProcessor* wedge_processor) const;

  // When the loop is modified (the only cae being Invert() at this time),
  // the indexing structures need to be deleted as they become invalid.
  void ResetMutableFields();

  // We store the vertices in an array rather than a vector because we don't
  // need any STL methods, and computing the number of vertices using size()
  // would be relatively expensive (due to division by sizeof(S2Point) == 24).
  // When DecodeWithinScope is used to initialize the loop, we do not
  // take ownership of the memory for vertices_, and the owns_vertices_ field
  // is used to prevent deallocation and overwriting.
  int num_vertices_;
  S2Point* vertices_;
  bool owns_vertices_;

  S2LatLngRect bound_;
  bool origin_inside_;
  int depth_;

  // Quadtree index structure of this loop's edges.
  mutable S2LoopIndex index_;

  // Map for speeding up FindVertex: We will compute a map from vertex to
  // index in the vertex array as soon as there has been enough calls.
  mutable int num_find_vertex_calls_;
  mutable map<S2Point, int> vertex_to_index_;

  DISALLOW_EVIL_CONSTRUCTORS(S2Loop);
};

//////////////////// Implementation Details Follow ////////////////////////

// Since this method is templatized and public, the implementation needs to be
// in the .h file.

template <class T>
T S2Loop::GetSurfaceIntegral(T f_tri(S2Point const&, S2Point const&,
                                     S2Point const&)) const {
  // We sum "f_tri" over a collection T of oriented triangles, possibly
  // overlapping.  Let the sign of a triangle be +1 if it is CCW and -1
  // otherwise, and let the sign of a point "x" be the sum of the signs of the
  // triangles containing "x".  Then the collection of triangles T is chosen
  // such that either:
  //
  //  (1) Each point in the loop interior has sign +1, and sign 0 otherwise; or
  //  (2) Each point in the loop exterior has sign -1, and sign 0 otherwise.
  //
  // The triangles basically consist of a "fan" from vertex 0 to every loop
  // edge that does not include vertex 0.  These triangles will always satisfy
  // either (1) or (2).  However, what makes this a bit tricky is that
  // spherical edges become numerically unstable as their length approaches
  // 180 degrees.  Of course there is not much we can do if the loop itself
  // contains such edges, but we would like to make sure that all the triangle
  // edges under our control (i.e., the non-loop edges) are stable.  For
  // example, consider a loop around the equator consisting of four equally
  // spaced points.  This is a well-defined loop, but we cannot just split it
  // into two triangles by connecting vertex 0 to vertex 2.
  //
  // We handle this type of situation by moving the origin of the triangle fan
  // whenever we are about to create an unstable edge.  We choose a new
  // location for the origin such that all relevant edges are stable.  We also
  // create extra triangles with the appropriate orientation so that the sum
  // of the triangle signs is still correct at every point.

  // The maximum length of an edge for it to be considered numerically stable.
  // The exact value is fairly arbitrary since it depends on the stability of
  // the "f_tri" function.  The value below is quite conservative but could be
  // reduced further if desired.
  static double const kMaxLength = M_PI - 1e-5;

  // The default constructor for T must initialize the value to zero.
  // (This is true for built-in types such as "double".)
  T sum = T();
  S2Point origin = vertex(0);
  for (int i = 1; i + 1 < num_vertices(); ++i) {
    // Let V_i be vertex(i), let O be the current origin, and let length(A,B)
    // be the length of edge (A,B).  At the start of each loop iteration, the
    // "leading edge" of the triangle fan is (O,V_i), and we want to extend
    // the triangle fan so that the leading edge is (O,V_i+1).
    //
    // Invariants:
    //  1. length(O,V_i) < kMaxLength for all (i > 1).
    //  2. Either O == V_0, or O is approximately perpendicular to V_0.
    //  3. "sum" is the oriented integral of f over the area defined by
    //     (O, V_0, V_1, ..., V_i).
    DCHECK(i == 1 || origin.Angle(vertex(i)) < kMaxLength);
    DCHECK(origin == vertex(0) || fabs(origin.DotProd(vertex(0))) < 1e-15);

    if (vertex(i+1).Angle(origin) > kMaxLength) {
      // We are about to create an unstable edge, so choose a new origin O'
      // for the triangle fan.
      S2Point old_origin = origin;
      if (origin == vertex(0)) {
        // The following point is well-separated from V_i and V_0 (and
        // therefore V_i+1 as well).
        origin = S2::RobustCrossProd(vertex(0), vertex(i)).Normalize();
      } else if (vertex(i).Angle(vertex(0)) < kMaxLength) {
        // All edges of the triangle (O, V_0, V_i) are stable, so we can
        // revert to using V_0 as the origin.
        origin = vertex(0);
      } else {
        // (O, V_i+1) and (V_0, V_i) are antipodal pairs, and O and V_0 are
        // perpendicular.  Therefore V_0.CrossProd(O) is approximately
        // perpendicular to all of {O, V_0, V_i, V_i+1}, and we can choose
        // this point O' as the new origin.
        origin = vertex(0).CrossProd(old_origin);

        // Advance the edge (V_0,O) to (V_0,O').
        sum += f_tri(vertex(0), old_origin, origin);
      }
      // Advance the edge (O,V_i) to (O',V_i).
      sum += f_tri(old_origin, vertex(i), origin);
    }
    // Advance the edge (O,V_i) to (O,V_i+1).
    sum += f_tri(origin, vertex(i), vertex(i+1));
  }
  // If the origin is not V_0, we need to sum one more triangle.
  if (origin != vertex(0)) {
    // Advance the edge (O,V_n-1) to (O,V_0).
    sum += f_tri(origin, vertex(num_vertices() - 1), vertex(0));
  }
  return sum;
}

#endif  // UTIL_GEOMETRY_S2LOOP_H__
