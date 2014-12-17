// Copyright 2009 Google Inc. All Rights Reserved.

// Defines the class S2EdgeIndex, a fast lookup structure for edges in S2.

#ifndef UTIL_GEOMETRY_S2EDGEINDEX_H_
#define UTIL_GEOMETRY_S2EDGEINDEX_H_

#include <map>
using std::map;
using std::multimap;

#include <utility>
using std::pair;
using std::make_pair;

#include <vector>
using std::vector;


#include "base/logging.h"
#include "base/macros.h"
#include "s2cellid.h"
#include "s2edgeutil.h"

class S2Cell;

// This class structures a set S of data edges, so that one can quickly
// find which edges of S may potentially intersect or touch a query edge.
//
// The set S is assumed to be indexable by a consecutive sequence of
// integers in the range [0..num_edges()-1].  You subclass this class by
// defining the three virtual functions num_edges(), edge_from(),
// edge_to().  Then you use it as follows for a query edge (a,b):
//
//   MyS2EdgeIndex edge_index;
//   MyS2EdgeIndex::Iterator it(&edge_index);
//   S2Point const* from;
//   S2Point const* to;
//   for (it.GetCandidates(a, b); !it.Done(); it.Next()) {
//     edge_index.GetEdge(it.Index(), &from, &to);
//     ... RobustCrossing(a,b, from,to) ...
//   }
//
// What is this GetEdge()?  You don't want to use edge_from() and
// edge_to() in your own code: these are virtual functions that will
// add a lot of overhead.  The most efficient way is as above: you
// define GetEdge() in your S2EdgeIndex subclass that access the edge
// points as efficiently as possible.
//
// The function GetCandidates initializes the iterator to return a set
// of candidate edges from S, such that we are sure that any data edge
// that touches or crosses (a,b) is a candidate.
//
// This class returns all edges until it finds that it is worth it to compute
// a quad tree on the data set.  Chance my have it that you compute the quad
// tree exactly when it's too late and all the work is done, If this happens,
// we only double the total running time.
//
// You can help the class by declaring in advance that you will make a
// certain number of calls to GetCandidates():
//   MyS2EdgeIndex::Iterator it(&edge_index)
//   edge_index.PredictAdditionalCalls(n);
//   for (int i = 0; i < n; ++i) {
//     for (it.GetCandidates(v(i), v(i+1)); !it.Done(); it.Next()) {
//        ... RobustCrossing(v(i), v(i+1), it.From(), it.To()) ...
//     }
//   }
//
// Here, we say that we will call GetCandidates() n times.  If we have
// 1000 data edges and n=1000, then we will compute the quad tree
// immediately instead of waiting till we've wasted enough time to
// justify the cost.
//
// The tradeoff between brute force and quad tree depends on many
// things, we use a very approximate trade-off.
//
// See examples in S2Loop.cc and S2Polygon.cc, in particular, look at
// the optimization that allows one to use the EdgeCrosser.
//
// TODO(user): Get a better API without the clumsy GetCandidates().
//   Maybe edge_index.GetIterator()?
class S2EdgeIndex {
 public:
  S2EdgeIndex() { Reset(); }

  virtual ~S2EdgeIndex() {  }

  // An iterator on data edges that may cross a query edge (a,b).
  // Create the iterator, call GetCandidates(), then Done()/Next()
  // repeatedly.
  //
  // The current edge in the iteration has index Index(), goes between
  // From() and To().
  class Iterator {
   public:
    explicit Iterator(S2EdgeIndex* edge_index): edge_index_(edge_index) {}

    // Initializes the iterator to iterate over a set of candidates that may
    // cross the edge (a,b).
    void GetCandidates(S2Point const& a, S2Point const& b);

    // Index of the current edge in the iteration.
    int Index() const;

    // True if there is no more candidate.
    bool Done() const;

    // Iterate to the next available candidate.
    void Next();

   private:
    // The structure containing the data edges.
    S2EdgeIndex* edge_index_;

    // Tells whether GetCandidates() obtained the candidates through brute
    // force iteration or using the quad tree structure.
    bool is_brute_force_;

    // Index of the current edge and of the edge before the last Next() call.
    int current_index_;

    // Cache of edge_index_->num_edges() so that Done() doesn't call a virtual
    int num_edges_;

    // All the candidates obtained by GetCandidates() when we are
    // using a quad-tree (i.e. is_brute_force = false).
    vector<int> candidates_;

    // Index within array above.
    // We have: current_index_ = candidates_[current_index_in_candidates_].
    int current_index_in_candidates_;

