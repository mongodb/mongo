// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2cell.h"

#include "base/integral_types.h"
#include "base/logging.h"
#include "s2.h"
#include "s2cap.h"
#include "s2latlngrect.h"
#include "util/math/vector2-inl.h"

// Since S2Cells are copied by value, the following assertion is a reminder
// not to add fields unnecessarily.  An S2Cell currently consists of 43 data
// bytes, one vtable pointer, plus alignment overhead.  This works out to 48
// bytes on 32 bit architectures and 56 bytes on 64 bit architectures.
//
// The expression below rounds up (43 + sizeof(void*)) to the nearest
// multiple of sizeof(void*).
#pragma warning(push)
#pragma warning( disable: 4146 )
COMPILE_ASSERT(sizeof(S2Cell) <= ((43+2*sizeof(void*)-1) & -sizeof(void*)),
               S2Cell_is_getting_bloated);
#pragma warning(pop)

S2Point S2Cell::GetVertexRaw(int k) const {
  // Vertices are returned in the order SW, SE, NE, NW.
  return S2::FaceUVtoXYZ(face_, uv_[0][(k>>1) ^ (k&1)], uv_[1][k>>1]);
}

S2Point S2Cell::GetEdgeRaw(int k) const {
  switch (k) {
    case 0:  return S2::GetVNorm(face_, uv_[1][0]);   // South
    case 1:  return S2::GetUNorm(face_, uv_[0][1]);   // East
    case 2:  return -S2::GetVNorm(face_, uv_[1][1]);  // North
    default: return -S2::GetUNorm(face_, uv_[0][0]);  // West
  }
}

void S2Cell::Init(S2CellId const& id) {
  id_ = id;
  int ij[2], orientation;
  face_ = id.ToFaceIJOrientation(&ij[0], &ij[1], &orientation);
  orientation_ = orientation;  // Compress int to a byte.
  level_ = id.level();
  int cellSize = GetSizeIJ();  // Depends on level_.
  for (int d = 0; d < 2; ++d) {
    int ij_lo = ij[d] & -cellSize;
    int ij_hi = ij_lo + cellSize;
    uv_[d][0] = S2::STtoUV((1.0 / S2CellId::kMaxSize) * ij_lo);
    uv_[d][1] = S2::STtoUV((1.0 / S2CellId::kMaxSize) * ij_hi);
  }
}

bool S2Cell::Subdivide(S2Cell children[4]) const {
  // This function is equivalent to just iterating over the child cell ids
  // and calling the S2Cell constructor, but it is about 2.5 times faster.

  if (id_.is_leaf()) return false;

  // Compute the cell midpoint in uv-space.
  Vector2_d uv_mid = id_.GetCenterUV();

  // Create four children with the appropriate bounds.
  S2CellId id = id_.child_begin();
  for (int pos = 0; pos < 4; ++pos, id = id.next()) {
    S2Cell *child = &children[pos];
    child->face_ = face_;
    child->level_ = level_ + 1;
    child->orientation_ = orientation_ ^ S2::kPosToOrientation[pos];
    child->id_ = id;
    // We want to split the cell in half in "u" and "v".  To decide which
    // side to set equal to the midpoint value, we look at cell's (i,j)
    // position within its parent.  The index for "i" is in bit 1 of ij.
    int ij = S2::kPosToIJ[orientation_][pos];
    int i = ij >> 1;
    int j = ij & 1;
    child->uv_[0][i] = uv_[0][i];
    child->uv_[0][1-i] = uv_mid[0];
    child->uv_[1][j] = uv_[1][j];
    child->uv_[1][1-j] = uv_mid[1];
  }
  return true;
}

S2Point S2Cell::GetCenterRaw() const {
  return id_.ToPointRaw();
}

double S2Cell::AverageArea(int level) {
  return S2::kAvgArea.GetValue(level);
}

double S2Cell::ApproxArea() const {
  // All cells at the first two levels have the same area.
  if (level_ < 2) return AverageArea(level_);

  // First, compute the approximate area of the cell when projected
  // perpendicular to its normal.  The cross product of its diagonals gives
  // the normal, and the length of the normal is twice the projected area.
  double flat_area = 0.5 * (GetVertex(2) - GetVertex(0)).
                     CrossProd(GetVertex(3) - GetVertex(1)).Norm();

  // Now, compensate for the curvature of the cell surface by pretending
  // that the cell is shaped like a spherical cap.  The ratio of the
  // area of a spherical cap to the area of its projected disc turns out
  // to be 2 / (1 + sqrt(1 - r*r)) where "r" is the radius of the disc.
  // For example, when r=0 the ratio is 1, and when r=1 the ratio is 2.
  // Here we set Pi*r*r == flat_area to find the equivalent disc.
  return flat_area * 2 / (1 + sqrt(1 - min(M_1_PI * flat_area, 1.0)));
}

double S2Cell::ExactArea() const {
  S2Point v0 = GetVertex(0);
  S2Point v1 = GetVertex(1);
  S2Point v2 = GetVertex(2);
  S2Point v3 = GetVertex(3);
  return S2::Area(v0, v1, v2) + S2::Area(v0, v2, v3);
}

S2Cell* S2Cell::Clone() const {
  return new S2Cell(*this);
}

