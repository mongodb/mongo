// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2cellunion.h"

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <vector>
using std::vector;


#include "base/integral_types.h"
#include "base/logging.h"
#include "s2.h"
#include "s2cap.h"
#include "s2cell.h"
#include "s2cellid.h"
#include "s2latlngrect.h"

// Returns true if the vector of cell_ids is sorted.  Used only in
// DCHECKs.
extern bool IsSorted(vector<S2CellId> const& cell_ids) {
  for (size_t i = 0; i + 1 < cell_ids.size(); ++i) {
    if (cell_ids[i + 1] < cell_ids[i]) {
      return false;
    }
  }
  return true;
}

void S2CellUnion::Init(vector<S2CellId> const& cell_ids) {
  InitRaw(cell_ids);
  Normalize();
}

void S2CellUnion::Init(vector<uint64> const& cell_ids) {
  InitRaw(cell_ids);
  Normalize();
}

void S2CellUnion::InitSwap(vector<S2CellId>* cell_ids) {
  InitRawSwap(cell_ids);
  Normalize();
}

void S2CellUnion::InitRaw(vector<S2CellId> const& cell_ids) {
  cell_ids_ = cell_ids;
}

void S2CellUnion::InitRaw(vector<uint64> const& cell_ids) {
  cell_ids_.resize(cell_ids.size());
  for (int i = 0; i < num_cells(); ++i) {
    cell_ids_[i] = S2CellId(cell_ids[i]);
  }
}

void S2CellUnion::InitRawSwap(vector<S2CellId>* cell_ids) {
  cell_ids_.swap(*cell_ids);
  cell_ids->clear();
}

void S2CellUnion::Add(const vector<S2CellId>& cell_ids) {
    cell_ids_.insert(cell_ids_.end(), cell_ids.begin(), cell_ids.end());
    Normalize();
}

void S2CellUnion::Detach(vector<S2CellId>* cell_ids) {
  cell_ids_.swap(*cell_ids);
  cell_ids_.clear();
}

void S2CellUnion::Pack(int excess) {
  if (cell_ids_.capacity() - cell_ids_.size() > (size_t)excess) {
    vector<S2CellId> packed = cell_ids_;
    cell_ids_.swap(packed);
  }
}

S2CellUnion* S2CellUnion::Clone() const {
  S2CellUnion* copy = new S2CellUnion;
  copy->InitRaw(cell_ids_);
  return copy;
}

bool S2CellUnion::Normalize() {
  // Optimize the representation by looking for cases where all subcells
  // of a parent cell are present.

  vector<S2CellId> output;
  output.reserve(cell_ids_.size());
  sort(cell_ids_.begin(), cell_ids_.end());

  for (int i = 0; i < num_cells(); ++i) {
    S2CellId id = cell_id(i);

    // Check whether this cell is contained by the previous cell.
    if (!output.empty() && output.back().contains(id)) continue;

    // Discard any previous cells contained by this cell.
    while (!output.empty() && id.contains(output.back())) {
      output.pop_back();
    }

    // Check whether the last 3 elements of "output" plus "id" can be
    // collapsed into a single parent cell.
    while (output.size() >= 3) {
      // A necessary (but not sufficient) condition is that the XOR of the
      // four cells must be zero.  This is also very fast to test.
      if ((output.end()[-3].id() ^ output.end()[-2].id() ^ output.back().id())
          != id.id())
        break;

      // Now we do a slightly more expensive but exact test.  First, compute a
      // mask that blocks out the two bits that encode the child position of
      // "id" with respect to its parent, then check that the other three
      // children all agree with "mask.
      uint64 mask = id.lsb() << 1;
      mask = ~(mask + (mask << 1));
      uint64 id_masked = (id.id() & mask);
      if ((output.end()[-3].id() & mask) != id_masked ||
          (output.end()[-2].id() & mask) != id_masked ||
          (output.end()[-1].id() & mask) != id_masked ||
          id.is_face())
        break;

      // Replace four children by their parent cell.
      output.erase(output.end() - 3, output.end());
      id = id.parent();
    }
    output.push_back(id);
  }
  if (output.size() < (size_t)num_cells()) {
    InitRawSwap(&output);
    return true;
  }
  return false;
}

