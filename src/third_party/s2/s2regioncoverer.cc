// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2regioncoverer.h"

#ifndef OS_WINDOWS
#include <pthread.h>
#endif

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <functional>
using std::less;

#include <queue>
using std::priority_queue;

#include <vector>
using std::vector;


#include "base/logging.h"
#include "s2.h"
#include "s2cap.h"
#include "s2cellunion.h"
#include "mongo/base/init.h"

// Define storage for header file constants (the values are not needed here).
int const S2RegionCoverer::kDefaultMaxCells = 8;

// We define our own own comparison function on QueueEntries in order to
// make the results deterministic.  Using the default less<QueueEntry>,
// entries of equal priority would be sorted according to the memory address
// of the candidate.

struct S2RegionCoverer::CompareQueueEntries : public less<QueueEntry> {
  bool operator()(QueueEntry const& x, QueueEntry const& y) {
    return x.first < y.first;
  }
};

static S2Cell face_cells[6];

static void Init() {
  for (int face = 0; face < 6; ++face) {
    face_cells[face] = S2Cell::FromFacePosLevel(face, 0, 0);
  }
}

MONGO_INITIALIZER_WITH_PREREQUISITES(S2RegionCovererInit, ("S2CellIdInit"))(mongo::InitializerContext *context) {
    Init();
    return mongo::Status::OK();
}

S2RegionCoverer::S2RegionCoverer() :
  min_level_(0),
  max_level_(S2CellId::kMaxLevel),
  level_mod_(1),
  max_cells_(kDefaultMaxCells),
  region_(NULL),
  result_(new vector<S2CellId>),
  pq_(new CandidateQueue) {
}

S2RegionCoverer::~S2RegionCoverer() {
  // Need to declare explicitly because of scoped pointers.
}

void S2RegionCoverer::set_min_level(int min_level) {
  DCHECK_GE(min_level, 0);
  DCHECK_LE(min_level, S2CellId::kMaxLevel);
  min_level_ = max(0, min(S2CellId::kMaxLevel, min_level));
}

void S2RegionCoverer::set_max_level(int max_level) {
  DCHECK_GE(max_level, 0);
  DCHECK_LE(max_level, S2CellId::kMaxLevel);
  max_level_ = max(0, min(S2CellId::kMaxLevel, max_level));
}

void S2RegionCoverer::set_level_mod(int level_mod) {
  DCHECK_GE(level_mod, 1);
  DCHECK_LE(level_mod, 3);
  level_mod_ = max(1, min(3, level_mod));
}

void S2RegionCoverer::set_max_cells(int max_cells) {
  max_cells_ = max_cells;
}

S2RegionCoverer::Candidate* S2RegionCoverer::NewCandidate(S2Cell const& cell) {
  if (!region_->MayIntersect(cell)) return NULL;

  bool is_terminal = false;
  size_t size = sizeof(Candidate);
  if (cell.level() >= min_level_) {
    if (interior_covering_) {
      if (region_->Contains(cell)) {
        is_terminal = true;
      } else if (cell.level() + level_mod_ > max_level_) {
        return NULL;
      }
    } else {
      if (cell.level() + level_mod_ > max_level_ || region_->Contains(cell)) {
        is_terminal = true;
      }
    }
  }
  if (!is_terminal) {
    size += sizeof(Candidate*) << max_children_shift();
  }
  Candidate* candidate = static_cast<Candidate*>(malloc(size));
  memset(candidate, 0, size);
  candidate->cell = cell;
  candidate->is_terminal = is_terminal;
  ++candidates_created_counter_;
  return candidate;
}

void S2RegionCoverer::DeleteCandidate(Candidate* candidate,
                                      bool delete_children) {
  if (delete_children) {
    for (int i = 0; i < candidate->num_children; ++i)
      DeleteCandidate(candidate->children[i], true);
  }
  free(candidate);
}

