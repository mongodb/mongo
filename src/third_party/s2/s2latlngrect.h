// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S2LATLNGRECT_H_
#define UTIL_GEOMETRY_S2LATLNGRECT_H_

#include <iostream>
using std::ostream;
using std::cout;
using std::endl;


#include "base/basictypes.h"
#include "base/logging.h"
#include "s1angle.h"
#include "r1interval.h"
#include "s1interval.h"
#include "s2region.h"
#include "s2latlng.h"

// An S2LatLngRect represents a closed latitude-longitude rectangle.  It is
// capable of representing the empty and full rectangles as well as
// single points.
//
// This class is intended to be copied by value as desired.  It uses
// the default copy constructor and assignment operator, however it is
// not a "plain old datatype" (POD) because it has virtual functions.
class S2LatLngRect : public S2Region {
 public:
  // Construct a rectangle from minimum and maximum latitudes and longitudes.
  // If lo.lng() > hi.lng(), the rectangle spans the 180 degree longitude
  // line. Both points must be normalized, with lo.lat() <= hi.lat().
  // The rectangle contains all the points p such that 'lo' <= p <= 'hi',
  // where '<=' is defined in the obvious way.
  inline S2LatLngRect(S2LatLng const& lo, S2LatLng const& hi);

  // Construct a rectangle from latitude and longitude intervals.  The two
  // intervals must either be both empty or both non-empty, and the latitude
  // interval must not extend outside [-90, +90] degrees.
  // Note that both intervals (and hence the rectangle) are closed.
  inline S2LatLngRect(R1Interval const& lat, S1Interval const& lng);

  // The default constructor creates an empty S2LatLngRect.
  inline S2LatLngRect();

  // Construct a rectangle of the given size centered around the given point.
  // "center" needs to be normalized, but "size" does not.  The latitude
  // interval of the result is clamped to [-90,90] degrees, and the longitude
  // interval of the result is Full() if and only if the longitude size is
  // 360 degrees or more.  Examples of clamping (in degrees):
  //
  //   center=(80,170),  size=(40,60)   -> lat=[60,90],   lng=[140,-160]
  //   center=(10,40),   size=(210,400) -> lat=[-90,90],  lng=[-180,180]
  //   center=(-90,180), size=(20,50)   -> lat=[-90,-80], lng=[155,-155]
  static S2LatLngRect FromCenterSize(S2LatLng const& center,
                                     S2LatLng const& size);

  // Construct a rectangle containing a single (normalized) point.
  static S2LatLngRect FromPoint(S2LatLng const& p);

  // Construct the minimal bounding rectangle containing the two given
  // normalized points.  This is equivalent to starting with an empty
  // rectangle and calling AddPoint() twice.  Note that it is different than
  // the S2LatLngRect(lo, hi) constructor, where the first point is always
  // used as the lower-left corner of the resulting rectangle.
  static S2LatLngRect FromPointPair(S2LatLng const& p1, S2LatLng const& p2);

  // Accessor methods.
  S1Angle lat_lo() const { return S1Angle::Radians(lat_.lo()); }
  S1Angle lat_hi() const { return S1Angle::Radians(lat_.hi()); }
  S1Angle lng_lo() const { return S1Angle::Radians(lng_.lo()); }
  S1Angle lng_hi() const { return S1Angle::Radians(lng_.hi()); }
  R1Interval const& lat() const { return lat_; }
  S1Interval const& lng() const { return lng_; }
  R1Interval *mutable_lat() { return &lat_; }
  S1Interval *mutable_lng() { return &lng_; }
  S2LatLng lo() const { return S2LatLng(lat_lo(), lng_lo()); }
  S2LatLng hi() const { return S2LatLng(lat_hi(), lng_hi()); }

  // The canonical empty and full rectangles.
  static inline S2LatLngRect Empty();
  static inline S2LatLngRect Full();

  // The full allowable range of latitudes and longitudes.
  static R1Interval FullLat() { return R1Interval(-M_PI_2, M_PI_2); }
  static S1Interval FullLng() { return S1Interval::Full(); }

  // Return true if the rectangle is valid, which essentially just means
  // that the latitude bounds do not exceed Pi/2 in absolute value and
  // the longitude bounds do not exceed Pi in absolute value.  Also, if
  // either the latitude or longitude bound is empty then both must be.
  inline bool is_valid() const;

  // Return true if the rectangle is empty, i.e. it contains no points at all.
  inline bool is_empty() const;

  // Return true if the rectangle is full, i.e. it contains all points.
  inline bool is_full() const;

