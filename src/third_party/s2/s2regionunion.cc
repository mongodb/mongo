// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2regionunion.h"

#include "s2cap.h"
#include "s2cell.h"
#include "s2latlngrect.h"

S2RegionUnion::S2RegionUnion() { }

S2RegionUnion::S2RegionUnion(vector<S2Region*>* regions) {
  Init(regions);
}

S2RegionUnion::~S2RegionUnion() {
    /*
  for (size_t i = 0; i < regions_.size(); ++i) {
    delete regions_[i];
  }
  regions_.clear();
  */
}

void S2RegionUnion::Init(vector<S2Region*>* regions) {
  DCHECK(regions_.empty());
  // We copy the vector rather than calling swap() to optimize storage.
  regions_ = *regions;
  regions->clear();
}

S2RegionUnion::S2RegionUnion(S2RegionUnion const* src)
  : regions_(src->num_regions()) {
  for (int i = 0; i < num_regions(); ++i) {
    regions_[i] = src->region(i)->Clone();
  }
}

void S2RegionUnion::Release(vector<S2Region*>* regions) {
  if (regions != NULL) {
    regions->insert(regions->end(), regions_.begin(), regions_.end());
  }
  regions_.clear();
}

void S2RegionUnion::Add(S2Region* region) {
  regions_.push_back(region);
}

S2RegionUnion* S2RegionUnion::Clone() const {
  return new S2RegionUnion(this);
}

S2Cap S2RegionUnion::GetCapBound() const {
  // TODO: This could be optimized to return a tighter bound, but doesn't
  // seem worth it unless profiling shows otherwise.
  return GetRectBound().GetCapBound();
}

S2LatLngRect S2RegionUnion::GetRectBound() const {
  S2LatLngRect result = S2LatLngRect::Empty();
  for (int i = 0; i < num_regions(); ++i) {
    result = result.Union(region(i)->GetRectBound());
  }
  return result;
}

bool S2RegionUnion::VirtualContainsPoint(S2Point const& p) const {
  return Contains(p);  // The same as Contains(), just virtual.
}

bool S2RegionUnion::Contains(S2Cell const& cell) const {
  // Note that this method is allowed to return false even if the cell
  // is contained by the region.
  for (int i = 0; i < num_regions(); ++i) {
    if (region(i)->Contains(cell)) return true;
  }
  return false;
}

bool S2RegionUnion::Contains(S2Point const& p) const {
  for (int i = 0; i < num_regions(); ++i) {
    if (region(i)->VirtualContainsPoint(p)) return true;
  }
  return false;
}

bool S2RegionUnion::MayIntersect(S2Cell const& cell) const {
  for (int i = 0; i < num_regions(); ++i) {
    if (region(i)->MayIntersect(cell)) return true;
  }
  return false;
}
