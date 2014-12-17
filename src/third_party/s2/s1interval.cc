// Copyright 2005 Google Inc. All Rights Reserved.

#include "s1interval.h"

#include "base/logging.h"

S1Interval S1Interval::FromPoint(double p) {
  if (p == -M_PI) p = M_PI;
  return S1Interval(p, p, ARGS_CHECKED);
}

double S1Interval::GetCenter() const {
  double center = 0.5 * (lo() + hi());
  if (!is_inverted()) return center;
  // Return the center in the range (-Pi, Pi].
  return (center <= 0) ? (center + M_PI) : (center - M_PI);
}

double S1Interval::GetLength() const {
  double length = hi() - lo();
  if (length >= 0) return length;
  length += 2 * M_PI;
  // Empty intervals have a negative length.
  return (length > 0) ? length : -1;
}

S1Interval S1Interval::Complement() const {
  if (lo() == hi()) return Full();   // Singleton.
  return S1Interval(hi(), lo(), ARGS_CHECKED);  // Handles empty and full.
}

double S1Interval::GetComplementCenter() const {
  if (lo() != hi()) {
    return Complement().GetCenter();
  } else {  // Singleton.
    return (hi() <= 0) ? (hi() + M_PI) : (hi() - M_PI);
  }
}

bool S1Interval::FastContains(double p) const {
  if (is_inverted()) {
    return (p >= lo() || p <= hi()) && !is_empty();
  } else {
    return p >= lo() && p <= hi();
  }
}

bool S1Interval::Contains(double p) const {
  // Works for empty, full, and singleton intervals.
  DCHECK_LE(fabs(p), M_PI);
  if (p == -M_PI) p = M_PI;
  return FastContains(p);
}

bool S1Interval::InteriorContains(double p) const {
  // Works for empty, full, and singleton intervals.
  DCHECK_LE(fabs(p), M_PI);
  if (p == -M_PI) p = M_PI;

  if (is_inverted()) {
    return p > lo() || p < hi();
  } else {
    return (p > lo() && p < hi()) || is_full();
  }
}

bool S1Interval::Contains(S1Interval const& y) const {
  // It might be helpful to compare the structure of these tests to
  // the simpler Contains(double) method above.

  if (is_inverted()) {
    if (y.is_inverted()) return y.lo() >= lo() && y.hi() <= hi();
    return (y.lo() >= lo() || y.hi() <= hi()) && !is_empty();
  } else {
    if (y.is_inverted()) return is_full() || y.is_empty();
    return y.lo() >= lo() && y.hi() <= hi();
  }
}

bool S1Interval::InteriorContains(S1Interval const& y) const {
  if (is_inverted()) {
    if (!y.is_inverted()) return y.lo() > lo() || y.hi() < hi();
    return (y.lo() > lo() && y.hi() < hi()) || y.is_empty();
  } else {
    if (y.is_inverted()) return is_full() || y.is_empty();
    return (y.lo() > lo() && y.hi() < hi()) || is_full();
  }
}

bool S1Interval::Intersects(S1Interval const& y) const {
  if (is_empty() || y.is_empty()) return false;
  if (is_inverted()) {
    // Every non-empty inverted interval contains Pi.
    return y.is_inverted() || y.lo() <= hi() || y.hi() >= lo();
  } else {
    if (y.is_inverted()) return y.lo() <= hi() || y.hi() >= lo();
    return y.lo() <= hi() && y.hi() >= lo();
  }
}

bool S1Interval::InteriorIntersects(S1Interval const& y) const {
  if (is_empty() || y.is_empty() || lo() == hi()) return false;
  if (is_inverted()) {
    return y.is_inverted() || y.lo() < hi() || y.hi() > lo();
  } else {
    if (y.is_inverted()) return y.lo() < hi() || y.hi() > lo();
    return (y.lo() < hi() && y.hi() > lo()) || is_full();
  }
}

inline static double PositiveDistance(double a, double b) {
  // Compute the distance from "a" to "b" in the range [0, 2*Pi).
  // This is equivalent to (drem(b - a - M_PI, 2 * M_PI) + M_PI),
  // except that it is more numerically stable (it does not lose
  // precision for very small positive distances).
  double d = b - a;
  if (d >= 0) return d;
  // We want to ensure that if b == Pi and a == (-Pi + eps),
  // the return result is approximately 2*Pi and not zero.
  return (b + M_PI) - (a - M_PI);
}

