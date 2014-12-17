// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S2LATLNG_H__
#define UTIL_GEOMETRY_S2LATLNG_H__

#include <string>
using std::string;

#include <ostream>
#include "base/basictypes.h"
#include "s1angle.h"
#include "s2.h"
#include "util/math/vector2-inl.h"

// This class represents a point on the unit sphere as a pair
// of latitude-longitude coordinates.  Like the rest of the "geometry"
// package, the intent is to represent spherical geometry as a mathematical
// abstraction, so functions that are specifically related to the Earth's
// geometry (e.g. easting/northing conversions) should be put elsewhere.
//
// This class is intended to be copied by value as desired.  It uses
// the default copy constructor and assignment operator.
class S2LatLng {
 public:
  // Constructor.  The latitude and longitude are allowed to be outside
  // the is_valid() range.  However, note that most methods that accept
  // S2LatLngs expect them to be normalized (see Normalized() below).
  inline S2LatLng(S1Angle const& lat, S1Angle const& lng);

  // The default constructor sets the latitude and longitude to zero.  This is
  // mainly useful when declaring arrays, STL containers, etc.
  inline S2LatLng();

  // Convert a direction vector (not necessarily unit length) to an S2LatLng.
  explicit S2LatLng(S2Point const& p);

  // Returns an S2LatLng for which is_valid() will return false.
  inline static S2LatLng Invalid();

  // Convenience functions -- shorter than calling S1Angle::Radians(), etc.
  inline static S2LatLng FromRadians(double lat_radians, double lng_radians);
  inline static S2LatLng FromDegrees(double lat_degrees, double lng_degrees);
  inline static S2LatLng FromE5(int32 lat_e5, int32 lng_e5);
  inline static S2LatLng FromE6(int32 lat_e6, int32 lng_e6);
  inline static S2LatLng FromE7(int32 lat_e7, int32 lng_e7);

  // Convenience functions -- to use when args have been fixed32s in protos.
  //
  // The arguments are static_cast into int32, so very large unsigned values
  // are treated as negative numbers.
  inline static S2LatLng FromUnsignedE6(uint32 lat_e6, uint32 lng_e6);
  inline static S2LatLng FromUnsignedE7(uint32 lat_e7, uint32 lng_e7);

  // Methods to compute the latitude and longitude of a point separately.
  inline static S1Angle Latitude(S2Point const& p);
  inline static S1Angle Longitude(S2Point const& p);

  // Accessor methods.
  S1Angle lat() const { return S1Angle::Radians(coords_[0]); }
  S1Angle lng() const { return S1Angle::Radians(coords_[1]); }
  Vector2_d const& coords() const { return coords_; }

  // Return true if the latitude is between -90 and 90 degrees inclusive
  // and the longitude is between -180 and 180 degrees inclusive.
  inline bool is_valid() const;

  // Clamps the latitude to the range [-90, 90] degrees, and adds or subtracts
  // a multiple of 360 degrees to the longitude if necessary to reduce it to
  // the range [-180, 180].
  S2LatLng Normalized() const;

  // Convert a normalized S2LatLng to the equivalent unit-length vector.
  S2Point ToPoint() const;

  // Return the distance (measured along the surface of the sphere) to the
  // given S2LatLng.  This is mathematically equivalent to:
  //
  //   S1Angle::Radians(ToPoint().Angle(o.ToPoint()))
  //
  // but this implementation is slightly more efficient.  Both S2LatLngs
  // must be normalized.
  S1Angle GetDistance(S2LatLng const& o) const;

  // Simple arithmetic operations for manipulating latitude-longitude pairs.
  // The results are not normalized (see Normalized()).
  friend inline S2LatLng operator+(S2LatLng const& a, S2LatLng const& b);
  friend inline S2LatLng operator-(S2LatLng const& a, S2LatLng const& b);
  friend inline S2LatLng operator*(double m, S2LatLng const& a);
  friend inline S2LatLng operator*(S2LatLng const& a, double m);

  bool operator==(S2LatLng const& o) const { return coords_ == o.coords_; }
  bool operator!=(S2LatLng const& o) const { return coords_ != o.coords_; }
  bool operator<(S2LatLng const& o) const { return coords_ < o.coords_; }
  bool operator>(S2LatLng const& o) const { return coords_ > o.coords_; }
  bool operator<=(S2LatLng const& o) const { return coords_ <= o.coords_; }
  bool operator>=(S2LatLng const& o) const { return coords_ >= o.coords_; }

