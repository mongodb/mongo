// Copyright 2006 Google Inc. All Rights Reserved.

#include "s2regionintersection.h"

#include "s2cap.h"
#include "s2cell.h"
#include "s2latlngrect.h"

S2RegionIntersection::S2RegionIntersection() { }

S2RegionIntersection::S2RegionIntersection(vector<S2Region*>* regions) {
  Init(regions);
}

S2RegionIntersection::~S2RegionIntersection() {
  for (size_t i = 0; i < regions_.size(); ++i) {
    delete regions_[i];
  }
  regions_.clear();
}

void S2RegionIntersection::Init(vector<S2Region*>* regions) {
  DCHECK(regions_.empty());
  // We copy the vector rather than calling swap() to optimize storage.
  regions_ = *regions;
  regions->clear();
}

S2RegionIntersection::S2RegionIntersection(S2RegionIntersection const* src)
  : regions_(src->num_regions()) {
  for (int i = 0; i < num_regions(); ++i) {
    regions_[i] = src->region(i)->Clone();
  }
}

void S2RegionIntersection::Release(vector<S2Region*>* regions) {
  if (regions != NULL) {
    regions->insert(regions->end(), regions_.begin(), regions_.end());
  }
  regions_.clear();
}

S2RegionIntersection* S2RegionIntersection::Clone() const {
  return new S2RegionIntersection(this);
}

S2Cap S2RegionIntersection::GetCapBound() const {
  // TODO: This could be optimized to return a tighter bound, but doesn't
  // seem worth it unless profiling shows otherwise.
  return GetRectBound().GetCapBound();
}

S2LatLngRect S2RegionIntersection::GetRectBound() const {
  S2LatLngRect result = S2LatLngRect::Full();
  for (int i = 0; i < num_regions(); ++i) {
    result = result.Intersection(region(i)->GetRectBound());
  }
  return result;
}

bool S2RegionIntersection::VirtualContainsPoint(S2Point const& p) const {
  return Contains(p);  // The same as Contains(), just virtual.
}

bool S2RegionIntersection::Contains(S2Cell const& cell) const {
  for (int i = 0; i < num_regions(); ++i) {
    if (!region(i)->Contains(cell)) return false;
  }
  return true;
}

bool S2RegionIntersection::Contains(S2Point const& p) const {
  for (int i = 0; i < num_regions(); ++i) {
    if (!region(i)->VirtualContainsPoint(p)) return false;
  }
  return true;
}

bool S2RegionIntersection::MayIntersect(S2Cell const& cell) const {
  for (int i = 0; i < num_regions(); ++i) {
    if (!region(i)->MayIntersect(cell)) return false;
  }
  return true;
}
