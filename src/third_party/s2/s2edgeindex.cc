// Copyright 2009 Google Inc. All Rights Reserved.
//         julienbasch@google.com (Julien Basch)

// Implementation of class S2EdgeIndex, a fast lookup structure for edges in S2.
//
// An object of this class contains a set S of edges called the test edges.
// For a query edge q, you want to compute a superset of all test edges that
// intersect q.
//
// The idea is roughly that of
// Each edge is covered by one or several S2 cells, stored in a multimap
//   cell -> edge*.
// To perform a query, you cover the query edge with a set of cells.  For
// each such cell c, you find all test edges that are in c,in an ancestor of c
// or in a child of c.
//
// This is simple, but there are two complications:
//
// 1. For containment queries, the query edge is very long (from S2::Origin()
//    to the query point).  A standard cell covering of q is either useless or
//    too large.  The covering needs to be adapted to S: if a cell contains too
//    many edges from S, you subdivide it and keep only the subcells that
//    intersect q.  See comments for FindCandidateCrossings().
//
// 2. To decide if edge q could possibly cross edge e, we end up comparing
//    both with edges that bound s2 cells.  Numerical inaccuracies
//    can lead to inconcistencies, e.g.: there may be an edge b at the
//    boundary of two cells such that q and e are on opposite sides of b,
//    yet they cross each other.  This special case happens a lot if your
//    test and query edges are cell boundaries themselves, and this in turn
//    is a common case when regions are approximated by cell unions.
//
// We expand here on the solution to the second problem.  Two components:
//
// 1. Each test edge is thickened to a rectangle before it is S2-covered.
//    See the comment for GetThickenedEdgeCovering().
//
// 2. When recursing through the children of a cell c for a query edge q,
//    we test q against the boundaries of c's children in a 'lenient'
//    way.  That is, instead of testing e.g. area(abc)*area(abd) < 0,
//    we check if it is 'approximately negative'.
//
// To see how the second point is necessary, imagine that your query
// edge q is the North boundary of cell x.  We recurse into the four
// children a,b,c,d of x.  To do so, we check if q crosses or touches any
// of a,b,c or d boundaries.  As all the situations are degenerate, it is
// possible that all crossing tests return false, thus making q suddenly
// 'disappear'.  Using the lenient crossing test, we are guaranteed that q
// will intersect one of the four edges of the cross that bounds a,b,c,d.
// The same holds true if q passes through the cell center of x.



#include "s2edgeindex.h"

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <set>
using std::set;
using std::multiset;

#include <utility>
using std::pair;
using std::make_pair;


#include "base/logging.h"
#include "s2cell.h"
#include "s2edgeutil.h"
#include "s2polyline.h"
#include "s2regioncoverer.h"


//"When we test a query edge against a cell, we don't "
//"recurse if there are only a few test edges in it.  "
//"For testing, it is useful to always recurse to the end.  "
//"You don't want to use this flag anywhere but in tests.";
static bool FLAGS_always_recurse_on_children = false;

void S2EdgeIndex::Reset() {
  minimum_s2_level_used_ = S2CellId::kMaxLevel;
  index_computed_ = false;
  query_count_ = 0;
  mapping_.clear();
}

void S2EdgeIndex::ComputeIndex() {
  DCHECK(!index_computed_);

  for (int i = 0; i < num_edges(); ++i) {
    S2Point from, to;
    vector<S2CellId> cover;
    int level = GetCovering(*edge_from(i), *edge_to(i),
                            true, &cover);
    minimum_s2_level_used_ = min(minimum_s2_level_used_, level);

    for (vector<S2CellId>::const_iterator it = cover.begin(); it != cover.end();
         ++it) {
      mapping_.insert(make_pair(*it, i));
    }
  }
  index_computed_ = true;
}

bool S2EdgeIndex::IsIndexComputed() const {
  return index_computed_;
}

void S2EdgeIndex::IncrementQueryCount() {
  query_count_++;
}