    DISALLOW_COPY_AND_ASSIGN(Iterator);
  };

  // Empties the index in case it already contained something.
  void Reset();

  // Computes the index if not yet done and tells if the index has
  // been computed.
  void ComputeIndex();
  bool IsIndexComputed() const;

  // If the index hasn't been computed yet, looks at how much work has
  // gone into iterating using the brute force method, and how much
  // more work is planned as defined by 'cost'.  If it were to have been
  // cheaper to use a quad tree from the beginning, then compute it
  // now.  This guarantees that we will never use more than twice the
  // time we would have used had we known in advance exactly how many
  // edges we would have wanted to test.  It is the theoretical best.
  //
  // The value 'n' is the number of iterators we expect to request from
  // this edge index.
  void PredictAdditionalCalls(int n);

  // Overwrite these functions to give access to the underlying data.
  // The function num_edges() returns the number of edges in the
  // index, while edge_from(index) and edge_to(index) return the
  // "from" and "to" endpoints of the edge at the given index.
  virtual int num_edges() const = 0;
  virtual S2Point const* edge_from(int index) const = 0;
  virtual S2Point const* edge_to(int index) const = 0;

 protected:
  // Appends to result all edge references in the map that cross the
  // query edge, and possibly some more.
  void FindCandidateCrossings(S2Point const& a, S2Point const& b,
                              vector<int>* result) const;

  // Tell the index that we just received a new request for candidates.
  // Useful to compute when to switch to quad tree.
  void IncrementQueryCount();

 private:
  typedef multimap<S2CellId, int> CellEdgeMultimap;

  // Inserts the given directed edge into the quad tree.
  void Insert(S2Point const& a, S2Point const& b, int reference);

  // Computes a cell covering of an edge.  Returns the level of the s2 cells
  // used in the covering (only one level is ever used for each call).
  //
  // If thicken_edge is true, the edge is thickened and extended
  // by 1% of its length.
  //
  // It is guaranteed that no child of a covering cell will fully contain
  // the covered edge.
  int GetCovering(S2Point const& a, S2Point const& b,
                  bool thicken_edge,
                  vector<S2CellId>* result) const;

  // Adds to candidate_crossings all the edges present in any ancestor of any
  // cell of cover, down to minimum_s2_level_used.  The cell->edge map
  // is in the variable mapping.
  static void GetEdgesInParentCells(
    const vector<S2CellId>& cover,
    const CellEdgeMultimap& mapping,
    int minimum_s2_level_used,
    vector<int>* candidate_crossings);

  // Returns true if the edge and the cell (including boundary) intersect.
  static bool EdgeIntersectsCellBoundary(
    S2Point const& a, S2Point const& b,
    const S2Cell& cell);

  // Appends to candidate_crossings the edges that are fully contained in an
  // S2 covering of edge.  The covering of edge used is initially cover, but
  // is refined to eliminate quickly subcells that contain many edges but do
  // not intersect with edge.
  static void GetEdgesInChildrenCells(
    S2Point const& a, S2Point const& b,
    vector<S2CellId>* cover,
    const CellEdgeMultimap& mapping,
    vector<int>* candidate_crossings);

  // Maps cell ids to covered edges; has the property that the set of all cell
  // ids mapping to a particular edge forms a covering of that edge.
  CellEdgeMultimap mapping_;

  // No cell strictly below this level appears in mapping_.  Initially leaf
  // level, that's the minimum level at which we will ever look for test edges.
  int minimum_s2_level_used_;

  // Has the index been computed already?
  bool index_computed_;

  // Number of queries so far
  int query_count_;

  DISALLOW_COPY_AND_ASSIGN(S2EdgeIndex);
};

inline bool S2EdgeIndex::Iterator::Done() const {
  if (is_brute_force_) {
    return (current_index_ >= num_edges_);
  } else {
    return (size_t)current_index_in_candidates_ >= candidates_.size();
  }
}

inline int S2EdgeIndex::Iterator::Index() const {
  DCHECK(!Done());
  return current_index_;
}

inline void S2EdgeIndex::Iterator::Next() {
  DCHECK(!Done());
  if (is_brute_force_) {
    current_index_++;
  } else {
    current_index_in_candidates_++;
    if ((size_t)current_index_in_candidates_ < candidates_.size()) {
      current_index_ = candidates_[current_index_in_candidates_];
    }
  }
}

#endif  // UTIL_GEOMETRY_S2EDGEINDEX_H_
