// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S2REGION_COVERER_H_
#define UTIL_GEOMETRY_S2REGION_COVERER_H_

#include <queue>
using std::priority_queue;

#include <utility>
using std::pair;
using std::make_pair;

#include <vector>
using std::vector;

#include "base/macros.h"
#include <boost/scoped_ptr.hpp>
using boost::scoped_ptr;
//#include "base/scoped_ptr.h"
#include "s2cell.h"
#include "s2cellid.h"

class S2CellUnion;

// An S2RegionCoverer is a class that allows arbitrary regions to be
// approximated as unions of cells (S2CellUnion).  This is useful for
// implementing various sorts of search and precomputation operations.
//
// Typical usage:
//
// S2RegionCoverer coverer;
// coverer.set_max_cells(5);
// S2Cap cap = S2Cap::FromAxisAngle(...);
// vector<S2CellId> covering;
// coverer.GetCovering(cap, &covering);
//
// This yields a vector of at most 5 cells that is guaranteed to cover the
// given cap (a disc-shaped region on the sphere).
//
// The approximation algorithm is not optimal but does a pretty good job in
// practice.  The output does not always use the maximum number of cells
// allowed, both because this would not always yield a better approximation,
// and because max_cells() is a limit on how much work is done exploring the
// possible covering as well as a limit on the final output size.
//
// One can also generate interior coverings, which are sets of cells which
// are entirely contained within a region.  Interior coverings can be
// empty, even for non-empty regions, if there are no cells that satisfy
// the provided constraints and are contained by the region.  Note that for
// performance reasons, it is wise to specify a max_level when computing
// interior coverings - otherwise for regions with small or zero area, the
// algorithm may spend a lot of time subdividing cells all the way to leaf
// level to try to find contained cells.
class S2RegionCoverer {
 public:
  // By default, the covering uses at most 8 cells at any level.  This gives
  // a reasonable tradeoff between the number of cells used and the accuracy
  // of the approximation (see table below).
  static int const kDefaultMaxCells;

  S2RegionCoverer();
  ~S2RegionCoverer();

  // Set the minimum and maximum cell level to be used.  The default is to use
  // all cell levels.  Requires: max_level() >= min_level().
  //
  // To find the cell level corresponding to a given physical distance, use
  // the S2Cell metrics defined in s2.h.  For example, to find the cell
  // level that corresponds to an average edge length of 10km, use:
  //
  //     int level = S2::kAvgEdge.GetClosestLevel(
  //                 geostore::S2Earth::KmToRadians(length_km));
  //
  // Note: min_level() takes priority over max_cells(), i.e. cells below the
  // given level will never be used even if this causes a large number of
  // cells to be returned.
  void set_min_level(int min_level);
  void set_max_level(int max_level);
  int min_level() const { return min_level_; }
  int max_level() const { return max_level_; }

  // If specified, then only cells where (level - min_level) is a multiple of
  // "level_mod" will be used (default 1).  This effectively allows the
  // branching factor of the S2CellId hierarchy to be increased.  Currently
  // the only parameter values allowed are 1, 2, or 3, corresponding to
  // branching factors of 4, 16, and 64 respectively.
  void set_level_mod(int level_mod);
  int level_mod() const { return level_mod_; }

  // Sets the maximum desired number of cells in the approximation (defaults
  // to kDefaultMaxCells).  Note the following:
  //
  //  - For any setting of max_cells(), up to 6 cells may be returned if that
  //    is the minimum number of cells required (e.g. if the region intersects
  //    all six face cells).  Up to 3 cells may be returned even for very tiny
  //    convex regions if they happen to be located at the intersection of
  //    three cube faces.
  //
  //  - For any setting of max_cells(), an arbitrary number of cells may be
  //    returned if min_level() is too high for the region being approximated.
  //
  //  - If max_cells() is less than 4, the area of the covering may be
  //    arbitrarily large compared to the area of the original region even if
  //    the region is convex (e.g. an S2Cap or S2LatLngRect).
  //
  // Accuracy is measured by dividing the area of the covering by the area of
  // the original region.  The following table shows the median and worst case
  // values for this area ratio on a test case consisting of 100,000 spherical
  // caps of random size (generated using s2regioncoverer_unittest):
  //
  //   max_cells:        3      4     5     6     8    12    20   100   1000
  //   median ratio:  5.33   3.32  2.73  2.34  1.98  1.66  1.42  1.11   1.01
  //   worst case:  215518  14.41  9.72  5.26  3.91  2.75  1.92  1.20   1.02
  void set_max_cells(int max_cells);
  int max_cells() const { return max_cells_; }