int S2RegionCoverer::ExpandChildren(Candidate* candidate,
                                    S2Cell const& cell, int num_levels) {
  num_levels--;
  S2Cell child_cells[4];
  cell.Subdivide(child_cells);
  int num_terminals = 0;
  for (int i = 0; i < 4; ++i) {
    if (num_levels > 0) {
      if (region_->MayIntersect(child_cells[i])) {
        num_terminals += ExpandChildren(candidate, child_cells[i], num_levels);
      }
      continue;
    }
    Candidate* child = NewCandidate(child_cells[i]);
    if (child) {
      candidate->children[candidate->num_children++] = child;
      if (child->is_terminal) ++num_terminals;
    }
  }
  return num_terminals;
}

void S2RegionCoverer::AddCandidate(Candidate* candidate) {
  if (candidate == NULL) return;

  if (candidate->is_terminal) {
    result_->push_back(candidate->cell.id());
    DeleteCandidate(candidate, true);
    return;
  }
  DCHECK_EQ(0, candidate->num_children);

  // Expand one level at a time until we hit min_level_ to ensure that
  // we don't skip over it.
  int num_levels = (candidate->cell.level() < min_level_) ? 1 : level_mod_;
  int num_terminals = ExpandChildren(candidate, candidate->cell, num_levels);

  if (candidate->num_children == 0) {
    DeleteCandidate(candidate, false);

  } else if (!interior_covering_ &&
             num_terminals == 1 << max_children_shift() &&
             candidate->cell.level() >= min_level_) {
    // Optimization: add the parent cell rather than all of its children.
    // We can't do this for interior coverings, since the children just
    // intersect the region, but may not be contained by it - we need to
    // subdivide them further.
    candidate->is_terminal = true;
    AddCandidate(candidate);

  } else {
    // We negate the priority so that smaller absolute priorities are returned
    // first.  The heuristic is designed to refine the largest cells first,
    // since those are where we have the largest potential gain.  Among cells
    // at the same level, we prefer the cells with the smallest number of
    // intersecting children.  Finally, we prefer cells that have the smallest
    // number of children that cannot be refined any further.
    int priority = -((((candidate->cell.level() << max_children_shift())
                       + candidate->num_children) << max_children_shift())
                     + num_terminals);
    pq_->push(make_pair(priority, candidate));
    VLOG(2) << "Push: " << candidate->cell.id() << " (" << priority << ") ";
  }
}

void S2RegionCoverer::GetInitialCandidates() {
  // Optimization: if at least 4 cells are desired (the normal case),
  // start with a 4-cell covering of the region's bounding cap.  This
  // lets us skip quite a few levels of refinement when the region to
  // be covered is relatively small.
  if (max_cells() >= 4) {
    // Find the maximum level such that the bounding cap contains at most one
    // cell vertex at that level.
    S2Cap cap = region_->GetCapBound();
    int level = min(S2::kMinWidth.GetMaxLevel(2 * cap.angle().radians()),
                    min(max_level(), S2CellId::kMaxLevel - 1));
    if (level_mod() > 1 && level > min_level()) {
      level -= (level - min_level()) % level_mod();
    }
    // We don't bother trying to optimize the level == 0 case, since more than
    // four face cells may be required.
    if (level > 0) {
      // Find the leaf cell containing the cap axis, and determine which
      // subcell of the parent cell contains it.
      vector<S2CellId> base;
      base.reserve(4);
      S2CellId id = S2CellId::FromPoint(cap.axis());
      id.AppendVertexNeighbors(level, &base);
      for (size_t i = 0; i < base.size(); ++i) {
        AddCandidate(NewCandidate(S2Cell(base[i])));
      }
      return;
    }
  }
  // Default: start with all six cube faces.
  for (size_t face = 0; face < 6; ++face) {
    AddCandidate(NewCandidate(face_cells[face]));
  }
}

