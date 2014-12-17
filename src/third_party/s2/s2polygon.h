// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S2POLYGON_H_
#define UTIL_GEOMETRY_S2POLYGON_H_

#include <map>
using std::map;
using std::multimap;

#include <vector>
using std::vector;


#include "base/definer.h"
#include "base/basictypes.h"
#include "base/macros.h"
#include "s2.h"
#include "s2region.h"
#include "s2loop.h"
#include "s2polyline.h"

class S2CellUnion;

// An S2Polygon is an S2Region object that represents a polygon.  A polygon
// consists of zero or more loops representing "shells" and "holes".  All
// loops should be oriented CCW, i.e. the shell or hole is on the left side of
// the loop.  Loops may be specified in any order.  A point is defined to be
// inside the polygon if it is contained by an odd number of loops.
//
// Polygons have the following restrictions:
//
//  - Loops may not cross, i.e. the boundary of a loop may not intersect
//    both the interior and exterior of any other loop.
//
//  - Loops may not share edges, i.e. if a loop contains an edge AB, then
//    no other loop may contain AB or BA.
//
//  - No loop may cover more than half the area of the sphere.  This ensures
//    that no loop properly contains the complement of any other loop, even
//    if the loops are from different polygons.  (Loops that represent exact
//    hemispheres are allowed.)
//
// Loops may share vertices, however no vertex may appear twice in a single
// loop (see s2loop.h).
class S2Polygon : public S2Region {
 public:
  // Creates an empty polygon that should be initialized by calling Init() or
  // Decode().
  S2Polygon();

  // Convenience constructor that calls Init() with the given loops.  Takes
  // ownership of the loops and clears the given vector.
  explicit S2Polygon(vector<S2Loop*>* loops);

  // Convenience constructor that creates a polygon with a single loop
  // corresponding to the given cell.
  explicit S2Polygon(S2Cell const& cell);

  // Initialize a polygon by taking ownership of the given loops and clearing
  // the given vector.  This method figures out the loop nesting hierarchy and
  // then reorders the loops by following a preorder traversal.  This implies
  // that each loop is immediately followed by its descendants in the nesting
  // hierarchy.  (See also GetParent and GetLastDescendant.)
  void Init(vector<S2Loop*>* loops);

  // Release ownership of the loops of this polygon, and appends them to
  // "loops" if non-NULL.  Resets the polygon to be empty.
  void Release(vector<S2Loop*>* loops);

  // Makes a deep copy of the given source polygon.  Requires that the
  // destination polygon is empty.
  void Copy(S2Polygon const* src);

  // Destroys the polygon and frees its loops.
  ~S2Polygon();

  // Return true if the given loops form a valid polygon.  Assumes that
  // all of the given loops have already been validated.
  // Output errors to "err" if it's not NULL.
  static bool IsValid(const vector<S2Loop*>& loops, string* err = NULL);

  // Return true if this is a valid polygon.  Note that in debug mode,
  // validity is checked at polygon creation time, so IsValid() should always
  // return true.
  // If err is not NULL, output errors to it.
  bool IsValid(string* err = NULL) const;

  // DEPRECATED.
  bool IsValid(bool check_loops, int max_adjacent) const;

  int num_loops() const { return loops_.size(); }

  // Total number of vertices in all loops.
  int num_vertices() const { return num_vertices_; }

  S2Loop* loop(int k) const { return loops_[k]; }

  // Return the index of the parent of loop k, or -1 if it has no parent.
  int GetParent(int k) const;

  // Return the index of the last loop that is contained within loop k.
  // Returns num_loops() - 1 if k < 0.  Note that loops are indexed according
  // to a preorder traversal of the nesting hierarchy, so the immediate
  // children of loop k can be found by iterating over loops
  // (k+1)..GetLastDescendant(k) and selecting those whose depth is equal to
  // (loop(k)->depth() + 1).
  int GetLastDescendant(int k) const;

  // Return the area of the polygon interior, i.e. the region on the left side
  // of an odd number of loops.  The return value is between 0 and 4*Pi.
  double GetArea() const;

  // Return the true centroid of the polygon multiplied by the area of the
  // polygon (see s2.h for details on centroids).  The result is not unit
  // length, so you may want to normalize it.  Also note that in general, the
  // centroid may not be contained by the polygon.
  //
  // We prescale by the polygon area for two reasons: (1) it is cheaper to
  // compute this way, and (2) it makes it easier to compute the centroid of
  // more complicated shapes (by splitting them into disjoint regions and
  // adding their centroids).
  S2Point GetCentroid() const;

