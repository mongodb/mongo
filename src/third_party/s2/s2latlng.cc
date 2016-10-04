// Copyright 2005 Google Inc. All Rights Reserved.
#include "s2.h"
#include "base/logging.h"
#include "strings/stringprintf.h"
#include "s2latlng.h"

S2LatLng S2LatLng::Normalized() const {
  // drem(x, 2 * M_PI) reduces its argument to the range [-M_PI, M_PI]
  // inclusive, which is what we want here.
  return S2LatLng(max(-M_PI_2, min(M_PI_2, lat().radians())),
                  drem(lng().radians(), 2 * M_PI));
}

S2Point S2LatLng::ToPoint() const {
  DCHECK(is_valid());
  // As of crosstool v14, gcc tries to calculate sin(phi), cos(phi),
  // sin(theta), cos(theta) on the following section by two sincos()
  // calls. However, for some inputs, sincos() returns significantly
  // different values between AMD and Intel.
  //
  // As a temporary workaround, "volatile" is added to phi and theta
  // to prohibit the compiler to use such sincos() call, because sin()
  // and cos() don't seem to have the problem. See b/3088321 for
  // details.
  volatile double phi = lat().radians();
  volatile double theta = lng().radians();
  double cosphi = cos(phi);
  return S2Point(cos(theta) * cosphi, sin(theta) * cosphi, sin(phi));
}

S2LatLng::S2LatLng(S2Point const& p)
  : coords_(Latitude(p).radians(), Longitude(p).radians()) {
  // The latitude and longitude are already normalized.
  DCHECK(is_valid());
}

S1Angle S2LatLng::GetDistance(S2LatLng const& o) const {
  // This implements the Haversine formula, which is numerically stable for
  // small distances but only gets about 8 digits of precision for very large
  // distances (e.g. antipodal points).  Note that 8 digits is still accurate
  // to within about 10cm for a sphere the size of the Earth.
  //
  // This could be fixed with another sin() and cos() below, but at that point
  // you might as well just convert both arguments to S2Points and compute the
  // distance that way (which gives about 15 digits of accuracy for all
  // distances).

  DCHECK(is_valid());
  DCHECK(o.is_valid());
  double lat1 = lat().radians();
  double lat2 = o.lat().radians();
  double lng1 = lng().radians();
  double lng2 = o.lng().radians();
  double dlat = sin(0.5 * (lat2 - lat1));
  double dlng = sin(0.5 * (lng2 - lng1));
  double x = dlat * dlat + dlng * dlng * cos(lat1) * cos(lat2);
  return S1Angle::Radians(2 * atan2(sqrt(x), sqrt(max(0.0, 1.0 - x))));
}

string S2LatLng::ToStringInDegrees() const {
  S2LatLng pt = Normalized();
  return StringPrintf("%f,%f", pt.lat().degrees(), pt.lng().degrees());
}

void S2LatLng::ToStringInDegrees(string* s) const {
  *s = ToStringInDegrees();
}

ostream& operator<<(ostream& os, S2LatLng const& ll) {
  return os << "[" << ll.lat() << ", " << ll.lng() << "]";
}