// If we have m data edges and n query edges, then the brute force cost is
//   m * n * test_cost
// where test_cost is taken to be the cost of EdgeCrosser::RobustCrossing,
// measured to be about 30ns at the time of this writing.
//
// If we compute the index, the cost becomes:
//   m * cost_insert + n * cost_find(m)
//
// - cost_insert can be expected to be reasonably stable, and was measured
// at 1200ns with the BM_QuadEdgeInsertionCost benchmark.
//
// - cost_find depends on the length of the edge .  For m=1000 edges,
// we got timings ranging from 1ms (edge the length of the polygon) to
// 40ms.  The latter is for very long query edges, and needs to be
// optimized.  We will assume for the rest of the discussion that
// cost_find is roughly 3ms.
//
// When doing one additional query, the differential cost is
//   m * test_cost - cost_find(m)
// With the numbers above, it is better to use the quad tree (if we have it)
// if m >= 100.
//
// If m = 100, 30 queries will give m*n*test_cost = m*cost_insert = 100ms,
// while the marginal cost to find is 3ms.  Thus, this is a reasonable
// thing to do.
void S2EdgeIndex::PredictAdditionalCalls(int n) {
  if (index_computed_) return;
  if (num_edges() > 100 && (query_count_ + n) > 30) {
    ComputeIndex();
  }
}

void S2EdgeIndex::GetEdgesInParentCells(
    const vector<S2CellId>& cover,
    const CellEdgeMultimap& mapping,
    int minimum_s2_level_used,
    vector<int>* candidate_crossings) {
  // Find all parent cells of covering cells.
  set<S2CellId> parent_cells;
  for (vector<S2CellId>::const_iterator it = cover.begin(); it != cover.end();
       ++it) {
    for (int parent_level = it->level() - 1;
         parent_level >= minimum_s2_level_used;
         --parent_level) {
      if (!parent_cells.insert(it->parent(parent_level)).second) {
        break;  // cell is already in => parents are too.
      }
    }
  }

  // Put parent cell edge references into result.
  for (set<S2CellId>::const_iterator it = parent_cells.begin(); it
      != parent_cells.end(); ++it) {
    pair<CellEdgeMultimap::const_iterator,
        CellEdgeMultimap::const_iterator> range =
        mapping.equal_range(*it);
    for (CellEdgeMultimap::const_iterator it2 = range.first;
         it2 != range.second; ++it2) {
      candidate_crossings->push_back(it2->second);
    }
  }
}

// Returns true if ab possibly crosses cd, by clipping tiny angles to
// zero.
static bool LenientCrossing(S2Point const& a, S2Point const& b,
                            S2Point const& c, S2Point const& d) {
  DCHECK(S2::IsUnitLength(a));
  DCHECK(S2::IsUnitLength(b));
  DCHECK(S2::IsUnitLength(c));
  // See comment for RobustCCW() in s2.h
  const double kMaxDetError = 1.e-14;
  double acb = a.CrossProd(c).DotProd(b);
  double bda = b.CrossProd(d).DotProd(a);
  if (fabs(acb) < kMaxDetError || fabs(bda) < kMaxDetError) {
    return true;
  }
  if (acb * bda < 0) return false;
  double cbd = c.CrossProd(b).DotProd(d);
  double dac = d.CrossProd(a).DotProd(c);
  if (fabs(cbd) < kMaxDetError || fabs(dac) < kMaxDetError) {
    return true;
  }
  return (acb * cbd >= 0) && (acb * dac >= 0);
}

bool S2EdgeIndex::EdgeIntersectsCellBoundary(
    S2Point const& a, S2Point const& b, const S2Cell& cell) {
  //S2Point start_vertex = cell.GetVertex(0);

  S2Point vertices[4];
  for (int i = 0; i < 4; ++i) {
    vertices[i] = cell.GetVertex(i);
  }
  for (int i = 0; i < 4; ++i) {
    S2Point from_point = vertices[i];
    S2Point to_point = vertices[(i+1) % 4];
    if (LenientCrossing(a, b, from_point, to_point)) {
      return true;
    }
  }
  return false;
}

