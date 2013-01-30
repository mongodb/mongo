// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_R1INTERVAL_H_
#define UTIL_GEOMETRY_R1INTERVAL_H_

#include <math.h>

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <iostream>
using std::ostream;
using std::cout;
using std::endl;

#include "base/basictypes.h"
#include "base/logging.h"
#include "util/math/vector2-inl.h"

// An R1Interval represents a closed, bounded interval on the real line.
// It is capable of representing the empty interval (containing no points)
// and zero-length intervals (containing a single point).
//
// This class is intended to be copied by value as desired.  It uses
// the default copy constructor and assignment operator.
class R1Interval {
 public:
  // Constructor.  If lo > hi, the interval is empty.
  R1Interval(double lo, double hi) : bounds_(lo, hi) {}

  // The default constructor creates an empty interval.  (Any interval where
  // lo > hi is considered to be empty.)
  //
  // Note: Don't construct an interval using the default constructor and
  // set_lo()/set_hi(), since this technique doesn't work with S1Interval and
  // is bad programming style anyways.  If you need to set both endpoints, use
  // the constructor above:
  //
  //   lat_bounds_ = R1Interval(lat_lo, lat_hi);
  R1Interval() : bounds_(1, 0) {}

  // Returns an empty interval.
  static inline R1Interval Empty() { return R1Interval(); }

  // Convenience method to construct an interval containing a single point.
  static R1Interval FromPoint(double p) {
    return R1Interval(p, p);
  }

  // Convenience method to construct the minimal interval containing
  // the two given points.  This is equivalent to starting with an empty
  // interval and calling AddPoint() twice, but it is more efficient.
  static R1Interval FromPointPair(double p1, double p2) {
    if (p1 <= p2) {
      return R1Interval(p1, p2);
    } else {
      return R1Interval(p2, p1);
    }
  }

  double lo() const { return bounds_[0]; }
  double hi() const { return bounds_[1]; }
  double bound(int i) const { return bounds_[i]; }
  Vector2_d const& bounds() const { return bounds_; }

  // Methods to modify one endpoint of an existing R1Interval.  Do not use
  // these methods if you want to replace both endpoints of the interval; use
  // a constructor instead.  For example:
  //
  //   *lat_bounds = R1Interval(lat_lo, lat_hi);
  void set_lo(double p) { bounds_[0] = p; }
  void set_hi(double p) { bounds_[1] = p; }

  // Return true if the interval is empty, i.e. it contains no points.
  bool is_empty() const { return lo() > hi(); }

  // Return the center of the interval.  For empty intervals,
  // the result is arbitrary.
  double GetCenter() const { return 0.5 * (lo() + hi()); }

  // Return the length of the interval.  The length of an empty interval
  // is negative.
  double GetLength() const { return hi() - lo(); }

  bool Contains(double p) const {
    return p >= lo() && p <= hi();
  }

  bool InteriorContains(double p) const {
    return p > lo() && p < hi();
  }

  // Return true if this interval contains the interval 'y'.
  bool Contains(R1Interval const& y) const {
    if (y.is_empty()) return true;
    return y.lo() >= lo() && y.hi() <= hi();
  }

  // Return true if the interior of this interval contains the entire
  // interval 'y' (including its boundary).
  bool InteriorContains(R1Interval const& y) const {
    if (y.is_empty()) return true;
    return y.lo() > lo() && y.hi() < hi();
  }

  // Return true if this interval intersects the given interval,
  // i.e. if they have any points in common.
  bool Intersects(R1Interval const& y) const {
    if (lo() <= y.lo()) {
      return y.lo() <= hi() && y.lo() <= y.hi();
    } else {
      return lo() <= y.hi() && lo() <= hi();
    }
  }

  // Return true if the interior of this interval intersects
  // any point of the given interval (including its boundary).
  bool InteriorIntersects(R1Interval const& y) const {
    return y.lo() < hi() && lo() < y.hi() && lo() < hi() && y.lo() <= y.hi();
  }

  // Return the Hausdorff distance to the given interval 'y'. For two
  // R1Intervals x and y, this distance is defined as
  //     h(x, y) = max_{p in x} min_{q in y} d(p, q).
  double GetDirectedHausdorffDistance(R1Interval const& y) const {
    if (is_empty()) return 0.0;
    if (y.is_empty()) return HUGE_VAL;
    return max(0.0, max(hi() - y.hi(), y.lo() - lo()));
  }

  // Expand the interval so that it contains the given point "p".
  void AddPoint(double p) {
    if (is_empty()) { set_lo(p); set_hi(p); }
    else if (p < lo()) { set_lo(p); }
    else if (p > hi()) { set_hi(p); }
  }

  // Return an interval that contains all points with a distance "radius" of
  // a point in this interval.  Note that the expansion of an empty interval
  // is always empty.
  R1Interval Expanded(double radius) const {
    DCHECK_GE(radius, 0);
    if (is_empty()) return *this;
    return R1Interval(lo() - radius, hi() + radius);
  }

  // Return the smallest interval that contains this interval and the
  // given interval "y".
  R1Interval Union(R1Interval const& y) const {
    if (is_empty()) return y;
    if (y.is_empty()) return *this;
    return R1Interval(min(lo(), y.lo()), max(hi(), y.hi()));
  }

  // Return the intersection of this interval with the given interval.
  // Empty intervals do not need to be special-cased.
  R1Interval Intersection(R1Interval const& y) const {
    return R1Interval(max(lo(), y.lo()), min(hi(), y.hi()));
  }

  // Return true if two intervals contain the same set of points.
  bool operator==(R1Interval const& y) const {
    return (lo() == y.lo() && hi() == y.hi()) || (is_empty() && y.is_empty());
  }

  // Return true if length of the symmetric difference between the two
  // intervals is at most the given tolerance.
  bool ApproxEquals(R1Interval const& y, double max_error = 1e-15) const {
    if (is_empty()) return y.GetLength() <= max_error;
    if (y.is_empty()) return GetLength() <= max_error;
    return fabs(y.lo() - lo()) + fabs(y.hi() - hi()) <= max_error;
  }

 private:
  Vector2_d bounds_;
};
DECLARE_POD(R1Interval);

inline ostream& operator<<(ostream& os, R1Interval const& x) {
  return os << "[" << x.lo() << ", " << x.hi() << "]";
}

#endif  // UTIL_GEOMETRY_R1INTERVAL_H_