double S1Interval::GetDirectedHausdorffDistance(S1Interval const& y) const {
  if (y.Contains(*this)) return 0.0;  // this includes the case *this is empty
  if (y.is_empty()) return M_PI;  // maximum possible distance on S1

  double y_complement_center = y.GetComplementCenter();
  if (Contains(y_complement_center)) {
    return PositiveDistance(y.hi(), y_complement_center);
  } else {
    // The Hausdorff distance is realized by either two hi() endpoints or two
    // lo() endpoints, whichever is farther apart.
    double hi_hi = S1Interval(y.hi(), y_complement_center).Contains(hi()) ?
        PositiveDistance(y.hi(), hi()) : 0;
    double lo_lo = S1Interval(y_complement_center, y.lo()).Contains(lo()) ?
        PositiveDistance(lo(), y.lo()) : 0;
    DCHECK(hi_hi > 0 || lo_lo > 0);
    return max(hi_hi, lo_lo);
  }
}

void S1Interval::AddPoint(double p) {
  DCHECK_LE(fabs(p), M_PI);
  if (p == -M_PI) p = M_PI;

  if (FastContains(p)) return;
  if (is_empty()) {
    set_hi(p);
    set_lo(p);
  } else {
    // Compute distance from p to each endpoint.
    double dlo = PositiveDistance(p, lo());
    double dhi = PositiveDistance(hi(), p);
    if (dlo < dhi) {
      set_lo(p);
    } else {
      set_hi(p);
    }
    // Adding a point can never turn a non-full interval into a full one.
  }
}

S1Interval S1Interval::FromPointPair(double p1, double p2) {
  DCHECK_LE(fabs(p1), M_PI);
  DCHECK_LE(fabs(p2), M_PI);
  if (p1 == -M_PI) p1 = M_PI;
  if (p2 == -M_PI) p2 = M_PI;
  if (PositiveDistance(p1, p2) <= M_PI) {
    return S1Interval(p1, p2, ARGS_CHECKED);
  } else {
    return S1Interval(p2, p1, ARGS_CHECKED);
  }
}

S1Interval S1Interval::Expanded(double radius) const {
  DCHECK_GE(radius, 0);
  if (is_empty()) return *this;

  // Check whether this interval will be full after expansion, allowing
  // for a 1-bit rounding error when computing each endpoint.
  if (GetLength() + 2 * radius >= 2 * M_PI - 1e-15) return Full();

  S1Interval result(drem(lo() - radius, 2*M_PI), drem(hi() + radius, 2*M_PI));
  if (result.lo() <= -M_PI) result.set_lo(M_PI);
  return result;
}

S1Interval S1Interval::Union(S1Interval const& y) const {
  // The y.is_full() case is handled correctly in all cases by the code
  // below, but can follow three separate code paths depending on whether
  // this interval is inverted, is non-inverted but contains Pi, or neither.

  if (y.is_empty()) return *this;
  if (FastContains(y.lo())) {
    if (FastContains(y.hi())) {
      // Either this interval contains y, or the union of the two
      // intervals is the Full() interval.
      if (Contains(y)) return *this;  // is_full() code path
      return Full();
    }
    return S1Interval(lo(), y.hi(), ARGS_CHECKED);
  }
  if (FastContains(y.hi())) return S1Interval(y.lo(), hi(), ARGS_CHECKED);

  // This interval contains neither endpoint of y.  This means that either y
  // contains all of this interval, or the two intervals are disjoint.
  if (is_empty() || y.FastContains(lo())) return y;

  // Check which pair of endpoints are closer together.
  double dlo = PositiveDistance(y.hi(), lo());
  double dhi = PositiveDistance(hi(), y.lo());
  if (dlo < dhi) {
    return S1Interval(y.lo(), hi(), ARGS_CHECKED);
  } else {
    return S1Interval(lo(), y.hi(), ARGS_CHECKED);
  }
}

S1Interval S1Interval::Intersection(S1Interval const& y) const {
  // The y.is_full() case is handled correctly in all cases by the code
  // below, but can follow three separate code paths depending on whether
  // this interval is inverted, is non-inverted but contains Pi, or neither.

  if (y.is_empty()) return Empty();
  if (FastContains(y.lo())) {
    if (FastContains(y.hi())) {
      // Either this interval contains y, or the region of intersection
      // consists of two disjoint subintervals.  In either case, we want
      // to return the shorter of the two original intervals.
      if (y.GetLength() < GetLength()) return y;  // is_full() code path
      return *this;
    }
    return S1Interval(y.lo(), hi(), ARGS_CHECKED);
  }
  if (FastContains(y.hi())) return S1Interval(lo(), y.hi(), ARGS_CHECKED);

  // This interval contains neither endpoint of y.  This means that either y
  // contains all of this interval, or the two intervals are disjoint.

  if (y.FastContains(lo())) return *this;  // is_empty() okay here
  DCHECK(!Intersects(y));
  return Empty();
}

bool S1Interval::ApproxEquals(S1Interval const& y, double max_error) const {
  if (is_empty()) return y.GetLength() <= max_error;
  if (y.is_empty()) return GetLength() <= max_error;
  return (fabs(drem(y.lo() - lo(), 2 * M_PI)) +
          fabs(drem(y.hi() - hi(), 2 * M_PI))) <= max_error;
}