  // Return true if this polygon contains the given other polygon, i.e.
  // if polygon A contains all points contained by polygon B.
  bool Contains(S2Polygon const* b) const;

  // Returns true if this polgyon (A) approximately contains the given other
  // polygon (B). This is true if it is possible to move the vertices of B
  // no further than "vertex_merge_radius" such that A contains the modified B.
  //
  // For example, the empty polygon will contain any polygon whose maximum
  // width is no more than vertex_merge_radius.
  bool ApproxContains(S2Polygon const* b, S1Angle vertex_merge_radius) const;

  // Return true if this polygon intersects the given other polygon, i.e.
  // if there is a point that is contained by both polygons.
  bool Intersects(S2Polygon const* b) const;

  // Initialize this polygon to the intersection, union, or difference
  // (A - B) of the given two polygons.  The "vertex_merge_radius" determines
  // how close two vertices must be to be merged together and how close a
  // vertex must be to an edge in order to be spliced into it (see
  // S2PolygonBuilder for details).  By default, the merge radius is just
  // large enough to compensate for errors that occur when computing
  // intersection points between edges (S2EdgeUtil::kIntersectionTolerance).
  //
  // If you are going to convert the resulting polygon to a lower-precision
  // format, it is necessary to increase the merge radius in order to get a
  // valid result after rounding (i.e. no duplicate vertices, etc).  For
  // example, if you are going to convert them to geostore::PolygonProto
  // format, then S1Angle::E7(1) is a good value for "vertex_merge_radius".
  void InitToIntersection(S2Polygon const* a, S2Polygon const* b);
  void InitToIntersectionSloppy(S2Polygon const* a, S2Polygon const* b,
                                S1Angle vertex_merge_radius);
  void InitToUnion(S2Polygon const* a, S2Polygon const* b);
  void InitToUnionSloppy(S2Polygon const* a, S2Polygon const* b,
                         S1Angle vertex_merge_radius);
  void InitToDifference(S2Polygon const* a, S2Polygon const* b);
  void InitToDifferenceSloppy(S2Polygon const* a, S2Polygon const* b,
                              S1Angle vertex_merge_radius);

  // Initializes this polygon to a polygon that contains fewer vertices and is
  // within tolerance of the polygon a, with some caveats.
  //
  // - If there is a very small island in the original polygon, it may
  //   disappear completely.  Thus some parts of the original polygon
  //   may not be close to the simplified polygon.  Those parts are small,
  //   though, and arguably don't need to be kept.
  // - However, if there are dense islands, they may all disappear, instead
  //   of replacing them by a big simplified island.
  // - For small tolerances (compared to the polygon size), it may happen that
  //   the simplified polygon has more vertices than the original, if the
  //   first step of the simplification creates too many self-intersections.
  //   One can construct irrealistic cases where that happens to an extreme
  //   degree.
  void InitToSimplified(S2Polygon const* a, S1Angle tolerance);

  // Intersect this polygon with the polyline "in" and append the resulting
  // zero or more polylines to "out" (which must be empty).  The polylines
  // are appended in the order they would be encountered by traversing "in"
  // from beginning to end.  Note that the output may include polylines with
  // only one vertex, but there will not be any zero-vertex polylines.
  //
  // This is equivalent to calling IntersectWithPolylineSloppy() with the
  // "vertex_merge_radius" set to S2EdgeUtil::kIntersectionTolerance.
  void IntersectWithPolyline(S2Polyline const* in,
                             vector<S2Polyline*> *out) const;

  // Similar to IntersectWithPolyline(), except that vertices will be
  // dropped as necessary to ensure that all adjacent vertices in the
  // sequence obtained by concatenating the output polylines will be
  // farther than "vertex_merge_radius" apart.  Note that this can change
  // the number of output polylines and/or yield single-vertex polylines.
  void IntersectWithPolylineSloppy(S2Polyline const* in,
                                   vector<S2Polyline*> *out,
                                   S1Angle vertex_merge_radius) const;

  // Same as IntersectWithPolyline, but subtracts this polygon from
  // the given polyline.
  void SubtractFromPolyline(S2Polyline const* in,
                            vector<S2Polyline*> *out) const;

  // Same as IntersectWithPolylineSloppy, but subtracts this polygon
  // from the given polyline.
  void SubtractFromPolylineSloppy(S2Polyline const* in,
                                  vector<S2Polyline*> *out,
                                  S1Angle vertex_merge_radius) const;

  // Return a polygon which is the union of the given polygons.
  // Clears the vector and deletes the polygons!
  static S2Polygon* DestructiveUnion(vector<S2Polygon*>* polygons);
  static S2Polygon* DestructiveUnionSloppy(vector<S2Polygon*>* polygons,
                                           S1Angle vertex_merge_radius);

