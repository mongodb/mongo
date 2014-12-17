// Copyright 2005 Google Inc. All Rights Reserved.

#include <math.h>
#include <stdio.h>
#include <iostream>
using std::ostream;
using std::cout;
using std::endl;

#include "s1angle.h"
#include "s2latlng.h"

S1Angle::S1Angle(S2Point const& x, S2Point const& y)
    : radians_(x.Angle(y)) {
}

S1Angle::S1Angle(S2LatLng const& x, S2LatLng const& y)
    : radians_(x.GetDistance(y).radians()) {
}

S1Angle S1Angle::Normalized() const {
  S1Angle a(radians_);
  a.Normalize();
  return a;
}

void S1Angle::Normalize() {
  radians_ = drem(radians_, 2.0 * M_PI);
  if (radians_ <= -M_PI) radians_ = M_PI;
}

ostream& operator<<(ostream& os, S1Angle const& a) {
  double degrees = a.degrees();
  char buffer[13];
  int sz = snprintf(buffer, sizeof(buffer), "%.7f", degrees);
  if (sz >= 0 && (size_t)sz < sizeof(buffer)) {
    return os << buffer;
  } else {
    return os << degrees;
  }
}