  // Return true if the rectangle is a point, i.e. lo() == hi()
  inline bool is_point() const;

  // Return true if lng_.lo() > lng_.hi(), i.e. the rectangle crosses
  // the 180 degree longitude line.
  bool is_inverted() const { return lng_.is_inverted(); }

  // Return the k-th vertex of the rectangle (k = 0,1,2,3) in CCW order.
  S2LatLng GetVertex(int k) const;

  // Return the center of the rectangle in latitude-longitude space
  // (in general this is not the center of the region on the sphere).
  S2LatLng GetCenter() const;

  // Return the width and height of this rectangle in latitude-longitude
  // space.  Empty rectangles have a negative width and height.
  S2LatLng GetSize() const;

  // Returns the surface area of this rectangle on the unit sphere.
  double Area() const;

  // More efficient version of Contains() that accepts a S2LatLng rather than
  // an S2Point.  The argument must be normalized.
  bool Contains(S2LatLng const& ll) const;

  // Return true if and only if the given point is contained in the interior
  // of the region (i.e. the region excluding its boundary).  The point 'p'
  // does not need to be normalized.
  bool InteriorContains(S2Point const& p) const;

  // More efficient version of InteriorContains() that accepts a S2LatLng
  // rather than an S2Point.  The argument must be normalized.
  bool InteriorContains(S2LatLng const& ll) const;

  // Return true if and only if the rectangle contains the given other
  // rectangle.
  bool Contains(S2LatLngRect const& other) const;

  // Return true if and only if the interior of this rectangle contains all
  // points of the given other rectangle (including its boundary).
  bool InteriorContains(S2LatLngRect const& other) const;

  // Return true if this rectangle and the given other rectangle have any
  // points in common.
  bool Intersects(S2LatLngRect const& other) const;

  // Returns true if this rectangle intersects the given cell.  (This is an
  // exact test and may be fairly expensive, see also MayIntersect below.)
  bool Intersects(S2Cell const& cell) const;

  // Return true if and only if the interior of this rectangle intersects
  // any point (including the boundary) of the given other rectangle.
  bool InteriorIntersects(S2LatLngRect const& other) const;

  // Increase the size of the bounding rectangle to include the given point.
  // The rectangle is expanded by the minimum amount possible.  The S2LatLng
  // argument must be normalized.
  void AddPoint(S2Point const& p);
  void AddPoint(S2LatLng const& ll);

  // Return a rectangle that contains all points whose latitude distance from
  // this rectangle is at most margin.lat(), and whose longitude distance
  // from this rectangle is at most margin.lng().  In particular, latitudes
  // are clamped while longitudes are wrapped.  Note that any expansion of an
  // empty interval remains empty, and both components of the given margin
  // must be non-negative.  "margin" does not need to be normalized.
  //
  // NOTE: If you are trying to grow a rectangle by a certain *distance* on
  // the sphere (e.g. 5km), use the ConvolveWithCap() method instead.
  S2LatLngRect Expanded(S2LatLng const& margin) const;

  // Return the smallest rectangle containing the union of this rectangle and
  // the given rectangle.
  S2LatLngRect Union(S2LatLngRect const& other) const;

  // Return the smallest rectangle containing the intersection of this
  // rectangle and the given rectangle.  Note that the region of intersection
  // may consist of two disjoint rectangles, in which case a single rectangle
  // spanning both of them is returned.
  S2LatLngRect Intersection(S2LatLngRect const& other) const;

  // Return a rectangle that contains the convolution of this rectangle with a
  // cap of the given angle.  This expands the rectangle by a fixed distance
  // (as opposed to growing the rectangle in latitude-longitude space).  The
  // returned rectangle includes all points whose minimum distance to the
  // original rectangle is at most the given angle.
  S2LatLngRect ConvolveWithCap(S1Angle const& angle) const;

  // Returns the minimum distance (measured along the surface of the sphere) to
  // the given S2LatLngRect. Both S2LatLngRects must be non-empty.
  S1Angle GetDistance(S2LatLngRect const& other) const;

  // Returns the minimum distance (measured along the surface of the sphere)
  // from a given point to the rectangle (both its boundary and its interior).
  // The latlng must be valid.
  S1Angle GetDistance(S2LatLng const& p) const;

  // Returns the (directed or undirected) Hausdorff distance (measured along the
  // surface of the sphere) to the given S2LatLngRect. The directed Hausdorff
  // distance from rectangle A to rectangle B is given by
  //     h(A, B) = max_{p in A} min_{q in B} d(p, q).
  // The Hausdorff distance between rectangle A and rectangle B is given by
  //     H(A, B) = max{h(A, B), h(B, A)}.
  S1Angle GetDirectedHausdorffDistance(S2LatLngRect const& other) const;
  S1Angle GetHausdorffDistance(S2LatLngRect const& other) const;

