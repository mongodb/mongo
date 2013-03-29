// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S2REGIONUNION_H__
#define UTIL_GEOMETRY_S2REGIONUNION_H__

#include <vector>
using std::vector;

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/macros.h"
#include "s2region.h"

class S2Cap;
class S2Cell;
class S2LatLngRect;

// An S2RegionUnion represents a union of possibly overlapping regions.
// It is convenient for computing a covering of a set of regions.
class S2RegionUnion : public S2Region {
 public:
  // Create an empty region.  Can be made non-empty by calling Init() or Add().
  S2RegionUnion();

  // Create a region representing the union of the given regions.
  // DOES NOT take ownership of all regions.
  // clears the given vector.
  S2RegionUnion(vector<S2Region*>* regions);

  virtual ~S2RegionUnion();

  // Initialize region by taking ownership of the given regions.
  void Init(vector<S2Region*>* regions);

  // Release ownership of the regions of this union, and appends them to
  // "regions" if non-NULL.  Resets the region to be empty.
  void Release(vector<S2Region*>* regions);

  // Add the given region to the union.  This method can be called repeatedly
  // as an alternative to Init().
  // Takes ownership of the pointer.
  void Add(S2Region* region);

  // Accessor methods.
  int num_regions() const { return regions_.size(); }
  inline S2Region* region(int i) const { return regions_[i]; }

  ////////////////////////////////////////////////////////////////////////
  // S2Region interface (see s2region.h for details):

  virtual S2RegionUnion* Clone() const;
  virtual S2Cap GetCapBound() const;
  virtual S2LatLngRect GetRectBound() const;
  virtual bool VirtualContainsPoint(S2Point const& p) const;
  bool Contains(S2Point const& p) const;
  virtual bool Contains(S2Cell const& cell) const;
  virtual bool MayIntersect(S2Cell const& cell) const;
  virtual void Encode(Encoder* const encoder) const {
    S2LOG(FATAL) << "Unimplemented";
  }
  virtual bool Decode(Decoder* const decoder) { return false; }

 private:
  // Internal constructor used only by Clone() that makes a deep copy of
  // its argument.
  S2RegionUnion(S2RegionUnion const* src);

  vector<S2Region*> regions_;

  DISALLOW_EVIL_CONSTRUCTORS(S2RegionUnion);
};

#endif  // UTIL_GEOMETRY_S2REGIONUNION_H__
