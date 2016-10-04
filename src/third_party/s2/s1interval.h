// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S1INTERVAL_H_
#define UTIL_GEOMETRY_S1INTERVAL_H_

#include <iostream>
using std::ostream;
using std::cout;
using std::endl;
#include "base/definer.h"

#ifdef OS_WINDOWS
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#include "base/basictypes.h"
#include "base/logging.h"
#include "util/math/vector2-inl.h"

// An S1Interval represents a closed interval on a unit circle (also known
// as a 1-dimensional sphere).  It is capable of representing the empty
// interval (containing no points), the full interval (containing all
// points), and zero-length intervals (containing a single point).
//
// Points are represented by the angle they make with the positive x-axis in
// the range [-Pi, Pi].  An interval is represented by its lower and upper
// bounds (both inclusive, since the interval is closed).  The lower bound may
// be greater than the upper bound, in which case the interval is "inverted"
// (i.e. it passes through the point (-1, 0)).
//
// Note that the point (-1, 0) has two valid representations, Pi and -Pi.
// The normalized representation of this point internally is Pi, so that
// endpoints of normal intervals are in the range (-Pi, Pi].  However, we
// take advantage of the point -Pi to construct two special intervals:
// the Full() interval is [-Pi, Pi], and the Empty() interval is [Pi, -Pi].
//
// This class is intended to be copied by value as desired.  It uses
// the default copy constructor and assignment operator.
class S1Interval {
 public:
  // Constructor.  Both endpoints must be in the range -Pi to Pi inclusive.
  // The value -Pi is converted internally to Pi except for the Full()
  // and Empty() intervals.
  inline S1Interval(double lo, double hi);

  // The default constructor creates an empty interval.
  //
  // Note: Don't construct an interval using the default constructor and
  // set_lo()/set_hi().  If you need to set both endpoints, use the
  // constructor above:
  //
  //   lng_bounds_ = S1Interval(lng_lo, lng_hi);
  inline S1Interval();

  // Returns the empty interval.
  static inline S1Interval Empty();

  // Returns the full interval.
  static inline S1Interval Full();

  // Convenience method to construct an interval containing a single point.
  static S1Interval FromPoint(double p);

  // Convenience method to construct the minimal interval containing
  // the two given points.  This is equivalent to starting with an empty
  // interval and calling AddPoint() twice, but it is more efficient.
  static S1Interval FromPointPair(double p1, double p2);

  double lo() const { return bounds_[0]; }
  double hi() const { return bounds_[1]; }
  double bound(int i) const { return bounds_[i]; }
  Vector2_d const& bounds() const { return bounds_; }

  // Methods to modify one endpoint of an existing S1Interval.  Requires that
  // the resulting S1Interval is valid.  This implies you cannot call this
  // method on an Empty() or Full() interval, since these intervals do not
  // have any endpoints.
  //
  // Do not use these methods if you want to replace both endpoints of the
  // interval; use a constructor instead.  For example:
  //
  //   *lng_bounds = S1Interval(lng_lo, lng_hi);
  void set_lo(double p) { bounds_[0] = p; DCHECK(is_valid()); }
  void set_hi(double p) { bounds_[1] = p; DCHECK(is_valid()); }

  // An interval is valid if neither bound exceeds Pi in absolute value,
  // and the value -Pi appears only in the Empty() and Full() intervals.
  inline bool is_valid() const;

  // Return true if the interval contains all points on the unit circle.
  bool is_full() const { return hi() - lo() == 2 * M_PI; }

  // Return true if the interval is empty, i.e. it contains no points.
  bool is_empty() const { return lo() - hi() == 2 * M_PI; }

  // Return true if lo() > hi().  (This is true for empty intervals.)
  bool is_inverted() const { return lo() > hi(); }

  // Return the midpoint of the interval.  For full and empty intervals,
  // the result is arbitrary.
  double GetCenter() const;

  // Return the length of the interval.  The length of an empty interval
  // is negative.
  double GetLength() const;

  // Return the complement of the interior of the interval.  An interval and
  // its complement have the same boundary but do not share any interior
  // values.  The complement operator is not a bijection, since the complement
  // of a singleton interval (containing a single value) is the same as the
  // complement of an empty interval.
  S1Interval Complement() const;