  // Return true if two rectangles contains the same set of points.
  inline bool operator==(S2LatLngRect const& other) const;

  // Return the opposite of what operator == returns.
  inline bool operator!=(S2LatLngRect const& other) const;

  // Return true if the latitude and longitude intervals of the two rectangles
  // are the same up to the given tolerance (see r1interval.h and s1interval.h
  // for details).
  bool ApproxEquals(S2LatLngRect const& other, double max_error = 1e-15) const;

  ////////////////////////////////////////////////////////////////////////
  // S2Region interface (see s2region.h for details):

  virtual S2LatLngRect* Clone() const;
  virtual S2Cap GetCapBound() const;
  virtual S2LatLngRect GetRectBound() const;
  virtual bool Contains(S2Cell const& cell) const;
  virtual bool VirtualContainsPoint(S2Point const& p) const {
    return Contains(p);  // The same as Contains() below, just virtual.
  }

  // This test is cheap but is NOT exact.  Use Intersects() if you want a more
  // accurate and more expensive test.  Note that when this method is used by
  // an S2RegionCoverer, the accuracy isn't all that important since if a cell
  // may intersect the region then it is subdivided, and the accuracy of this
  // method goes up as the cells get smaller.
  virtual bool MayIntersect(S2Cell const& cell) const;

  // The point 'p' does not need to be normalized.
  bool Contains(S2Point const& p) const;

  virtual void Encode(Encoder* const encoder) const;
  virtual bool Decode(Decoder* const decoder);

 private:
  // Return true if the edge AB intersects the given edge of constant
  // longitude.
  static bool IntersectsLngEdge(S2Point const& a, S2Point const& b,
                                R1Interval const& lat, double lng);

  // Return true if the edge AB intersects the given edge of constant
  // latitude.
  static bool IntersectsLatEdge(S2Point const& a, S2Point const& b,
                                double lat, S1Interval const& lng);

  // Helper function. See .cc for description.
  static S1Angle GetDirectedHausdorffDistance(double lng_diff,
                                              R1Interval const& a_lat,
                                              R1Interval const& b_lat);

  // Helper function. See .cc for description.
  static S1Angle GetInteriorMaxDistance(R1Interval const& a_lat,
                                        S2Point const& b);

  // Helper function. See .cc for description.
  static S2Point GetBisectorIntersection(R1Interval const& lat, double lng);

  R1Interval lat_;
  S1Interval lng_;
};

inline S2LatLngRect::S2LatLngRect(S2LatLng const& lo, S2LatLng const& hi)
  : lat_(lo.lat().radians(), hi.lat().radians()),
    lng_(lo.lng().radians(), hi.lng().radians()) {
  DCHECK(is_valid()) << lo << ", " << hi;
}

inline S2LatLngRect::S2LatLngRect(R1Interval const& lat, S1Interval const& lng)
  : lat_(lat), lng_(lng) {
  DCHECK(is_valid()) << lat << ", " << lng;
}

inline S2LatLngRect::S2LatLngRect()
    : lat_(R1Interval::Empty()), lng_(S1Interval::Empty()) {
}

inline S2LatLngRect S2LatLngRect::Empty() {
  return S2LatLngRect();
}

inline S2LatLngRect S2LatLngRect::Full() {
  return S2LatLngRect(FullLat(), FullLng());
}

inline bool S2LatLngRect::is_valid() const {
  // The lat/lng ranges must either be both empty or both non-empty.
  return (fabs(lat_.lo()) <= M_PI_2 &&
          fabs(lat_.hi()) <= M_PI_2 &&
          lng_.is_valid() &&
          lat_.is_empty() == lng_.is_empty());
}

inline bool S2LatLngRect::is_empty() const {
  return lat_.is_empty();
}

inline bool S2LatLngRect::is_full() const {
  return lat_ == FullLat() && lng_.is_full();
}

inline bool S2LatLngRect::is_point() const {
  return lat_.lo() == lat_.hi() && lng_.lo() == lng_.hi();
}

inline bool S2LatLngRect::operator==(S2LatLngRect const& other) const {
  return lat() == other.lat() && lng() == other.lng();
}

inline bool S2LatLngRect::operator!=(S2LatLngRect const& other) const {
  return !operator==(other);
}

ostream& operator<<(ostream& os, S2LatLngRect const& r);

#endif  // UTIL_GEOMETRY_S2LATLNGRECT_H_
