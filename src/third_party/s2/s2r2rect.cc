// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2r2rect.h"

#include "base/logging.h"
#include "r1interval.h"
#include "s2.h"
#include "s2cap.h"
#include "s2cell.h"
#include "s2latlngrect.h"

S2R2Rect S2R2Rect::FromCell(S2Cell const& cell) {
  // S2Cells have a more efficient GetSizeST() method than S2CellIds.
  double size = cell.GetSizeST();
  return FromCenterSize(cell.id().GetCenterST(), R2Point(size, size));
}

S2R2Rect S2R2Rect::FromCellId(S2CellId const& id) {
  double size = id.GetSizeST();
  return FromCenterSize(id.GetCenterST(), R2Point(size, size));
}

S2R2Rect S2R2Rect::FromCenterSize(R2Point const& center, R2Point const& size) {
  return S2R2Rect(R1Interval(center.x() - 0.5 * size.x(),
                             center.x() + 0.5 * size.x()),
                  R1Interval(center.y() - 0.5 * size.y(),
                             center.y() + 0.5 * size.y()));
}

S2R2Rect S2R2Rect::FromPoint(R2Point const& p) {
  return S2R2Rect(p, p);
}

S2R2Rect S2R2Rect::FromPointPair(R2Point const& p1, R2Point const& p2) {
  return S2R2Rect(R1Interval::FromPointPair(p1.x(), p2.x()),
                  R1Interval::FromPointPair(p1.y(), p2.y()));
}

S2R2Rect* S2R2Rect::Clone() const {
  return new S2R2Rect(*this);
}

R2Point S2R2Rect::GetVertex(int k) const {
  // Twiddle bits to return the points in CCW order (SW, SE, NE, NW).
  return R2Point(x_.bound((k>>1) ^ (k&1)), y_.bound(k>>1));
}

R2Point S2R2Rect::GetCenter() const {
  return R2Point(x_.GetCenter(), y_.GetCenter());
}

R2Point S2R2Rect::GetSize() const {
  return R2Point(x_.GetLength(), y_.GetLength());
}

bool S2R2Rect::Contains(R2Point const& p) const {
  return x_.Contains(p.x()) && y_.Contains(p.y());
}

bool S2R2Rect::InteriorContains(R2Point const& p) const {
  return x_.InteriorContains(p.x()) && y_.InteriorContains(p.y());
}

bool S2R2Rect::Contains(S2R2Rect const& other) const {
  return x_.Contains(other.x_) && y_.Contains(other.y_);
}

bool S2R2Rect::InteriorContains(S2R2Rect const& other) const {
  return x_.InteriorContains(other.x_) && y_.InteriorContains(other.y_);
}

bool S2R2Rect::Intersects(S2R2Rect const& other) const {
  return x_.Intersects(other.x_) && y_.Intersects(other.y_);
}

bool S2R2Rect::InteriorIntersects(S2R2Rect const& other) const {
  return x_.InteriorIntersects(other.x_) && y_.InteriorIntersects(other.y_);
}

void S2R2Rect::AddPoint(R2Point const& p) {
  x_.AddPoint(p.x());
  y_.AddPoint(p.y());
}

S2R2Rect S2R2Rect::Expanded(R2Point const& margin) const {
  DCHECK_GE(margin.x(), 0);
  DCHECK_GE(margin.y(), 0);
  return S2R2Rect(x_.Expanded(margin.x()), y_.Expanded(margin.y()));
}

S2R2Rect S2R2Rect::Union(S2R2Rect const& other) const {
  return S2R2Rect(x_.Union(other.x_), y_.Union(other.y_));
}

S2R2Rect S2R2Rect::Intersection(S2R2Rect const& other) const {
  R1Interval x = x_.Intersection(other.x_);
  R1Interval y = y_.Intersection(other.y_);
  if (x.is_empty() || y.is_empty()) {
    // The x/y ranges must either be both empty or both non-empty.
    return Empty();
  }
  return S2R2Rect(x, y);
}

bool S2R2Rect::ApproxEquals(S2R2Rect const& other, double max_error) const {
  return (x_.ApproxEquals(other.x_, max_error) &&
          y_.ApproxEquals(other.y_, max_error));
}

S2Point S2R2Rect::ToS2Point(R2Point const& p) {
  return S2::FaceUVtoXYZ(0, S2::STtoUV(p.x()), S2::STtoUV(p.y())).Normalize();
}

S2Cap S2R2Rect::GetCapBound() const {
  if (is_empty()) return S2Cap::Empty();

  // The rectangle is a convex polygon on the sphere, since it is a subset of
  // one cube face.  Its bounding cap is also a convex region on the sphere,
  // and therefore we can bound the rectangle by just bounding its vertices.
  // We use the rectangle's center in (s,t)-space as the cap axis.  This
  // doesn't yield the minimal cap but it's pretty close.
  S2Cap cap = S2Cap::FromAxisHeight(ToS2Point(GetCenter()), 0);
  for (int k = 0; k < 4; ++k) {
    cap.AddPoint(ToS2Point(GetVertex(k)));
  }
  return cap;
}

S2LatLngRect S2R2Rect::GetRectBound() const {
  // This is not very tight but hopefully good enough.
  return GetCapBound().GetRectBound();
}

bool S2R2Rect::Contains(S2Point const& p) const {
  S2CellId cellid = S2CellId::FromPoint(p);
  if (cellid.face() != 0) return false;
  return Contains(cellid.GetCenterST());
}

bool S2R2Rect::Contains(S2Cell const& cell) const {
  if (cell.face() != 0) return false;
  return Contains(S2R2Rect::FromCell(cell));
}

bool S2R2Rect::MayIntersect(S2Cell const& cell) const {
  if (cell.face() != 0) return false;
  return Intersects(S2R2Rect::FromCell(cell));
}

ostream& operator<<(ostream& os, S2R2Rect const& r) {
  return os << "[Lo" << r.lo() << ", Hi" << r.hi() << "]";
}