void S2EdgeIndex::GetEdgesInChildrenCells(
    S2Point const& a, S2Point const& b,
    vector<S2CellId>* cover,
    const CellEdgeMultimap& mapping,
    vector<int>* candidate_crossings) {
  CellEdgeMultimap::const_iterator it, start, end;

  int num_cells = 0;

  // Put all edge references of (covering cells + descendant cells) into result.
  // This relies on the natural ordering of S2CellIds.
  while (!cover->empty()) {
    S2CellId cell = cover->back();
    cover->pop_back();
    num_cells++;
    start = mapping.lower_bound(cell.range_min());
    end = mapping.upper_bound(cell.range_max());
    int num_edges = 0;
    bool rewind = FLAGS_always_recurse_on_children;
    // TODO(user): Maybe distinguish between edges in current cell, that
    // are going to be added anyhow, and edges in subcells, and rewind only
    // those.
    if (!rewind) {
      for (it = start; it != end; ++it) {
        candidate_crossings->push_back(it->second);
        ++num_edges;
        if (num_edges == 16 && !cell.is_leaf()) {
          rewind = true;
          break;
        }
      }
    }
    // If there are too many to insert, uninsert and recurse.
    if (rewind) {
      for (int i = 0; i < num_edges; ++i) {
        candidate_crossings->pop_back();
      }
      // Add cells at this level
      pair<CellEdgeMultimap::const_iterator,
           CellEdgeMultimap::const_iterator> eq =
                                          mapping.equal_range(cell);
      for (it = eq.first; it != eq.second; ++it) {
        candidate_crossings->push_back(it->second);
      }
      // Recurse on the children -- hopefully some will be empty.
      if (eq.first != start || eq.second != end) {
        S2Cell children[4];
        S2Cell c(cell);
        c.Subdivide(children);
        for (int i = 0; i < 4; ++i) {
          // TODO(user): Do the check for the four cells at once,
          // as it is enough to check the four edges between the cells.  At
          // this time, we are checking 16 edges, 4 times too many.
          //
          // Note that given the guarantee of AppendCovering, it is enough
          // to check that the edge intersect with the cell boundary as it
          // cannot be fully contained in a cell.
          if (EdgeIntersectsCellBoundary(a, b, children[i])) {
            cover->push_back(children[i].id());
          }
        }
      }
    }
  }
  VLOG(1) << "Num cells traversed: " << num_cells;
}

// Appends to "candidate_crossings" all edge references which may cross the
// given edge.  This is done by covering the edge and then finding all
// references of edges whose coverings overlap this covering. Parent cells
// are checked level by level.  Child cells are checked all at once by taking
// advantage of the natural ordering of S2CellIds.
void S2EdgeIndex::FindCandidateCrossings(
    S2Point const& a, S2Point const& b,
    vector<int>* candidate_crossings) const {
  DCHECK(index_computed_);
  vector<S2CellId> cover;
  GetCovering(a, b, false, &cover);
  GetEdgesInParentCells(cover, mapping_, minimum_s2_level_used_,
                        candidate_crossings);

  // TODO(user): An important optimization for long query
  // edges (Contains queries): keep a bounding cap and clip the query
  // edge to the cap before starting the descent.
  GetEdgesInChildrenCells(a, b, &cover, mapping_, candidate_crossings);

  // Remove duplicates: This is necessary because edge references are
  // inserted into the map once for each covering cell. (Testing shows
  // this to be at least as fast as using a set.)
  sort(candidate_crossings->begin(), candidate_crossings->end());
  candidate_crossings->erase(
      unique(candidate_crossings->begin(), candidate_crossings->end()),
      candidate_crossings->end());
}


// Returns the smallest cell containing all four points, or Sentinel
// if they are not all on the same face.
// The points don't need to be normalized.
static S2CellId ContainingCell(S2Point const& pa, S2Point const& pb,
                               S2Point const& pc, S2Point const& pd) {
  S2CellId a = S2CellId::FromPoint(pa);
  S2CellId b = S2CellId::FromPoint(pb);
  S2CellId c = S2CellId::FromPoint(pc);
  S2CellId d = S2CellId::FromPoint(pd);

  if (a.face() != b.face() || a.face() != c.face() || a.face() != d.face()) {
    return S2CellId::Sentinel();
  }

  while (a != b || a != c || a != d) {
    a = a.parent();
    b = b.parent();
    c = c.parent();
    d = d.parent();
  }
  return a;
}