  // Return the midpoint of the complement of the interval. For full and empty
  // intervals, the result is arbitrary. For a singleton interval (containing a
  // single point), the result is its antipodal point on S1.
  double GetComplementCenter() const;

  // Return true if the interval (which is closed) contains the point 'p'.
  bool Contains(double p) const;

  // Return true if the interior of the interval contains the point 'p'.
  bool InteriorContains(double p) const;

  // Return true if the interval contains the given interval 'y'.
  // Works for empty, full, and singleton intervals.
  bool Contains(S1Interval const& y) const;

  // Returns true if the interior of this interval contains the entire
  // interval 'y'.  Note that x.InteriorContains(x) is true only when
  // x is the empty or full interval, and x.InteriorContains(S1Interval(p,p))
  // is equivalent to x.InteriorContains(p).
  bool InteriorContains(S1Interval const& y) const;

  // Return true if the two intervals contain any points in common.
  // Note that the point +/-Pi has two representations, so the intervals
  // [-Pi,-3] and [2,Pi] intersect, for example.
  bool Intersects(S1Interval const& y) const;

  // Return true if the interior of this interval contains any point of the
  // interval 'y' (including its boundary).  Works for empty, full, and
  // singleton intervals.
  bool InteriorIntersects(S1Interval const& y) const;

  // Return the Hausdorff distance to the given interval 'y'. For two
  // S1Intervals x and y, this distance is defined by
  //     h(x, y) = max_{p in x} min_{q in y} d(p, q),
  // where d(.,.) is measured along S1.
  double GetDirectedHausdorffDistance(S1Interval const& y) const;

  // Expand the interval by the minimum amount necessary so that it
  // contains the given point "p" (an angle in the range [-Pi, Pi]).
  void AddPoint(double p);

  // Return an interval that contains all points with a distance "radius" of a
  // point in this interval.  Note that the expansion of an empty interval is
  // always empty.  The radius must be non-negative.
  S1Interval Expanded(double radius) const;

  // Return the smallest interval that contains this interval and the
  // given interval "y".
  S1Interval Union(S1Interval const& y) const;

  // Return the smallest interval that contains the intersection of this
  // interval with "y".  Note that the region of intersection may
  // consist of two disjoint intervals.
  S1Interval Intersection(S1Interval const& y) const;

  // Return true if two intervals contains the same set of points.
  inline bool operator==(S1Interval const& y) const;

  // Return true if the length of the symmetric difference between the two
  // intervals is at most the given tolerance.
  bool ApproxEquals(S1Interval const& y, double max_error = 1e-15) const;

 private:
  enum ArgsChecked { ARGS_CHECKED };

  // Internal constructor that assumes that both arguments are in the
  // correct range, i.e. normalization from -Pi to Pi is already done.
  inline S1Interval(double lo, double hi, ArgsChecked dummy);

  // Return true if the interval (which is closed) contains the point 'p'.
  // Skips the normalization of 'p' from -Pi to Pi.
  bool FastContains(double p) const;

  Vector2_d bounds_;
};
DECLARE_POD(S1Interval);

inline S1Interval::S1Interval(double lo, double hi) : bounds_(lo, hi) {
  if (lo == -M_PI && hi != M_PI) set_lo(M_PI);
  if (hi == -M_PI && lo != M_PI) set_hi(M_PI);
  DCHECK(is_valid());
}

inline S1Interval::S1Interval(double lo, double hi, ArgsChecked dummy)
  : bounds_(lo, hi) {
  DCHECK(is_valid());
}

inline S1Interval::S1Interval() : bounds_(M_PI, -M_PI) {
}

inline S1Interval S1Interval::Empty() {
  return S1Interval();
}

inline S1Interval S1Interval::Full() {
  return S1Interval(-M_PI, M_PI, ARGS_CHECKED);
}

inline bool S1Interval::is_valid() const {
  return (fabs(lo()) <= M_PI && fabs(hi()) <= M_PI &&
          !(lo() == -M_PI && hi() != M_PI) &&
          !(hi() == -M_PI && lo() != M_PI));
}

inline bool S1Interval::operator==(S1Interval const& y) const {
  return lo() == y.lo() && hi() == y.hi();
}

inline ostream& operator<<(ostream& os, S1Interval const& x) {
  return os << "[" << x.lo() << ", " << x.hi() << "]";
}

#endif  // UTIL_GEOMETRY_S1INTERVAL_H_
