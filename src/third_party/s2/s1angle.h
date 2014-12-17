// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S1ANGLE_H_
#define UTIL_GEOMETRY_S1ANGLE_H_

// to forward declare ostream
#include <iosfwd>
using std::ostream;
#ifdef OS_WINDOWS
#define _USE_MATH_DEFINES
#include <cmath>
#endif
#include "base/basictypes.h"
#include "util/math/mathutil.h"
#include "s2.h"

class S2LatLng;

// This class represents a one-dimensional angle (as opposed to a
// two-dimensional solid angle).  It has methods for converting angles to
// or from radians, degrees, and the E5/E6/E7 representations (i.e. degrees
// multiplied by 1e5/1e6/1e7 and rounded to the nearest integer).
//
// This class has built-in support for the E5, E6, and E7
// representations.  An E5 is the measure of an angle in degrees,
// multiplied by 10**5.
//
// This class is intended to be copied by value as desired.  It uses
// the default copy constructor and assignment operator.
class S1Angle {
 public:
  // These methods construct S1Angle objects from their measure in radians
  // or degrees.
  inline static S1Angle Radians(double radians);
  inline static S1Angle Degrees(double degrees);
  inline static S1Angle E5(int32 e5);
  inline static S1Angle E6(int32 e6);
  inline static S1Angle E7(int32 e7);

  // Convenience functions -- to use when args have been fixed32s in protos.
  //
  // The arguments are static_cast into int32, so very large unsigned values
  // are treated as negative numbers.
  inline static S1Angle UnsignedE6(uint32 e6);
  inline static S1Angle UnsignedE7(uint32 e7);

  // The default constructor yields a zero angle.  This is useful for STL
  // containers and class methods with output arguments.
  inline S1Angle() : radians_(0) {}

  // Return the angle between two points, which is also equal to the distance
  // between these points on the unit sphere.  The points do not need to be
  // normalized.
  S1Angle(S2Point const& x, S2Point const& y);

  // Like the constructor above, but return the angle (i.e., distance)
  // between two S2LatLng points.
  S1Angle(S2LatLng const& x, S2LatLng const& y);

  double radians() const { return radians_; }
  double degrees() const { return radians_ * (180 / M_PI); }

  int32 e5() const { return MathUtil::FastIntRound(degrees() * 1e5); }
  int32 e6() const { return MathUtil::FastIntRound(degrees() * 1e6); }
  int32 e7() const { return MathUtil::FastIntRound(degrees() * 1e7); }

  // Return the absolute value of an angle.
  S1Angle abs() const { return S1Angle(fabs(radians_)); }

  // Comparison operators.
  friend inline bool operator==(S1Angle const& x, S1Angle const& y);
  friend inline bool operator!=(S1Angle const& x, S1Angle const& y);
  friend inline bool operator<(S1Angle const& x, S1Angle const& y);
  friend inline bool operator>(S1Angle const& x, S1Angle const& y);
  friend inline bool operator<=(S1Angle const& x, S1Angle const& y);
  friend inline bool operator>=(S1Angle const& x, S1Angle const& y);

  // Simple arithmetic operators for manipulating S1Angles.
  friend inline S1Angle operator-(S1Angle const& a);
  friend inline S1Angle operator+(S1Angle const& a, S1Angle const& b);
  friend inline S1Angle operator-(S1Angle const& a, S1Angle const& b);
  friend inline S1Angle operator*(double m, S1Angle const& a);
  friend inline S1Angle operator*(S1Angle const& a, double m);
  friend inline S1Angle operator/(S1Angle const& a, double m);
  friend inline double operator/(S1Angle const& a, S1Angle const& b);
  inline S1Angle& operator+=(S1Angle const& a);
  inline S1Angle& operator-=(S1Angle const& a);
  inline S1Angle& operator*=(double m);
  inline S1Angle& operator/=(double m);

  // Return the angle normalized to the range (-180, 180] degrees.
  S1Angle Normalized() const;

  // Normalize this angle to the range (-180, 180] degrees.
  void Normalize();

 private:
  explicit S1Angle(double radians) : radians_(radians) {}
  double radians_;
};
DECLARE_POD(S1Angle);

inline bool operator==(S1Angle const& x, S1Angle const& y) {
  return x.radians() == y.radians();
}

inline bool operator!=(S1Angle const& x, S1Angle const& y) {
  return x.radians() != y.radians();
}

inline bool operator<(S1Angle const& x, S1Angle const& y) {
  return x.radians() < y.radians();
}

inline bool operator>(S1Angle const& x, S1Angle const& y) {
  return x.radians() > y.radians();
}

inline bool operator<=(S1Angle const& x, S1Angle const& y) {
  return x.radians() <= y.radians();
}

inline bool operator>=(S1Angle const& x, S1Angle const& y) {
  return x.radians() >= y.radians();
}

inline S1Angle operator-(S1Angle const& a) {
  return S1Angle::Radians(-a.radians());
}

inline S1Angle operator+(S1Angle const& a, S1Angle const& b) {
  return S1Angle::Radians(a.radians() + b.radians());
}

inline S1Angle operator-(S1Angle const& a, S1Angle const& b) {
  return S1Angle::Radians(a.radians() - b.radians());
}

inline S1Angle operator*(double m, S1Angle const& a) {
  return S1Angle::Radians(m * a.radians());
}

inline S1Angle operator*(S1Angle const& a, double m) {
  return S1Angle::Radians(m * a.radians());
}

inline S1Angle operator/(S1Angle const& a, double m) {
  return S1Angle::Radians(a.radians() / m);
}

inline double operator/(S1Angle const& a, S1Angle const& b) {
  return a.radians() / b.radians();
}

inline S1Angle& S1Angle::operator+=(S1Angle const& a) {
  radians_ += a.radians();
  return *this;
}

inline S1Angle& S1Angle::operator-=(S1Angle const& a) {
  radians_ -= a.radians();
  return *this;
}

inline S1Angle& S1Angle::operator*=(double m) {
  radians_ *= m;
  return *this;
}

inline S1Angle& S1Angle::operator/=(double m) {
  radians_ /= m;
  return *this;
}

inline S1Angle S1Angle::Radians(double radians) {
  return S1Angle(radians);
}

inline S1Angle S1Angle::Degrees(double degrees) {
  return S1Angle(degrees * (M_PI / 180));
}

inline S1Angle S1Angle::E5(int32 e5) {
  // Multiplying by 1e-5 isn't quite as accurate as dividing by 1e5,
  // but it's about 10 times faster and more than accurate enough.
  return Degrees(e5 * 1e-5);
}

inline S1Angle S1Angle::E6(int32 e6) {
  return Degrees(e6 * 1e-6);
}

inline S1Angle S1Angle::E7(int32 e7) {
  return Degrees(e7 * 1e-7);
}

inline S1Angle S1Angle::UnsignedE6(uint32 e6) {
  return Degrees(static_cast<int32>(e6) * 1e-6);
}

inline S1Angle S1Angle::UnsignedE7(uint32 e7) {
  return Degrees(static_cast<int32>(e7) * 1e-7);
}

// Writes the angle in degrees with 7 digits of precision after the
// decimal point, e.g. "17.3745904".
ostream& operator<<(ostream& os, S1Angle const& a);

#endif  // UTIL_GEOMETRY_S1ANGLE_H_
