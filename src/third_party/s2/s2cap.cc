// Copyright 2005 Google Inc. All Rights Reserved.

#include "base/integral_types.h"
#include "base/logging.h"
#include "s2.h"
#include "s2cap.h"
#include "s2cell.h"
#include "s2latlngrect.h"

namespace {

// Multiply a positive number by this constant to ensure that the result
// of a floating point operation is at least as large as the true
// infinite-precision result.
double const kRoundUp = 1.0 + 1.0 / (uint64(1) << 52);

// Return the cap height corresponding to the given non-negative cap angle in
// radians.  Cap angles of Pi radians or larger yield a full cap.
double GetHeightForAngle(double radians) {
  DCHECK_GE(radians, 0);

  // Caps of Pi radians or more are full.
  if (radians >= M_PI) return 2;

  // The height of the cap can be computed as 1 - cos(radians), but this isn't
  // very accurate for angles close to zero (where cos(radians) is almost 1).
  // Computing it as 2 * (sin(radians / 2) ** 2) gives much better precision.
  double d = sin(0.5 * radians);
  return 2 * d * d;
}

}  // namespace

S2Cap S2Cap::FromAxisAngle(S2Point const& axis, S1Angle const& angle) {
  DCHECK(S2::IsUnitLength(axis));
  DCHECK_GE(angle.radians(), 0);
  return S2Cap(axis, GetHeightForAngle(angle.radians()));
}

S1Angle S2Cap::angle() const {
  // This could also be computed as acos(1 - height_), but the following
  // formula is much more accurate when the cap height is small.  It
  // follows from the relationship h = 1 - cos(theta) = 2 sin^2(theta/2).
  if (is_empty()) return S1Angle::Radians(-1);
  return S1Angle::Radians(2 * asin(sqrt(0.5 * height_)));
}

S2Cap S2Cap::Complement() const {
  // The complement of a full cap is an empty cap, not a singleton.
  // Also make sure that the complement of an empty cap has height 2.
  double height = is_full() ? -1 : 2 - max(height_, 0.0);
  return S2Cap::FromAxisHeight(-axis_, height);
}

bool S2Cap::Contains(S2Cap const& other) const {
  if (is_full() || other.is_empty()) return true;
  return angle().radians() >= axis_.Angle(other.axis_) +
                              other.angle().radians();
}

bool S2Cap::Intersects(S2Cap const& other) const {
  if (is_empty() || other.is_empty()) return false;

  return (angle().radians() + other.angle().radians() >=
          axis_.Angle(other.axis_));
}

bool S2Cap::InteriorIntersects(S2Cap const& other) const {
  // Make sure this cap has an interior and the other cap is non-empty.
  if (height_ <= 0 || other.is_empty()) return false;

  return (angle().radians() + other.angle().radians() >
          axis_.Angle(other.axis_));
}

void S2Cap::AddPoint(S2Point const& p) {
  // Compute the squared chord length, then convert it into a height.
  DCHECK(S2::IsUnitLength(p));
  if (is_empty()) {
    axis_ = p;
    height_ = 0;
  } else {
    // To make sure that the resulting cap actually includes this point,
    // we need to round up the distance calculation.  That is, after
    // calling cap.AddPoint(p), cap.Contains(p) should be true.
    double dist2 = (axis_ - p).Norm2();
    height_ = max(height_, kRoundUp * 0.5 * dist2);
  }
}

void S2Cap::AddCap(S2Cap const& other) {
  if (is_empty()) {
    *this = other;
  } else {
    // See comments for AddPoint().  This could be optimized by doing the
    // calculation in terms of cap heights rather than cap opening angles.
    double radians = axis_.Angle(other.axis_) + other.angle().radians();
    height_ = max(height_, kRoundUp * GetHeightForAngle(radians));
  }
}

S2Cap S2Cap::Expanded(S1Angle const& distance) const {
  DCHECK_GE(distance.radians(), 0);
  if (is_empty()) return Empty();
  return FromAxisAngle(axis_, angle() + distance);
}

S2Cap* S2Cap::Clone() const {
  return new S2Cap(*this);
}

S2Cap S2Cap::GetCapBound() const {
  return *this;
}