void S2CellUnion::Denormalize(int min_level, int level_mod,
                              vector<S2CellId>* output) const {
  DCHECK_GE(min_level, 0);
  DCHECK_LE(min_level, S2CellId::kMaxLevel);
  DCHECK_GE(level_mod, 1);
  DCHECK_LE(level_mod, 3);

  output->clear();
  output->reserve(num_cells());
  for (int i = 0; i < num_cells(); ++i) {
    S2CellId id = cell_id(i);
    int level = id.level();
    int new_level = max(min_level, level);
    if (level_mod > 1) {
      // Round up so that (new_level - min_level) is a multiple of level_mod.
      // (Note that S2CellId::kMaxLevel is a multiple of 1, 2, and 3.)
      new_level += (S2CellId::kMaxLevel - (new_level - min_level)) % level_mod;
      new_level = min(S2CellId::kMaxLevel, new_level);
    }
    if (new_level == level) {
      output->push_back(id);
    } else {
      S2CellId end = id.child_end(new_level);
      for (id = id.child_begin(new_level); id != end; id = id.next()) {
        output->push_back(id);
      }
    }
  }
}

S2Cap S2CellUnion::GetCapBound() const {
  // Compute the approximate centroid of the region.  This won't produce the
  // bounding cap of minimal area, but it should be close enough.
  if (cell_ids_.empty()) return S2Cap::Empty();
  S2Point centroid(0, 0, 0);
  for (int i = 0; i < num_cells(); ++i) {
    double area = S2Cell::AverageArea(cell_id(i).level());
    centroid += area * cell_id(i).ToPoint();
  }
  if (centroid == S2Point(0, 0, 0)) {
    centroid = S2Point(1, 0, 0);
  } else {
    centroid = centroid.Normalize();
  }

  // Use the centroid as the cap axis, and expand the cap angle so that it
  // contains the bounding caps of all the individual cells.  Note that it is
  // *not* sufficient to just bound all the cell vertices because the bounding
  // cap may be concave (i.e. cover more than one hemisphere).
  S2Cap cap = S2Cap::FromAxisHeight(centroid, 0);
  for (int i = 0; i < num_cells(); ++i) {
    cap.AddCap(S2Cell(cell_id(i)).GetCapBound());
  }
  return cap;
}

S2LatLngRect S2CellUnion::GetRectBound() const {
  S2LatLngRect bound = S2LatLngRect::Empty();
  for (int i = 0; i < num_cells(); ++i) {
    bound = bound.Union(S2Cell(cell_id(i)).GetRectBound());
  }
  return bound;
}

bool S2CellUnion::Contains(S2CellId const& id) const {
  // This function requires that Normalize has been called first.
  //
  // This is an exact test.  Each cell occupies a linear span of the S2
  // space-filling curve, and the cell id is simply the position at the center
  // of this span.  The cell union ids are sorted in increasing order along
  // the space-filling curve.  So we simply find the pair of cell ids that
  // surround the given cell id (using binary search).  There is containment
  // if and only if one of these two cell ids contains this cell.

  vector<S2CellId>::const_iterator i =
    lower_bound(cell_ids_.begin(), cell_ids_.end(), id);
  if (i != cell_ids_.end() && i->range_min() <= id) return true;
  return i != cell_ids_.begin() && (--i)->range_max() >= id;
}

bool S2CellUnion::Intersects(S2CellId const& id) const {
  // This function requires that Normalize has been called first.
  // This is an exact test; see the comments for Contains() above.

  vector<S2CellId>::const_iterator i =
    lower_bound(cell_ids_.begin(), cell_ids_.end(), id);
  if (i != cell_ids_.end() && i->range_min() <= id.range_max()) return true;
  return i != cell_ids_.begin() && (--i)->range_max() >= id.range_min();
}

bool S2CellUnion::Contains(S2CellUnion const* y) const {
  // TODO: A divide-and-conquer or alternating-skip-search approach may be
  // sigificantly faster in both the average and worst case.

  for (int i = 0; i < y->num_cells(); ++i) {
    if (!Contains(y->cell_id(i))) return false;
  }
  return true;
}

