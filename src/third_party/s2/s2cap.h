// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S2CAP_H_
#define UTIL_GEOMETRY_S2CAP_H_

#include "base/basictypes.h"
#include "base/logging.h"
#include "s1angle.h"
#include "s2region.h"

// This class represents a spherical cap, i.e. a portion of a sphere cut off
// by a plane.  The cap is defined by its axis and height.  This
// representation has good numerical accuracy for very small caps (unlike the
// (axis, min-distance-from-origin) representation), and is also efficient for
// containment tests (unlike the (axis, angle) representation).
//
// Here are some useful relationships between the cap height (h), the cap
// opening angle (theta), the maximum chord length from the cap's center (d),
// and the radius of cap's base (a).  All formulas assume a unit radius.
//
//     h = 1 - cos(theta)
//       = 2 sin^2(theta/2)
//   d^2 = 2 h
//       = a^2 + h^2
//
// Caps may be constructed from either an axis and a height, or an axis and
// an angle.  To avoid ambiguity, there are no public constructors except
// the default constructor.
//
// This class is intended to be copied by value as desired.  It uses
// the default copy constructor and assignment operator, however it is
// not a "plain old datatype" (POD) because it has virtual functions.
class S2Cap : public S2Region {
 public:
  // The default constructor returns an empty S2Cap.
  S2Cap() : axis_(1, 0, 0), height_(-1) {}

  // Create a cap given its axis and the cap height, i.e. the maximum
  // projected distance along the cap axis from the cap center.
  // 'axis' should be a unit-length vector.
  inline static S2Cap FromAxisHeight(S2Point const& axis, double height);

  // Create a cap given its axis and the cap opening angle, i.e. maximum
  // angle between the axis and a point on the cap.  'axis' should be a
  // unit-length vector, and 'angle' should be non-negative.  If 'angle' is
  // 180 degrees or larger, the cap will contain the entire unit sphere.
  static S2Cap FromAxisAngle(S2Point const& axis, S1Angle const& angle);

  // Create a cap given its axis and its area in steradians.  'axis' should be
  // a unit-length vector, and 'area' should be between 0 and 4 * M_PI.
  inline static S2Cap FromAxisArea(S2Point const& axis, double area);

  // Return an empty cap, i.e. a cap that contains no points.
  static S2Cap Empty() { return S2Cap(); }

  // Return a full cap, i.e. a cap that contains all points.
  static S2Cap Full() { return S2Cap(S2Point(1, 0, 0), 2); }

  ~S2Cap() {}

  // Accessor methods.
  S2Point const& axis() const { return axis_; }
  double height() const { return height_; }
  double area() const { return 2 * M_PI * max(0.0, height_); }

  // Return the cap opening angle in radians, or a negative number for
  // empty caps.
  S1Angle angle() const;

  // We allow negative heights (to represent empty caps) but not heights
  // greater than 2.
  bool is_valid() const { return S2::IsUnitLength(axis_) && height_ <= 2; }

  // Return true if the cap is empty, i.e. it contains no points.
  bool is_empty() const { return height_ < 0; }

  // Return true if the cap is full, i.e. it contains all points.
  bool is_full() const { return height_ >= 2; }

  // Return the complement of the interior of the cap.  A cap and its
  // complement have the same boundary but do not share any interior points.
  // The complement operator is not a bijection, since the complement of a
  // singleton cap (containing a single point) is the same as the complement
  // of an empty cap.
  S2Cap Complement() const;

  // Return true if and only if this cap contains the given other cap
  // (in a set containment sense, e.g. every cap contains the empty cap).
  bool Contains(S2Cap const& other) const;

  // Return true if and only if this cap intersects the given other cap,
  // i.e. whether they have any points in common.
  bool Intersects(S2Cap const& other) const;

  // Return true if and only if the interior of this cap intersects the
  // given other cap.  (This relationship is not symmetric, since only
  // the interior of this cap is used.)
  bool InteriorIntersects(S2Cap const& other) const;

  // Return true if and only if the given point is contained in the interior
  // of the region (i.e. the region excluding its boundary).  'p' should be
  // be a unit-length vector.
  bool InteriorContains(S2Point const& p) const;

  // Increase the cap height if necessary to include the given point.
  // If the cap is empty the axis is set to the given point, but otherwise
  // it is left unchanged.  'p' should be a unit-length vector.
  void AddPoint(S2Point const& p);

  // Increase the cap height if necessary to include "other".  If the current
  // cap is empty it is set to the given other cap.
  void AddCap(S2Cap const& other);

  // Return a cap that contains all points within a given distance of this
  // cap.  Note that any expansion of the empty cap is still empty.
  S2Cap Expanded(S1Angle const& distance) const;

  ////////////////////////////////////////////////////////////////////////
  // S2Region interface (see s2region.h for details):

  virtual S2Cap* Clone() const;
  virtual S2Cap GetCapBound() const;
  virtual S2LatLngRect GetRectBound() const;
  virtual bool Contains(S2Cell const& cell) const;
  virtual bool MayIntersect(S2Cell const& cell) const;
  virtual bool VirtualContainsPoint(S2Point const& p) const {
    return Contains(p);  // The same as Contains() below, just virtual.
  }

  // The point 'p' should be a unit-length vector.
  bool Contains(S2Point const& p) const;

  virtual void Encode(Encoder* const encoder) const {
    S2LOG(FATAL) << "Unimplemented";
  }
  virtual bool Decode(Decoder* const decoder) { return false; }

  ///////////////////////////////////////////////////////////////////////
  // The following static methods are convenience functions for assertions
  // and testing purposes only.

  // Return true if two caps are identical.
  bool operator==(S2Cap const& other) const;

  // Return true if the cap axis and height differ by at most "max_error"
  // from the given cap "other".
  bool ApproxEquals(S2Cap const& other, double max_error = 1e-14);

 private:
  S2Cap(S2Point const& axis, double height)
    : axis_(axis), height_(height) {
    DCHECK(is_valid());
  }

  // Return true if the cap intersects 'cell', given that the cap
  // vertices have alrady been checked.
  bool Intersects(S2Cell const& cell, S2Point const* vertices) const;

  S2Point axis_;
  double height_;
};

inline S2Cap S2Cap::FromAxisHeight(S2Point const& axis, double height) {
  DCHECK(S2::IsUnitLength(axis));
  return S2Cap(axis, height);
}

inline S2Cap S2Cap::FromAxisArea(S2Point const& axis, double area) {
  DCHECK(S2::IsUnitLength(axis));
  return S2Cap(axis, area / (2 * M_PI));
}

ostream& operator<<(ostream& os, S2Cap const& cap);

#endif  // UTIL_GEOMETRY_S2CAP_H_
