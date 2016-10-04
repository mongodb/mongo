// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S2CELL_H_
#define UTIL_GEOMETRY_S2CELL_H_

#include "base/basictypes.h"
#include "base/logging.h"
#include "s2.h"
#include "s2cellid.h"
#include "s2region.h"
#include "util/math/vector2.h"

// An S2Cell is an S2Region object that represents a cell.  Unlike S2CellIds,
// it supports efficient containment and intersection tests.  However, it is
// also a more expensive representation (currently 48 bytes rather than 8).

// This class is intended to be copied by value as desired.  It uses
// the default copy constructor and assignment operator, however it is
// not a "plain old datatype" (POD) because it has virtual functions.
class S2Cell : public S2Region {
 public:
  // The default constructor is required in order to use freelists.
  // Cells should otherwise always be constructed explicitly.
  S2Cell() {}

  // An S2Cell always corresponds to a particular S2CellId.  The other
  // constructors are just convenience methods.
  explicit S2Cell(S2CellId const& id) { Init(id); }

  static S2Cell FromFacePosLevel(int face, uint64 pos, int level) {
    // This is a static method in order to provide named parameters.
    return S2Cell(S2CellId::FromFacePosLevel(face, pos, level));
  }
  // Convenience methods.  The S2LatLng must be normalized.
  explicit S2Cell(S2Point const& p) { Init(S2CellId::FromPoint(p)); }
  explicit S2Cell(S2LatLng const& ll) { Init(S2CellId::FromLatLng(ll)); }

  inline S2CellId id() const { return id_; }
  inline int face() const { return face_; }
  inline int level() const { return level_; }
  inline int orientation() const { return orientation_; }
  inline bool is_leaf() const { return level_ == S2CellId::kMaxLevel; }

  // These are equivalent to the S2CellId methods, but have a more efficient
  // implementation since the level has been precomputed.
  int GetSizeIJ() const;
  double GetSizeST() const;

  // Return the k-th vertex of the cell (k = 0,1,2,3).  Vertices are returned
  // in CCW order.  The points returned by GetVertexRaw are not necessarily
  // unit length.
  S2Point GetVertex(int k) const { return GetVertexRaw(k).Normalize(); }
  S2Point GetVertexRaw(int k) const;

  // Return the inward-facing normal of the great circle passing through
  // the edge from vertex k to vertex k+1 (mod 4).  The normals returned
  // by GetEdgeRaw are not necessarily unit length.
  S2Point GetEdge(int k) const { return GetEdgeRaw(k).Normalize(); }
  S2Point GetEdgeRaw(int k) const;

  // If this is not a leaf cell, set children[0..3] to the four children of
  // this cell (in traversal order) and return true.  Otherwise returns false.
  // This method is equivalent to the following:
  //
  // for (pos=0, id=child_begin(); id != child_end(); id = id.next(), ++pos)
  //   children[i] = S2Cell(id);
  //
  // except that it is more than two times faster.
  bool Subdivide(S2Cell children[4]) const;

  // Return the direction vector corresponding to the center in (s,t)-space of
  // the given cell.  This is the point at which the cell is divided into four
  // subcells; it is not necessarily the centroid of the cell in (u,v)-space
  // or (x,y,z)-space.  The point returned by GetCenterRaw is not necessarily
  // unit length.
  S2Point GetCenter() const { return GetCenterRaw().Normalize(); }
  S2Point GetCenterRaw() const;

  // Return the average area for cells at the given level.
  static double AverageArea(int level);

  // Return the average area of cells at this level.  This is accurate to
  // within a factor of 1.7 (for S2_QUADRATIC_PROJECTION) and is extremely
  // cheap to compute.
  double AverageArea() const { return AverageArea(level_); }

  // Return the approximate area of this cell.  This method is accurate to
  // within 3% percent for all cell sizes and accurate to within 0.1% for
  // cells at level 5 or higher (i.e. squares 350km to a side or smaller
  // on the Earth's surface).  It is moderately cheap to compute.
  double ApproxArea() const;

  // Return the area of this cell as accurately as possible.  This method is
  // more expensive but it is accurate to 6 digits of precision even for leaf
  // cells (whose area is approximately 1e-18).
  double ExactArea() const;

  ////////////////////////////////////////////////////////////////////////
  // S2Region interface (see s2region.h for details):

  virtual S2Cell* Clone() const;
  virtual S2Cap GetCapBound() const;
  virtual S2LatLngRect GetRectBound() const;
  virtual bool Contains(S2Cell const& cell) const;
  virtual bool MayIntersect(S2Cell const& cell) const;
  virtual bool VirtualContainsPoint(S2Point const& p) const {
    return Contains(p);  // The same as Contains() below, just virtual.
  }

  // The point 'p' does not need to be normalized.
  bool Contains(S2Point const& p) const;

  virtual void Encode(Encoder* const encoder) const {
    S2LOG(FATAL) << "Unimplemented";
  }
  virtual bool Decode(Decoder* const decoder) { return false; }

 private:
  // Internal method that does the actual work in the constructors.
  void Init(S2CellId const& id);

  // Return the latitude or longitude of the cell vertex given by (i,j),
  // where "i" and "j" are either 0 or 1.
  inline double GetLatitude(int i, int j) const;
  inline double GetLongitude(int i, int j) const;

  // This structure occupies 44 bytes plus one pointer for the vtable.
  int8 face_;
  int8 level_;
  int8 orientation_;
  S2CellId id_;
  double uv_[2][2];
};

inline int S2Cell::GetSizeIJ() const {
  return S2CellId::GetSizeIJ(level());
}

inline double S2Cell::GetSizeST() const {
  return S2CellId::GetSizeST(level());
}

#endif  // UTIL_GEOMETRY_S2CELL_H_