  // Return a vector of cell ids that covers (GetCovering) or is contained
  // within (GetInteriorCovering) the given region and satisfies the various
  // restrictions specified above.
  void GetCovering(S2Region const& region, vector<S2CellId>* covering);
  void GetInteriorCovering(S2Region const& region, vector<S2CellId>* interior);

  // Return a normalized cell union that covers (GetCellUnion) or is contained
  // within (GetInteriorCellUnion) the given region and satisfies the
  // restrictions *EXCEPT* for min_level() and level_mod().  These criteria
  // cannot be satisfied using a cell union because cell unions are
  // automatically normalized by replacing four child cells with their parent
  // whenever possible.  (Note that the list of cell ids passed to the cell
  // union constructor does in fact satisfy all the given restrictions.)
  void GetCellUnion(S2Region const& region, S2CellUnion* covering);
  void GetInteriorCellUnion(S2Region const& region, S2CellUnion* interior);

  // Given a connected region and a starting point, return a set of cells at
  // the given level that cover the region.
  static void GetSimpleCovering(S2Region const& region, S2Point const& start,
                                int level, vector<S2CellId>* output);

 private:
  struct Candidate {
    S2Cell cell;
    bool is_terminal;        // Cell should not be expanded further.
    int num_children;        // Number of children that intersect the region.
#pragma warning(push)
#pragma warning( disable: 4200 )
    Candidate* children[0];  // Actual size may be 0, 4, 16, or 64 elements.
#pragma warning(pop)
  };

  // If the cell intersects the given region, return a new candidate with no
  // children, otherwise return NULL.  Also marks the candidate as "terminal"
  // if it should not be expanded further.
  Candidate* NewCandidate(S2Cell const& cell);

  // Return the log base 2 of the maximum number of children of a candidate.
  inline int max_children_shift() const { return 2 * level_mod_; }

  // Free the memory associated with a candidate.
  static void DeleteCandidate(Candidate* candidate, bool delete_children);

  // Process a candidate by either adding it to the result_ vector or
  // expanding its children and inserting it into the priority queue.
  // Passing an argument of NULL does nothing.
  void AddCandidate(Candidate* candidate);

  // Populate the children of "candidate" by expanding the given number of
  // levels from the given cell.  Returns the number of children that were
  // marked "terminal".
  int ExpandChildren(Candidate* candidate, S2Cell const& cell, int num_levels);

  // Computes a set of initial candidates that cover the given region.
  void GetInitialCandidates();

  // Generates a covering and stores it in result_.
  void GetCoveringInternal(S2Region const& region);

  // Given a region and a starting cell, return the set of all the
  // edge-connected cells at the same level that intersect "region".
  // The output cells are returned in arbitrary order.
  static void FloodFill(S2Region const& region, S2CellId const& start,
                        vector<S2CellId>* output);

  int min_level_;
  int max_level_;
  int level_mod_;
  int max_cells_;

  // We save a temporary copy of the pointer passed to GetCovering() in order
  // to avoid passing this parameter around internally.  It is only used (and
  // only valid) for the duration of a single GetCovering() call.
  S2Region const* region_;

  // A temporary variable used by GetCovering() that holds the cell ids that
  // have been added to the covering so far.
  scoped_ptr<vector<S2CellId> > result_;

  // We keep the candidates in a priority queue.  We specify a vector to hold
  // the queue entries since for some reason priority_queue<> uses a deque by
  // default.
  struct CompareQueueEntries;
  typedef pair<int, Candidate*> QueueEntry;
  typedef priority_queue<QueueEntry, vector<QueueEntry>,
                         CompareQueueEntries> CandidateQueue;
  scoped_ptr<CandidateQueue> pq_;

  // True if we're computing an interior covering.
  bool interior_covering_;

  // Counter of number of candidates created, for performance evaluation.
  int candidates_created_counter_;

  DISALLOW_EVIL_CONSTRUCTORS(S2RegionCoverer);
};

#endif  // UTIL_GEOMETRY_S2REGION_COVERER_H_