bool S2CellUnion::Intersects(S2CellUnion const* y) const {
  // TODO: A divide-and-conquer or alternating-skip-search approach may be
  // sigificantly faster in both the average and worst case.

  for (int i = 0; i < y->num_cells(); ++i) {
    if (Intersects(y->cell_id(i))) return true;
  }
  return false;
}

void S2CellUnion::GetUnion(S2CellUnion const* x, S2CellUnion const* y) {
  DCHECK_NE(this, x);
  DCHECK_NE(this, y);
  cell_ids_.reserve(x->num_cells() + y->num_cells());
  cell_ids_ = x->cell_ids_;
  cell_ids_.insert(cell_ids_.end(), y->cell_ids_.begin(), y->cell_ids_.end());
  Normalize();
}

void S2CellUnion::GetIntersection(S2CellUnion const* x, S2CellId const& id) {
  DCHECK_NE(this, x);
  cell_ids_.clear();
  if (x->Contains(id)) {
    cell_ids_.push_back(id);
  } else {
    vector<S2CellId>::const_iterator i =
      lower_bound(x->cell_ids_.begin(), x->cell_ids_.end(), id.range_min());
    S2CellId idmax = id.range_max();
    while (i != x->cell_ids_.end() && *i <= idmax)
      cell_ids_.push_back(*i++);
  }
}

void S2CellUnion::GetIntersection(S2CellUnion const* x, S2CellUnion const* y) {
  DCHECK_NE(this, x);
  DCHECK_NE(this, y);

  // This is a fairly efficient calculation that uses binary search to skip
  // over sections of both input vectors.  It takes constant time if all the
  // cells of "x" come before or after all the cells of "y" in S2CellId order.

  cell_ids_.clear();
  vector<S2CellId>::const_iterator i = x->cell_ids_.begin();
  vector<S2CellId>::const_iterator j = y->cell_ids_.begin();
  while (i != x->cell_ids_.end() && j != y->cell_ids_.end()) {
    S2CellId imin = i->range_min();
    S2CellId jmin = j->range_min();
    if (imin > jmin) {
      // Either j->contains(*i) or the two cells are disjoint.
      if (*i <= j->range_max()) {
        cell_ids_.push_back(*i++);
      } else {
        // Advance "j" to the first cell possibly contained by *i.
        j = lower_bound(j + 1, y->cell_ids_.end(), imin);
        // The previous cell *(j-1) may now contain *i.
        if (*i <= (j - 1)->range_max()) --j;
      }
    } else if (jmin > imin) {
      // Identical to the code above with "i" and "j" reversed.
      if (*j <= i->range_max()) {
        cell_ids_.push_back(*j++);
      } else {
        i = lower_bound(i + 1, x->cell_ids_.end(), jmin);
        if (*j <= (i - 1)->range_max()) --i;
      }
    } else {
      // "i" and "j" have the same range_min(), so one contains the other.
      if (*i < *j)
        cell_ids_.push_back(*i++);
      else
        cell_ids_.push_back(*j++);
    }
  }
  // The output is generated in sorted order, and there should not be any
  // cells that can be merged (provided that both inputs were normalized).
  DCHECK(IsSorted(cell_ids_));
  DCHECK(!Normalize());
}

static void GetDifferenceInternal(S2CellId cell,
                                  S2CellUnion const* y,
                                  vector<S2CellId>* cell_ids) {
  // Add the difference between cell and y to cell_ids.
  // If they intersect but the difference is non-empty, divides and conquers.

  if (!y->Intersects(cell)) {
    cell_ids->push_back(cell);
  } else if (!y->Contains(cell)) {
    S2CellId child = cell.child_begin();
    for (int i = 0; ; ++i) {
      GetDifferenceInternal(child, y, cell_ids);
      if (i == 3) break;  // Avoid unnecessary next() computation.
      child = child.next();
    }
  }
}