  // Initialize this polygon to the outline of the given cell union.
  // In principle this polygon should exactly contain the cell union and
  // this polygon's inverse should not intersect the cell union, but rounding
  // issues may cause this not to be the case.
  // Does not work correctly if the union covers more than half the sphere.
  void InitToCellUnionBorder(S2CellUnion const& cells);

  // Return true if every loop of this polygon shares at most one vertex with
  // its parent loop.  Every polygon has a unique normalized form.  Normalized
  // polygons are useful for testing since it is easy to compare whether two
  // polygons represent the same region.
  // Output errors into "err" if it's not NULL.
  bool IsNormalized(string* err = NULL) const;

  // Return true if two polygons have the same boundary.  More precisely, this
  // method requires that both polygons have loops with the same cyclic vertex
  // order and the same nesting hierarchy.
  bool BoundaryEquals(S2Polygon const* b) const;

  // Return true if two polygons have the same boundary except for vertex
  // perturbations.  Both polygons must have loops with the same cyclic vertex
  // order and the same nesting hierarchy, but the vertex locations are
  // allowed to differ by up to "max_error".
  bool BoundaryApproxEquals(S2Polygon const* b, double max_error = 1e-15) const;

  // Return true if two polygons have boundaries that are within "max_error"
  // of each other along their entire lengths.  More precisely, there must be
  // a bijection between the two sets of loops such that for each pair of
  // loops, "a_loop->BoundaryNear(b_loop)" is true.
  bool BoundaryNear(S2Polygon const* b, double max_error = 1e-15) const;

  // If the point is not contained by the polygon returns a point on the
  // polygon closest to the given point. Otherwise returns the point itself.
  // The polygon must not be empty.
  S2Point Project(S2Point const& point) const;

  ////////////////////////////////////////////////////////////////////////
  // S2Region interface (see s2region.h for details):

  // GetRectBound() guarantees that it will return exact bounds. GetCapBound()
  // does not.
  virtual S2Polygon* Clone() const;
  virtual S2Cap GetCapBound() const;  // Cap surrounding rect bound.
  virtual S2LatLngRect GetRectBound() const { return bound_; }

  virtual bool Contains(S2Cell const& cell) const;
  virtual bool MayIntersect(S2Cell const& cell) const;
  virtual bool VirtualContainsPoint(S2Point const& p) const;

  // The point 'p' does not need to be normalized.
  bool Contains(S2Point const& p) const;

  virtual void Encode(Encoder* const encoder) const;
  virtual bool Decode(Decoder* const decoder);
  virtual bool DecodeWithinScope(Decoder* const decoder);

 private:
  // Internal constructor that does *not* take ownership of its argument.
  explicit S2Polygon(S2Loop* loop);

  // A map from each loop to its immediate children with respect to nesting.
  // This map is built during initialization of multi-loop polygons to
  // determine which are shells and which are holes, and then discarded.
  typedef map<S2Loop*, vector<S2Loop*> > LoopMap;

  // Internal implementation of the Decode and DecodeWithinScope methods above.
  // The within_scope parameter specifies whether to call DecodeWithinScope
  // on the loops.
  bool DecodeInternal(Decoder* const decoder, bool within_scope);

  // Internal implementation of intersect/subtract polyline functions above.
  void InternalClipPolyline(bool invert,
                            S2Polyline const* a,
                            vector<S2Polyline*> *out,
                            S1Angle vertex_merge_radius) const;

  static void InsertLoop(S2Loop* new_loop, S2Loop* parent, LoopMap* loop_map);
  static bool ContainsChild(S2Loop* a, S2Loop* b, LoopMap const& loop_map);
  void InitLoop(S2Loop* loop, int depth, LoopMap* loop_map);

  int ContainsOrCrosses(S2Loop const* b) const;
  bool AnyLoopContains(S2Loop const* b) const;
  bool ContainsAllShells(S2Polygon const* b) const;
  bool ExcludesAllHoles(S2Polygon const* b) const;
  bool IntersectsAnyShell(S2Polygon const* b) const;
  bool IntersectsShell(S2Loop const* b) const;

  vector<S2Loop*> loops_;
  S2LatLngRect bound_;
  char owns_loops_;
  char has_holes_;

  // Cache for num_vertices().
  int num_vertices_;

  DISALLOW_EVIL_CONSTRUCTORS(S2Polygon);
};

#endif  // UTIL_GEOMETRY_S2POLYGON_H_