S2Cap S2Cell::GetCapBound() const {
  // Use the cell center in (u,v)-space as the cap axis.  This vector is
  // very close to GetCenter() and faster to compute.  Neither one of these
  // vectors yields the bounding cap with minimal surface area, but they
  // are both pretty close.
  //
  // It's possible to show that the two vertices that are furthest from
  // the (u,v)-origin never determine the maximum cap size (this is a
  // possible future optimization).

  double u = 0.5 * (uv_[0][0] + uv_[0][1]);
  double v = 0.5 * (uv_[1][0] + uv_[1][1]);
  S2Cap cap = S2Cap::FromAxisHeight(S2::FaceUVtoXYZ(face_,u,v).Normalize(), 0);
  for (int k = 0; k < 4; ++k) {
    cap.AddPoint(GetVertex(k));
  }
  return cap;
}

inline double S2Cell::GetLatitude(int i, int j) const {
  S2Point p = S2::FaceUVtoXYZ(face_, uv_[0][i], uv_[1][j]);
  return S2LatLng::Latitude(p).radians();
}

inline double S2Cell::GetLongitude(int i, int j) const {
  S2Point p = S2::FaceUVtoXYZ(face_, uv_[0][i], uv_[1][j]);
  return S2LatLng::Longitude(p).radians();
}

S2LatLngRect S2Cell::GetRectBound() const {
  if (level_ > 0) {
    // Except for cells at level 0, the latitude and longitude extremes are
    // attained at the vertices.  Furthermore, the latitude range is
    // determined by one pair of diagonally opposite vertices and the
    // longitude range is determined by the other pair.
    //
    // We first determine which corner (i,j) of the cell has the largest
    // absolute latitude.  To maximize latitude, we want to find the point in
    // the cell that has the largest absolute z-coordinate and the smallest
    // absolute x- and y-coordinates.  To do this we look at each coordinate
    // (u and v), and determine whether we want to minimize or maximize that
    // coordinate based on the axis direction and the cell's (u,v) quadrant.
    double u = uv_[0][0] + uv_[0][1];
    double v = uv_[1][0] + uv_[1][1];
    int i = S2::GetUAxis(face_)[2] == 0 ? (u < 0) : (u > 0);
    int j = S2::GetVAxis(face_)[2] == 0 ? (v < 0) : (v > 0);

    // We grow the bounds slightly to make sure that the bounding rectangle
    // also contains the normalized versions of the vertices.  Note that the
    // maximum result magnitude is Pi, with a floating-point exponent of 1.
    // Therefore adding or subtracting 2**-51 will always change the result.
    static double const kMaxError = 1.0 / (int64(1) << 51);
    R1Interval lat = R1Interval::FromPointPair(GetLatitude(i, j),
                                               GetLatitude(1-i, 1-j));
    lat = lat.Expanded(kMaxError).Intersection(S2LatLngRect::FullLat());
    if (lat.lo() == -M_PI_2 || lat.hi() == M_PI_2) {
      return S2LatLngRect(lat, S1Interval::Full());
    }
    S1Interval lng = S1Interval::FromPointPair(GetLongitude(i, 1-j),
                                               GetLongitude(1-i, j));
    return S2LatLngRect(lat, lng.Expanded(kMaxError));
  }

  // The 4 cells around the equator extend to +/-45 degrees latitude at the
  // midpoints of their top and bottom edges.  The two cells covering the
  // poles extend down to +/-35.26 degrees at their vertices.
  static double const kPoleMinLat = asin(sqrt(1./3));  // 35.26 degrees

  // The face centers are the +X, +Y, +Z, -X, -Y, -Z axes in that order.
  DCHECK_EQ(((face_ < 3) ? 1 : -1), S2::GetNorm(face_)[face_ % 3]);
  switch (face_) {
    case 0:  return S2LatLngRect(R1Interval(-M_PI_4, M_PI_4),
                                 S1Interval(-M_PI_4, M_PI_4));
    case 1:  return S2LatLngRect(R1Interval(-M_PI_4, M_PI_4),
                                 S1Interval(M_PI_4, 3*M_PI_4));
    case 2:  return S2LatLngRect(R1Interval(kPoleMinLat, M_PI_2),
                                 S1Interval(-M_PI, M_PI));
    case 3:  return S2LatLngRect(R1Interval(-M_PI_4, M_PI_4),
                                 S1Interval(3*M_PI_4, -3*M_PI_4));
    case 4:  return S2LatLngRect(R1Interval(-M_PI_4, M_PI_4),
                                 S1Interval(-3*M_PI_4, -M_PI_4));
    default: return S2LatLngRect(R1Interval(-M_PI_2, -kPoleMinLat),
                                 S1Interval(-M_PI, M_PI));
  }
}

bool S2Cell::MayIntersect(S2Cell const& cell) const {
  return id_.intersects(cell.id_);
}

bool S2Cell::Contains(S2Cell const& cell) const {
  return id_.contains(cell.id_);
}

bool S2Cell::Contains(S2Point const& p) const {
  // We can't just call XYZtoFaceUV, because for points that lie on the
  // boundary between two faces (i.e. u or v is +1/-1) we need to return
  // true for both adjacent cells.
  double u, v;
  if (!S2::FaceXYZtoUV(face_, p, &u, &v)) return false;
  return (u >= uv_[0][0] && u <= uv_[0][1] &&
          v >= uv_[1][0] && v <= uv_[1][1]);
}