  bool ApproxEquals(S2LatLng const& o, double max_error = 1e-15) const {
    return coords_.aequal(o.coords_, max_error);
  }

  // Export the latitude and longitude in degrees, separated by a comma.
  // e.g. "94.518000,150.300000"
  string ToStringInDegrees() const;
  void ToStringInDegrees(string* s) const;
  string toString() const { return ToStringInDegrees(); }

 private:
  // Internal constructor.
  inline S2LatLng(Vector2_d const& coords) : coords_(coords) {}

  // This is internal to avoid ambiguity about which units are expected.
  inline S2LatLng(double lat_radians, double lng_radians)
    : coords_(lat_radians, lng_radians) {}

  Vector2_d coords_;
};
DECLARE_POD(S2LatLng);

inline S2LatLng::S2LatLng(S1Angle const& lat, S1Angle const& lng)
    : coords_(lat.radians(), lng.radians()) {}

inline S2LatLng::S2LatLng() : coords_(0, 0) {}

inline S2LatLng S2LatLng::FromRadians(double lat_radians, double lng_radians) {
  return S2LatLng(lat_radians, lng_radians);
}

inline S2LatLng S2LatLng::FromDegrees(double lat_degrees, double lng_degrees) {
  return S2LatLng(S1Angle::Degrees(lat_degrees), S1Angle::Degrees(lng_degrees));
}

inline S2LatLng S2LatLng::FromE5(int32 lat_e5, int32 lng_e5) {
  return S2LatLng(S1Angle::E5(lat_e5), S1Angle::E5(lng_e5));
}

inline S2LatLng S2LatLng::FromE6(int32 lat_e6, int32 lng_e6) {
  return S2LatLng(S1Angle::E6(lat_e6), S1Angle::E6(lng_e6));
}

inline S2LatLng S2LatLng::FromE7(int32 lat_e7, int32 lng_e7) {
  return S2LatLng(S1Angle::E7(lat_e7), S1Angle::E7(lng_e7));
}

inline S2LatLng S2LatLng::FromUnsignedE6(uint32 lat_e6, uint32 lng_e6) {
  return S2LatLng(S1Angle::UnsignedE6(lat_e6), S1Angle::UnsignedE6(lng_e6));
}

inline S2LatLng S2LatLng::FromUnsignedE7(uint32 lat_e7, uint32 lng_e7) {
  return S2LatLng(S1Angle::UnsignedE7(lat_e7), S1Angle::UnsignedE7(lng_e7));
}

inline S2LatLng S2LatLng::Invalid() {
  // These coordinates are outside the bounds allowed by is_valid().
  return S2LatLng(M_PI, 2 * M_PI);
}

inline S1Angle S2LatLng::Latitude(S2Point const& p) {
  // We use atan2 rather than asin because the input vector is not necessarily
  // unit length, and atan2 is much more accurate than asin near the poles.
  return S1Angle::Radians(atan2(p[2], sqrt(p[0]*p[0] + p[1]*p[1])));
}

inline S1Angle S2LatLng::Longitude(S2Point const& p) {
  // Note that atan2(0, 0) is defined to be zero.
  return S1Angle::Radians(atan2(p[1], p[0]));
}

inline bool S2LatLng::is_valid() const {
  return fabs(lat().radians()) <= M_PI_2 && fabs(lng().radians()) <= M_PI;
}

inline S2LatLng operator+(S2LatLng const& a, S2LatLng const& b) {
  return S2LatLng(a.coords_ + b.coords_);
}

inline S2LatLng operator-(S2LatLng const& a, S2LatLng const& b) {
  return S2LatLng(a.coords_ - b.coords_);
}

inline S2LatLng operator*(double m, S2LatLng const& a) {
  return S2LatLng(m * a.coords_);
}

inline S2LatLng operator*(S2LatLng const& a, double m) {
  return S2LatLng(m * a.coords_);
}

ostream& operator<<(ostream& os, S2LatLng const& ll);

#endif  // UTIL_GEOMETRY_S2LATLNG_H__