S2LatLngRect S2Cap::GetRectBound() const {
  if (is_empty()) return S2LatLngRect::Empty();

  // Convert the axis to a (lat,lng) pair, and compute the cap angle.
  S2LatLng axis_ll(axis_);
  double cap_angle = angle().radians();

  bool all_longitudes = false;
  double lat[2], lng[2];
  lng[0] = -M_PI;
  lng[1] = M_PI;

  // Check whether cap includes the south pole.
  lat[0] = axis_ll.lat().radians() - cap_angle;
  if (lat[0] <= -M_PI_2) {
    lat[0] = -M_PI_2;
    all_longitudes = true;
  }
  // Check whether cap includes the north pole.
  lat[1] = axis_ll.lat().radians() + cap_angle;
  if (lat[1] >= M_PI_2) {
    lat[1] = M_PI_2;
    all_longitudes = true;
  }
  if (!all_longitudes) {
    // Compute the range of longitudes covered by the cap.  We use the law
    // of sines for spherical triangles.  Consider the triangle ABC where
    // A is the north pole, B is the center of the cap, and C is the point
    // of tangency between the cap boundary and a line of longitude.  Then
    // C is a right angle, and letting a,b,c denote the sides opposite A,B,C,
    // we have sin(a)/sin(A) = sin(c)/sin(C), or sin(A) = sin(a)/sin(c).
    // Here "a" is the cap angle, and "c" is the colatitude (90 degrees
    // minus the latitude).  This formula also works for negative latitudes.
    //
    // The formula for sin(a) follows from the relationship h = 1 - cos(a).

    double sin_a = sqrt(height_ * (2 - height_));
    double sin_c = cos(axis_ll.lat().radians());
    if (sin_a <= sin_c) {
      double angle_A = asin(sin_a / sin_c);
      lng[0] = drem(axis_ll.lng().radians() - angle_A, 2 * M_PI);
      lng[1] = drem(axis_ll.lng().radians() + angle_A, 2 * M_PI);
    }
  }
  return S2LatLngRect(R1Interval(lat[0], lat[1]),
                      S1Interval(lng[0], lng[1]));
}

bool S2Cap::Intersects(S2Cell const& cell, S2Point const* vertices) const {
  // Return true if this cap intersects any point of 'cell' excluding its
  // vertices (which are assumed to already have been checked).

  // If the cap is a hemisphere or larger, the cell and the complement of the
  // cap are both convex.  Therefore since no vertex of the cell is contained,
  // no other interior point of the cell is contained either.
  if (height_ >= 1) return false;

  // We need to check for empty caps due to the axis check just below.
  if (is_empty()) return false;

  // Optimization: return true if the cell contains the cap axis.  (This
  // allows half of the edge checks below to be skipped.)
  if (cell.Contains(axis_)) return true;

  // At this point we know that the cell does not contain the cap axis,
  // and the cap does not contain any cell vertex.  The only way that they
  // can intersect is if the cap intersects the interior of some edge.

  double sin2_angle = height_ * (2 - height_);  // sin^2(cap_angle)
  for (int k = 0; k < 4; ++k) {
    S2Point edge = cell.GetEdgeRaw(k);
    double dot = axis_.DotProd(edge);
    if (dot > 0) {
      // The axis is in the interior half-space defined by the edge.  We don't
      // need to consider these edges, since if the cap intersects this edge
      // then it also intersects the edge on the opposite side of the cell
      // (because we know the axis is not contained with the cell).
      continue;
    }
    // The Norm2() factor is necessary because "edge" is not normalized.
    if (dot * dot > sin2_angle * edge.Norm2()) {
      return false;  // Entire cap is on the exterior side of this edge.
    }
    // Otherwise, the great circle containing this edge intersects
    // the interior of the cap.  We just need to check whether the point
    // of closest approach occurs between the two edge endpoints.
    S2Point dir = edge.CrossProd(axis_);
    if (dir.DotProd(vertices[k]) < 0 && dir.DotProd(vertices[(k+1)&3]) > 0)
      return true;
  }
  return false;
}

bool S2Cap::Contains(S2Cell const& cell) const {
  // If the cap does not contain all cell vertices, return false.
  // We check the vertices before taking the Complement() because we can't
  // accurately represent the complement of a very small cap (a height
  // of 2-epsilon is rounded off to 2).
  S2Point vertices[4];
  for (int k = 0; k < 4; ++k) {
    vertices[k] = cell.GetVertex(k);
    if (!Contains(vertices[k])) return false;
  }
  // Otherwise, return true if the complement of the cap does not intersect
  // the cell.  (This test is slightly conservative, because technically we
  // want Complement().InteriorIntersects() here.)
  return !Complement().Intersects(cell, vertices);
}

bool S2Cap::MayIntersect(S2Cell const& cell) const {
  // If the cap contains any cell vertex, return true.
  S2Point vertices[4];
  for (int k = 0; k < 4; ++k) {
    vertices[k] = cell.GetVertex(k);
    if (Contains(vertices[k])) return true;
  }
  return Intersects(cell, vertices);
}

bool S2Cap::Contains(S2Point const& p) const {
  DCHECK(S2::IsUnitLength(p));
  return (axis_ - p).Norm2() <= 2 * height_;
}

bool S2Cap::InteriorContains(S2Point const& p) const {
  DCHECK(S2::IsUnitLength(p));
  return is_full() || (axis_ - p).Norm2() < 2 * height_;
}

bool S2Cap::operator==(S2Cap const& other) const {
  return (axis_ == other.axis_ && height_ == other.height_) ||
         (is_empty() && other.is_empty()) ||
         (is_full() && other.is_full());
}

bool S2Cap::ApproxEquals(S2Cap const& other, double max_error) {
  return (S2::ApproxEquals(axis_, other.axis_, max_error) &&
          fabs(height_ - other.height_) <= max_error) ||
         (is_empty() && other.height_ <= max_error) ||
         (other.is_empty() && height_ <= max_error) ||
         (is_full() && other.height_ >= 2 - max_error) ||
         (other.is_full() && height_ >= 2 - max_error);
}

ostream& operator<<(ostream& os, S2Cap const& cap) {
  return os << "[Axis=" << cap.axis() << ", Angle=" << cap.angle() << "]";
}