void S2CellUnion::GetDifference(S2CellUnion const* x, S2CellUnion const* y) {
  DCHECK_NE(this, x);
  DCHECK_NE(this, y);
  // TODO: this is approximately O(N*log(N)), but could probably use similar
  // techniques as GetIntersection() to be more efficient.

  cell_ids_.clear();
  for (int i = 0; i < x->num_cells(); ++i) {
    GetDifferenceInternal(x->cell_id(i), y, &cell_ids_);
  }
  // The output is generated in sorted order, and there should not be any
  // cells that can be merged (provided that both inputs were normalized).
  DCHECK(IsSorted(cell_ids_));
  DCHECK(!Normalize());
}

void S2CellUnion::Expand(int level) {
  vector<S2CellId> output;
  uint64 level_lsb = S2CellId::lsb_for_level(level);
  for (int i = num_cells() - 1; i >= 0; --i) {
    S2CellId id = cell_id(i);
    if (id.lsb() < level_lsb) {
      id = id.parent(level);
      // Optimization: skip over any cells contained by this one.  This is
      // especially important when very small regions are being expanded.
      while (i > 0 && id.contains(cell_id(i-1))) --i;
    }
    output.push_back(id);
    id.AppendAllNeighbors(level, &output);
  }
  InitSwap(&output);
}

void S2CellUnion::Expand(S1Angle const& min_radius, int max_level_diff) {
  int min_level = S2CellId::kMaxLevel;
  for (int i = 0; i < num_cells(); ++i) {
    min_level = min(min_level, cell_id(i).level());
  }
  // Find the maximum level such that all cells are at least "min_radius" wide.
  int radius_level = S2::kMinWidth.GetMaxLevel(min_radius.radians());
  if (radius_level == 0 && min_radius.radians() > S2::kMinWidth.GetValue(0)) {
    // The requested expansion is greater than the width of a face cell.
    // The easiest way to handle this is to expand twice.
    Expand(0);
  }
  Expand(min(min_level + max_level_diff, radius_level));
}

void S2CellUnion::InitFromRange(S2CellId const& min_id,
                                S2CellId const& max_id) {
  DCHECK(min_id.is_leaf());
  DCHECK(max_id.is_leaf());
  DCHECK_LE(min_id, max_id);

  // We repeatedly add the largest cell we can.
  cell_ids_.clear();
  for (S2CellId next_min_id = min_id; next_min_id <= max_id; ) {
    DCHECK(next_min_id.is_leaf());

    // Find the largest cell that starts at "next_min_id" and doesn't extend
    // beyond "max_id".
    S2CellId next_id = next_min_id;
    while (!next_id.is_face() &&
           next_id.parent().range_min() == next_min_id &&
           next_id.parent().range_max() <= max_id) {
      next_id = next_id.parent();
    }
    cell_ids_.push_back(next_id);
    next_min_id = next_id.range_max().next();
  }

  // The output is already normalized.
  DCHECK(IsSorted(cell_ids_));
  DCHECK(!Normalize());
}

uint64 S2CellUnion::LeafCellsCovered() const {
  uint64 num_leaves = 0;
  for (int i = 0; i < num_cells(); ++i) {
    const int inverted_level =
        S2CellId::kMaxLevel - cell_id(i).level();
    num_leaves += (1ULL << (inverted_level << 1));
  }
  return num_leaves;
}

double S2CellUnion::AverageBasedArea() const {
  return S2Cell::AverageArea(S2CellId::kMaxLevel) * LeafCellsCovered();
}

double S2CellUnion::ApproxArea() const {
  double area = 0;
  for (int i = 0; i < num_cells(); ++i) {
    area += S2Cell(cell_id(i)).ApproxArea();
  }
  return area;
}

double S2CellUnion::ExactArea() const {
  double area = 0;
  for (int i = 0; i < num_cells(); ++i) {
    area += S2Cell(cell_id(i)).ExactArea();
  }
  return area;
}

bool operator==(S2CellUnion const& x, S2CellUnion const& y) {
  return x.cell_ids() == y.cell_ids();
}

bool S2CellUnion::Contains(S2Cell const& cell) const {
  return Contains(cell.id());
}

bool S2CellUnion::MayIntersect(S2Cell const& cell) const {
  return Intersects(cell.id());
}

bool S2CellUnion::Contains(S2Point const& p) const {
  return Contains(S2CellId::FromPoint(p));
}
