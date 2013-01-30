// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S2R2RECT_H_
#define UTIL_GEOMETRY_S2R2RECT_H_

#include "base/basictypes.h"
#include "base/logging.h"
#include "r1interval.h"
#include "s2region.h"
#include "util/math/vector2-inl.h"

class S2CellId;
class S2Cell;

// TODO: Create an r2.h and move this definition into it.
typedef Vector2_d R2Point;

// This class is a stopgap measure that allows some of the S2 spherical
// geometry machinery to be applied to planar geometry.  An S2R2Rect
// represents a closed axis-aligned rectangle in the (x,y) plane, but it also
// happens to be a subtype of S2Region, which means that you can use an
// S2RegionCoverer to approximate it as a collection of S2CellIds.
//
// With respect to the S2Cell decomposition, an S2R2Rect is interpreted as a
// region of (s,t)-space on face 0.  In particular, the rectangle [0,1]x[0,1]
// corresponds to the S2CellId that covers all of face 0.  This means that
// only rectangles that are subsets of [0,1]x[0,1] can be approximated using
// the S2RegionCoverer interface.
//
// The S2R2Rect class is also a convenient way to find the (s,t)-region
// covered by a given S2CellId (see the FromCell and FromCellId methods).
//
// TODO: If the geometry library is extended to have better support for planar
// geometry, then this class should no longer be necessary.
//
// This class is intended to be copied by value as desired.  It uses
// the default copy constructor and assignment operator, however it is
// not a "plain old datatype" (POD) because it has virtual functions.
class S2R2Rect : public S2Region {
 public:
  // Construct a rectangle from the given lower-left and upper-right points.
  inline S2R2Rect(R2Point const& lo, R2Point const& hi);

  // Construct a rectangle from the given intervals in x and y.  The two
  // intervals must either be both empty or both non-empty.
  inline S2R2Rect(R1Interval const& x, R1Interval const& y);

  // Construct a rectangle that corresponds to the boundary of the given cell
  // is (s,t)-space.  Such rectangles are always a subset of [0,1]x[0,1].
  static S2R2Rect FromCell(S2Cell const& cell);
  static S2R2Rect FromCellId(S2CellId const& id);

  // Construct a rectangle from a center point and size in each dimension.
  // Both components of size should be non-negative, i.e. this method cannot
  // be used to create an empty rectangle.
  static S2R2Rect FromCenterSize(R2Point const& center,
                                 R2Point const& size);

  // Convenience method to construct a rectangle containing a single point.
  static S2R2Rect FromPoint(R2Point const& p);

  // Convenience method to construct the minimal bounding rectangle containing
  // the two given points.  This is equivalent to starting with an empty
  // rectangle and calling AddPoint() twice.  Note that it is different than
  // the S2R2Rect(lo, hi) constructor, where the first point is always
  // used as the lower-left corner of the resulting rectangle.
  static S2R2Rect FromPointPair(R2Point const& p1, R2Point const& p2);

  // Accessor methods.
  R1Interval const& x() const { return x_; }
  R1Interval const& y() const { return y_; }
  R1Interval *mutable_x() { return &x_; }
  R1Interval *mutable_y() { return &y_; }
  R2Point lo() const { return R2Point(x_.lo(), y_.lo()); }
  R2Point hi() const { return R2Point(x_.hi(), y_.hi()); }

  // The canonical empty rectangle.  Use is_empty() to test for empty
  // rectangles, since they have more than one representation.
  static inline S2R2Rect Empty();

  // Return true if the rectangle is valid, which essentially just means
  // that if the bound for either axis is empty then both must be.
  inline bool is_valid() const;

  // Return true if the rectangle is empty, i.e. it contains no points at all.
  inline bool is_empty() const;

  // Return the k-th vertex of the rectangle (k = 0,1,2,3) in CCW order.
  // Vertex 0 is in the lower-left corner.
  R2Point GetVertex(int k) const;

  // Return the center of the rectangle in (x,y)-space
  // (in general this is not the center of the region on the sphere).
  R2Point GetCenter() const;

  // Return the width and height of this rectangle in (x,y)-space.  Empty
  // rectangles have a negative width and height.
  R2Point GetSize() const;