// Returns the smallest cell containing both points, or Sentinel
// if they are not all on the same face.
// The points don't need to be normalized.
static S2CellId ContainingCell(S2Point const& pa, S2Point const& pb) {
  S2CellId a = S2CellId::FromPoint(pa);
  S2CellId b = S2CellId::FromPoint(pb);

  if (a.face() != b.face()) return S2CellId::Sentinel();

  while (a != b) {
    a = a.parent();
    b = b.parent();
  }
  return a;
}

int S2EdgeIndex::GetCovering(
    S2Point const& a, S2Point const& b,
    bool thicken_edge,
    vector<S2CellId>* edge_covering) const {
  edge_covering->clear();

  // Thicken the edge in all directions by roughly 1% of the edge length when
  // thicken_edge is true.
  static const double kThickening = 0.01;

  // Selects the ideal s2 level at which to cover the edge, this will be the
  // level whose S2 cells have a width roughly commensurate to the length of
  // the edge.  We multiply the edge length by 2*kThickening to guarantee the
  // thickening is honored (it's not a big deal if we honor it when we don't
  // request it) when doing the covering-by-cap trick.
  const double edge_length = a.Angle(b);
  const int ideal_level = S2::kMinWidth.GetMaxLevel(
      edge_length * (1 + 2 * kThickening));

  S2CellId containing_cell;
  if (!thicken_edge) {
    containing_cell = ContainingCell(a, b);
  } else {
    if (ideal_level == S2CellId::kMaxLevel) {
      // If the edge is tiny, instabilities are more likely, so we
      // want to limit the number of operations.
      // We pretend we are in a cell much larger so as to trigger the
      // 'needs covering' case, so we won't try to thicken the edge.
      containing_cell = S2CellId(0xFFF0).parent(3);
    } else {
      S2Point pq = (b - a) * kThickening;
      S2Point ortho = pq.CrossProd(a).Normalize() *
          edge_length * kThickening;
      S2Point p = a - pq;
      S2Point q = b + pq;
      // If p and q were antipodal, the edge wouldn't be lengthened,
      // and it could even flip!  This is not a problem because
      // ideal_level != 0 here.  The farther p and q can be is roughly
      // a quarter Earth away from each other, so we remain
      // Theta(kThickening).
      containing_cell = ContainingCell(p - ortho, p + ortho,
                                       q - ortho, q + ortho);
    }
  }

  // Best case: edge is fully contained in a cell that's not too big.
  if (containing_cell != S2CellId::Sentinel() &&
      containing_cell.level() >= ideal_level - 2) {
    edge_covering->push_back(containing_cell);
    return containing_cell.level();
  }

  if (ideal_level == 0) {
    // Edge is very long, maybe even longer than a face width, so the
    // trick below doesn't work.  For now, we will add the whole S2 sphere.
    // TODO(user): Do something a tad smarter (and beware of the
    // antipodal case).
    for (S2CellId cellid = S2CellId::Begin(0); cellid != S2CellId::End(0);
         cellid = cellid.next()) {
      edge_covering->push_back(cellid);
    }
    return 0;
  }
  // TODO(user): Check trick below works even when vertex is at interface
  // between three faces.

  // Use trick as in S2PolygonBuilder::PointIndex::FindNearbyPoint:
  // Cover the edge by a cap centered at the edge midpoint, then cover
  // the cap by four big-enough cells around the cell vertex closest to the
  // cap center.
  S2Point middle = ((a + b) / 2.0).Normalize();
  int actual_level = min(ideal_level, S2CellId::kMaxLevel-1);
  S2CellId::FromPoint(middle).AppendVertexNeighbors(
      actual_level, edge_covering);
  return actual_level;
}

void S2EdgeIndex::Iterator::GetCandidates(S2Point const& a, S2Point const& b) {
  edge_index_->PredictAdditionalCalls(1);
  is_brute_force_ = !edge_index_->IsIndexComputed();
  if (is_brute_force_) {
    edge_index_->IncrementQueryCount();
    current_index_ = 0;
    num_edges_ = edge_index_->num_edges();
  } else {
    candidates_.clear();
    edge_index_->FindCandidateCrossings(a, b, &candidates_);
    current_index_in_candidates_ = 0;
    if (!candidates_.empty()) {
      current_index_ = candidates_[0];
    }
  }
}