void S2RegionCoverer::GetCoveringInternal(S2Region const& region) {
  // Strategy: Start with the 6 faces of the cube.  Discard any
  // that do not intersect the shape.  Then repeatedly choose the
  // largest cell that intersects the shape and subdivide it.
  //
  // result_ contains the cells that will be part of the output, while pq_
  // contains cells that we may still subdivide further.  Cells that are
  // entirely contained within the region are immediately added to the output,
  // while cells that do not intersect the region are immediately discarded.
  // Therefore pq_ only contains cells that partially intersect the region.
  // Candidates are prioritized first according to cell size (larger cells
  // first), then by the number of intersecting children they have (fewest
  // children first), and then by the number of fully contained children
  // (fewest children first).

  DCHECK(pq_->empty());
  DCHECK(result_->empty());
  region_ = &region;
  candidates_created_counter_ = 0;

  GetInitialCandidates();
  while (!pq_->empty() &&
         (!interior_covering_ || result_->size() < (size_t)max_cells_)) {
    Candidate* candidate = pq_->top().second;
    pq_->pop();
    VLOG(2) << "Pop: " << candidate->cell.id();
    if (candidate->cell.level() < min_level_ ||
        candidate->num_children == 1 ||
        (int)result_->size() + (int)(interior_covering_ ? 0 : (int)pq_->size()) +
            candidate->num_children <= max_cells_) {
      // Expand this candidate into its children.
      for (int i = 0; i < candidate->num_children; ++i) {
        AddCandidate(candidate->children[i]);
      }
      DeleteCandidate(candidate, false);
    } else if (interior_covering_) {
      DeleteCandidate(candidate, true);
    } else {
      candidate->is_terminal = true;
      AddCandidate(candidate);
    }
  }
  VLOG(2) << "Created " << result_->size() << " cells, " <<
      candidates_created_counter_ << " candidates created, " <<
      pq_->size() << " left";
  while (!pq_->empty()) {
    DeleteCandidate(pq_->top().second, true);
    pq_->pop();
  }
  region_ = NULL;
}

void S2RegionCoverer::GetCovering(S2Region const& region,
                                  vector<S2CellId>* covering) {

  // Rather than just returning the raw list of cell ids generated by
  // GetCoveringInternal(), we construct a cell union and then denormalize it.
  // This has the effect of replacing four child cells with their parent
  // whenever this does not violate the covering parameters specified
  // (min_level, level_mod, etc).  This strategy significantly reduces the
  // number of cells returned in many cases, and it is cheap compared to
  // computing the covering in the first place.

  S2CellUnion tmp;
  GetCellUnion(region, &tmp);
  tmp.Denormalize(min_level(), level_mod(), covering);
}

void S2RegionCoverer::GetInteriorCovering(S2Region const& region,
                                          vector<S2CellId>* interior) {
  S2CellUnion tmp;
  GetInteriorCellUnion(region, &tmp);
  tmp.Denormalize(min_level(), level_mod(), interior);
}

void S2RegionCoverer::GetCellUnion(S2Region const& region,
                                   S2CellUnion* covering) {
  interior_covering_ = false;
  GetCoveringInternal(region);
  covering->InitSwap(result_.get());
}

void S2RegionCoverer::GetInteriorCellUnion(S2Region const& region,
                                           S2CellUnion* interior) {
  interior_covering_ = true;
  GetCoveringInternal(region);
  interior->InitSwap(result_.get());
}

void S2RegionCoverer::FloodFill(
    S2Region const& region, S2CellId const& start, vector<S2CellId>* output) {
  hash_set<S2CellId> all;
  vector<S2CellId> frontier;
  output->clear();
  all.insert(start);
  frontier.push_back(start);
  while (!frontier.empty()) {
    S2CellId id = frontier.back();
    frontier.pop_back();
    if (!region.MayIntersect(S2Cell(id))) continue;
    output->push_back(id);

    S2CellId neighbors[4];
    id.GetEdgeNeighbors(neighbors);
    for (int edge = 0; edge < 4; ++edge) {
      S2CellId nbr = neighbors[edge];
      if (all.insert(nbr).second) {
        frontier.push_back(nbr);
      }
    }
  }
}

void S2RegionCoverer::GetSimpleCovering(
    S2Region const& region, S2Point const& start,
    int level, vector<S2CellId>* output) {
  return FloodFill(region, S2CellId::FromPoint(start).parent(level), output);
}