  // Return true if the rectangle contains the given point.  Note that
  // rectangles are closed regions, i.e. they contain their boundary.
  bool Contains(R2Point const& p) const;

  // Return true if and only if the given point is contained in the interior
  // of the region (i.e. the region excluding its boundary).
  bool InteriorContains(R2Point const& p) const;

  // Return true if and only if the rectangle contains the given other
  // rectangle.
  bool Contains(S2R2Rect const& other) const;

  // Return true if and only if the interior of this rectangle contains all
  // points of the given other rectangle (including its boundary).
  bool InteriorContains(S2R2Rect const& other) const;

  // Return true if this rectangle and the given other rectangle have any
  // points in common.
  bool Intersects(S2R2Rect const& other) const;

  // Return true if and only if the interior of this rectangle intersects
  // any point (including the boundary) of the given other rectangle.
  bool InteriorIntersects(S2R2Rect const& other) const;

  // Increase the size of the bounding rectangle to include the given point.
  // The rectangle is expanded by the minimum amount possible.
  void AddPoint(R2Point const& p);

  // Return a rectangle that contains all points whose x-distance from this
  // rectangle is at most margin.x(), and whose y-distance from this rectangle
  // is at most margin.y().  Note that any expansion of an empty interval
  // remains empty, and both components of the given margin must be
  // non-negative.
  S2R2Rect Expanded(R2Point const& margin) const;

  // Return the smallest rectangle containing the union of this rectangle and
  // the given rectangle.
  S2R2Rect Union(S2R2Rect const& other) const;

  // Return the smallest rectangle containing the intersection of this
  // rectangle and the given rectangle.
  S2R2Rect Intersection(S2R2Rect const& other) const;

  // Return true if two rectangles contains the same set of points.
  inline bool operator==(S2R2Rect const& other) const;

  // Return true if the x- and y-intervals of the two rectangles are the same
  // up to the given tolerance (see r1interval.h for details).
  bool ApproxEquals(S2R2Rect const& other, double max_error = 1e-15) const;

  // Return the unit-length S2Point corresponding to the given point "p" in
  // the (s,t)-plane.  "p" need not be restricted to the range [0,1]x[0,1].
  static S2Point ToS2Point(R2Point const& p);

  ////////////////////////////////////////////////////////////////////////
  // S2Region interface (see s2region.h for details):

  virtual S2R2Rect* Clone() const;
  virtual S2Cap GetCapBound() const;
  virtual S2LatLngRect GetRectBound() const;
  virtual bool VirtualContainsPoint(S2Point const& p) const {
    return Contains(p);  // The same as Contains() below, just virtual.
  }
  bool Contains(S2Point const& p) const;
  virtual bool Contains(S2Cell const& cell) const;
  virtual bool MayIntersect(S2Cell const& cell) const;
  virtual void Encode(Encoder* const encoder) const {
    S2LOG(FATAL) << "Unimplemented";
  }
  virtual bool Decode(Decoder* const decoder) { return false; }

 private:
  R1Interval x_;
  R1Interval y_;
};

inline S2R2Rect::S2R2Rect(R2Point const& lo, R2Point const& hi)
  : x_(lo.x(), hi.x()), y_(lo.y(), hi.y()) {
  DCHECK(is_valid());
}

inline S2R2Rect::S2R2Rect(R1Interval const& x, R1Interval const& y)
  : x_(x), y_(y) {
  DCHECK(is_valid());
}

inline S2R2Rect S2R2Rect::Empty() {
  return S2R2Rect(R1Interval::Empty(), R1Interval::Empty());
}

inline bool S2R2Rect::is_valid() const {
  // The x/y ranges must either be both empty or both non-empty.
  return x_.is_empty() == y_.is_empty();
}

inline bool S2R2Rect::is_empty() const {
  return x_.is_empty();
}

inline bool S2R2Rect::operator==(S2R2Rect const& other) const {
  return x_ == other.x_ && y_ == other.y_;
}

ostream& operator<<(ostream& os, S2R2Rect const& r);

#endif  // UTIL_GEOMETRY_S2R2RECT_H_
