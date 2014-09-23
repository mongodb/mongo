// Copyright 2005 Google Inc. All Rights Reserved.

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include "base/definer.h"
#include "s2.h"
#include "hash.h"

#include <set>
using std::set;
using std::multiset;

#include <vector>
using std::vector;

#include "s2polygon.h"

#include "base/port.h"  // for HASH_NAMESPACE_DECLARATION_START
#include "util/coding/coder.h"
#include "s2edgeindex.h"
#include "s2cap.h"
#include "s2cell.h"
#include "s2cellunion.h"
#include "s2latlngrect.h"
#include "s2polygonbuilder.h"
#include "s2polyline.h"

#include "mongo/util/mongoutils/str.h"
using mongoutils::str::stream;

static const unsigned char kCurrentEncodingVersionNumber = 1;

typedef pair<S2Point, S2Point> S2Edge;

S2Polygon::S2Polygon()
  : loops_(),
    bound_(S2LatLngRect::Empty()),
    owns_loops_(true),
    has_holes_(false),
    num_vertices_(0) {
}

S2Polygon::S2Polygon(vector<S2Loop*>* loops)
  : bound_(S2LatLngRect::Empty()),
    owns_loops_(true) {
  Init(loops);
}

S2Polygon::S2Polygon(S2Cell const& cell)
  : bound_(S2LatLngRect::Empty()),
    owns_loops_(true),
    has_holes_(false),
    num_vertices_(4) {
  S2Loop* loop = new S2Loop(cell);
  bound_ = loop->GetRectBound();
  loops_.push_back(loop);
}

S2Polygon::S2Polygon(S2Loop* loop)
  : bound_(loop->GetRectBound()),
    owns_loops_(false),
    has_holes_(false),
    num_vertices_(loop->num_vertices()) {
  loops_.push_back(loop);
}

void S2Polygon::Copy(S2Polygon const* src) {
  DCHECK_EQ(0, num_loops());
  for (int i = 0; i < src->num_loops(); ++i) {
    loops_.push_back(src->loop(i)->Clone());
  }
  bound_ = src->bound_;
  owns_loops_ = true;
  has_holes_ = src->has_holes_;
  num_vertices_ = src->num_vertices();
}

S2Polygon* S2Polygon::Clone() const {
  S2Polygon* result = new S2Polygon;
  result->Copy(this);
  return result;
}

void S2Polygon::Release(vector<S2Loop*>* loops) {
  if (loops != NULL) {
    loops->insert(loops->end(), loops_.begin(), loops_.end());
  }
  loops_.clear();
  bound_ = S2LatLngRect::Empty();
  has_holes_ = false;
}

static void DeleteLoopsInVector(vector<S2Loop*>* loops) {
  for (size_t i = 0; i < loops->size(); ++i) {
    delete loops->at(i);
  }
  loops->clear();
}

S2Polygon::~S2Polygon() {
  if (owns_loops_) DeleteLoopsInVector(&loops_);
}

typedef pair<S2Point, S2Point> S2PointPair;

HASH_NAMESPACE_START
template<> class hash<S2PointPair> {
public:
  size_t operator()(S2PointPair const& p) const {
    hash<S2Point> h;
    return h(p.first) + (h(p.second) << 1);
  }
};
HASH_NAMESPACE_END

bool S2Polygon::IsValid(const vector<S2Loop*>& loops, string* err) {
  // If a loop contains an edge AB, then no other loop may contain AB or BA.
  if (loops.size() > 1) {
    hash_map<S2PointPair, pair<int, int> > edges;
    for (size_t i = 0; i < loops.size(); ++i) {
      S2Loop* lp = loops[i];
      for (int j = 0; j < lp->num_vertices(); ++j) {
        S2PointPair key = make_pair(lp->vertex(j), lp->vertex(j + 1));
        if (edges.insert(make_pair(key, make_pair(i, j))).second) {
          key = make_pair(lp->vertex(j + 1), lp->vertex(j));
          if (edges.insert(make_pair(key, make_pair(i, j))).second)
            continue;
        }
        pair<int, int> other = edges[key];
        VLOG(2) << "Duplicate edge: loop " << i << ", edge " << j
                 << " and loop " << other.first << ", edge " << other.second;
        if (err) {
            *err = stream() << "Duplicate edge: loop " << i << ", edge " << j
                            << " and loop " << other.first << ", edge " << other.second;
        }
        return false;
      }
    }
  }

  // Verify that no loop covers more than half of the sphere, and that no
  // two loops cross.
  for (size_t i = 0; i < loops.size(); ++i) {
    if (!loops[i]->IsNormalized()) {
      VLOG(2) << "Loop " << i << " encloses more than half the sphere";
      if (err) *err = stream() << "Loop " << i << " encloses more than half the sphere";
      return false;
    }
    for (size_t j = i + 1; j < loops.size(); ++j) {
      // This test not only checks for edge crossings, it also detects
      // cases where the two boundaries cross at a shared vertex.
      if (loops[i]->ContainsOrCrosses(loops[j]) < 0) {
        VLOG(2) << "Loop " << i << " crosses loop " << j;
        if (err) *err = stream() << "Loop " << i << " crosses loop " << j;
        return false;
      }
    }
  }

  return true;
}

bool S2Polygon::IsValid(string* err) const {
  for (int i = 0; i < num_loops(); ++i) {
    if (!loop(i)->IsValid(err)) {
      return false;
    }
  }
  return IsValid(loops_, err);
}

bool S2Polygon::IsValid(bool check_loops, int max_adjacent) const {
  return IsValid();
}

void S2Polygon::InsertLoop(S2Loop* new_loop, S2Loop* parent,
                           LoopMap* loop_map) {
  vector<S2Loop*>* children = &(*loop_map)[parent];
  for (size_t i = 0; i < children->size(); ++i) {
    S2Loop* child = (*children)[i];
    if (child->ContainsNested(new_loop)) {
      InsertLoop(new_loop, child, loop_map);
      return;
    }
  }
  // No loop may contain the complement of another loop.  (Handling this case
  // is significantly more complicated.)
  DCHECK(parent == NULL || !new_loop->ContainsNested(parent));

  // Some of the children of the parent loop may now be children of
  // the new loop.
  vector<S2Loop*>* new_children = &(*loop_map)[new_loop];
  for (size_t i = 0; i < children->size();) {
    S2Loop* child = (*children)[i];
    if (new_loop->ContainsNested(child)) {
      new_children->push_back(child);
      children->erase(children->begin() + i);
    } else {
      ++i;
    }
  }
  children->push_back(new_loop);
}

void S2Polygon::InitLoop(S2Loop* loop, int depth, LoopMap* loop_map) {
  if (loop) {
    loop->set_depth(depth);
    loops_.push_back(loop);
  }
  vector<S2Loop*> const& children = (*loop_map)[loop];
  for (size_t i = 0; i < children.size(); ++i) {
    InitLoop(children[i], depth + 1, loop_map);
  }
}

bool S2Polygon::ContainsChild(S2Loop* a, S2Loop* b, LoopMap const& loop_map) {
  // This function is just used to verify that the loop map was
  // constructed correctly.

  if (a == b) return true;
  vector<S2Loop*> const& children = loop_map.find(a)->second;
  for (size_t i = 0; i < children.size(); ++i) {
    if (ContainsChild(children[i], b, loop_map)) return true;
  }
  return false;
}

void S2Polygon::Init(vector<S2Loop*>* loops) {
  if (S2::debug) {
      CHECK(IsValid(*loops));
  }
  DCHECK(loops_.empty());
  loops_.swap(*loops);

  num_vertices_ = 0;
  for (int i = 0; i < num_loops(); ++i) {
    num_vertices_ += loop(i)->num_vertices();
  }

  LoopMap loop_map;
  for (int i = 0; i < num_loops(); ++i) {
    InsertLoop(loop(i), NULL, &loop_map);
  }
  // Reorder the loops in depth-first traversal order.
  loops_.clear();
  InitLoop(NULL, -1, &loop_map);

  if (S2::debug) {
    // Check that the LoopMap is correct (this is fairly cheap).
    for (int i = 0; i < num_loops(); ++i) {
      for (int j = 0; j < num_loops(); ++j) {
        if (i == j) continue;
        CHECK_EQ(ContainsChild(loop(i), loop(j), loop_map),
                 loop(i)->ContainsNested(loop(j)));
      }
    }
  }

  // Compute the bounding rectangle of the entire polygon.
  has_holes_ = false;
  bound_ = S2LatLngRect::Empty();
  for (int i = 0; i < num_loops(); ++i) {
    if (loop(i)->sign() < 0) {
      has_holes_ = true;
    } else {
      bound_ = bound_.Union(loop(i)->GetRectBound());
    }
  }
}

int S2Polygon::GetParent(int k) const {
  int depth = loop(k)->depth();
  if (depth == 0) return -1;  // Optimization.
  while (--k >= 0 && loop(k)->depth() >= depth) continue;
  return k;
}

int S2Polygon::GetLastDescendant(int k) const {
  if (k < 0) return num_loops() - 1;
  int depth = loop(k)->depth();
  while (++k < num_loops() && loop(k)->depth() > depth) continue;
  return k - 1;
}

double S2Polygon::GetArea() const {
  double area = 0;
  for (int i = 0; i < num_loops(); ++i) {
    area += loop(i)->sign() * loop(i)->GetArea();
  }
  return area;
}

S2Point S2Polygon::GetCentroid() const {
  S2Point centroid;
  for (int i = 0; i < num_loops(); ++i) {
    centroid += loop(i)->sign() * loop(i)->GetCentroid();
  }
  return centroid;
}

int S2Polygon::ContainsOrCrosses(S2Loop const* b) const {
  bool inside = false;
  for (int i = 0; i < num_loops(); ++i) {
    int result = loop(i)->ContainsOrCrosses(b);
    if (result < 0) return -1;  // The loop boundaries intersect.
    if (result > 0) inside ^= true;
  }
  return static_cast<int>(inside);  // True if loop B is contained by the
                                    // polygon.
}

bool S2Polygon::AnyLoopContains(S2Loop const* b) const {
  // Return true if any loop contains the given loop.
  for (int i = 0; i < num_loops(); ++i) {
    if (loop(i)->Contains(b)) return true;
  }
  return false;
}

bool S2Polygon::ContainsAllShells(S2Polygon const* b) const {
  // Return true if this polygon (A) contains all the shells of B.
  for (int j = 0; j < b->num_loops(); ++j) {
    if (b->loop(j)->sign() < 0) continue;
    if (ContainsOrCrosses(b->loop(j)) <= 0) {
      // Shell of B is not contained by A, or the boundaries intersect.
      return false;
    }
  }
  return true;
}

bool S2Polygon::ExcludesAllHoles(S2Polygon const* b) const {
  // Return true if this polygon (A) excludes (i.e. does not intersect)
  // all holes of B.
  for (int j = 0; j < b->num_loops(); ++j) {
    if (b->loop(j)->sign() > 0) continue;
    if (ContainsOrCrosses(b->loop(j)) != 0) {
      // Hole of B is contained by A, or the boundaries intersect.
      return false;
    }
  }
  return true;
}

bool S2Polygon::IntersectsAnyShell(S2Polygon const* b) const {
  // Return true if this polygon (A) intersects any shell of B.
  for (int j = 0; j < b->num_loops(); ++j) {
    if (b->loop(j)->sign() < 0) continue;
    if (IntersectsShell(b->loop(j)) != 0)
      return true;
  }
  return false;
}

bool S2Polygon::IntersectsShell(S2Loop const* b) const {
  bool inside = false;
  for (int i = 0; i < num_loops(); ++i) {
    if (loop(i)->Contains(b)) {
      inside ^= true;
    } else if (!b->Contains(loop(i)) && loop(i)->Intersects(b)) {
      // We definitely have an intersection if the loops intersect AND one
      // is not properly contained in the other. If A (this) is properly
      // contained in a loop of B, we don't know yet if it may be actually
      // inside a hole within B.
      return true;
    }
  }
  return inside;
}

bool S2Polygon::Contains(S2Polygon const* b) const {
  // If both polygons have one loop, use the more efficient S2Loop method.
  // Note that S2Loop::Contains does its own bounding rectangle check.
  if (num_loops() == 1 && b->num_loops() == 1) {
    return loop(0)->Contains(b->loop(0));
  }

  // Otherwise if neither polygon has holes, we can still use the more
  // efficient S2Loop::Contains method (rather than ContainsOrCrosses),
  // but it's worthwhile to do our own bounds check first.
  if (!bound_.Contains(b->bound_)) {
    // If the union of the bounding boxes spans the full longitude range,
    // it is still possible that polygon A contains B.  (This is only
    // possible if at least one polygon has multiple shells.)
    if (!bound_.lng().Union(b->bound_.lng()).is_full()) return false;
  }
  if (!has_holes_ && !b->has_holes_) {
    for (int j = 0; j < b->num_loops(); ++j) {
      if (!AnyLoopContains(b->loop(j))) return false;
    }
    return true;
  }

  // This could be implemented more efficiently for polygons with lots of
  // holes by keeping a copy of the LoopMap computed during initialization.
  // However, in practice most polygons are one loop, and multiloop polygons
  // tend to consist of many shells rather than holes.  In any case, the real
  // way to get more efficiency is to implement a sub-quadratic algorithm
  // such as building a trapezoidal map.

  // Every shell of B must be contained by an odd number of loops of A,
  // and every hole of A must be contained by an even number of loops of B.
  return ContainsAllShells(b) && b->ExcludesAllHoles(this);
}

bool S2Polygon::Intersects(S2Polygon const* b) const {
  // A.Intersects(B) if and only if !Complement(A).Contains(B).  However,
  // implementing a Complement() operation is trickier than it sounds,
  // and in any case it's more efficient to test for intersection directly.

  // If both polygons have one loop, use the more efficient S2Loop method.
  // Note that S2Loop::Intersects does its own bounding rectangle check.
  if (num_loops() == 1 && b->num_loops() == 1) {
    return loop(0)->Intersects(b->loop(0));
  }

  // Otherwise if neither polygon has holes, we can still use the more
  // efficient S2Loop::Intersects method.  The polygons intersect if and
  // only if some pair of loop regions intersect.
  if (!bound_.Intersects(b->bound_)) return false;
  if (!has_holes_ && !b->has_holes_) {
    for (int i = 0; i < num_loops(); ++i) {
      for (int j = 0; j < b->num_loops(); ++j) {
        if (loop(i)->Intersects(b->loop(j))) return true;
      }
    }
    return false;
  }

  // Otherwise if any shell of B is contained by an odd number of loops of A,
  // or any shell of A is contained by an odd number of loops of B, or there is
  // an intersection without containment, then there is an intersection.
  return IntersectsAnyShell(b) || b->IntersectsAnyShell(this);
}

S2Cap S2Polygon::GetCapBound() const {
  return bound_.GetCapBound();
}

bool S2Polygon::Contains(S2Cell const& cell) const {
  if (num_loops() == 1) {
    return loop(0)->Contains(cell);
  }

  // We can't check bound_.Contains(cell.GetRectBound()) because S2Cell's
  // GetRectBound() calculation is not precise.
  if (!bound_.Contains(cell.GetCenter())) return false;

  S2Loop cell_loop(cell);
  S2Polygon cell_poly(&cell_loop);
  bool contains = Contains(&cell_poly);
  if (contains) {
      DCHECK(Contains(cell.GetCenter()));
  }
  return contains;
}

bool S2Polygon::ApproxContains(S2Polygon const* b,
                               S1Angle vertex_merge_radius) const {
  S2Polygon difference;
  difference.InitToDifferenceSloppy(b, this, vertex_merge_radius);
  return difference.num_loops() == 0;
}

bool S2Polygon::MayIntersect(S2Cell const& cell) const {
  if (num_loops() == 1) {
    return loop(0)->MayIntersect(cell);
  }
  if (!bound_.Intersects(cell.GetRectBound())) return false;

  S2Loop cell_loop(cell);
  S2Polygon cell_poly(&cell_loop);
  bool intersects = Intersects(&cell_poly);
  if (!intersects) {
      DCHECK(!Contains(cell.GetCenter()));
  }
  return intersects;
}

bool S2Polygon::VirtualContainsPoint(S2Point const& p) const {
  return Contains(p);  // The same as Contains() below, just virtual.
}

bool S2Polygon::Contains(S2Point const& p) const {
  if (num_loops() == 1) {
    return loop(0)->Contains(p);  // Optimization.
  }
  if (!bound_.Contains(p)) return false;
  bool inside = false;
  for (int i = 0; i < num_loops(); ++i) {
    inside ^= loop(i)->Contains(p);
    if (inside && !has_holes_) break;  // Shells are disjoint.
  }
  return inside;
}

void S2Polygon::Encode(Encoder* const encoder) const {
  encoder->Ensure(10);  // Sufficient
  encoder->put8(kCurrentEncodingVersionNumber);
  encoder->put8(owns_loops_);
  encoder->put8(has_holes_);
  encoder->put32(loops_.size());
  DCHECK_GE(encoder->avail(), 0);

  for (int i = 0; i < num_loops(); ++i) {
    loop(i)->Encode(encoder);
  }
  bound_.Encode(encoder);
}

bool S2Polygon::Decode(Decoder* const decoder) {
  return DecodeInternal(decoder, false);
}

bool S2Polygon::DecodeWithinScope(Decoder* const decoder) {
  return DecodeInternal(decoder, true);
}

bool S2Polygon::DecodeInternal(Decoder* const decoder, bool within_scope) {
  unsigned char version = decoder->get8();
  if (version > kCurrentEncodingVersionNumber) return false;

  if (owns_loops_) DeleteLoopsInVector(&loops_);

  owns_loops_ = decoder->get8();
  has_holes_ = decoder->get8();
  int num_loops = decoder->get32();
  loops_.clear();
  loops_.reserve(num_loops);
  num_vertices_ = 0;
  for (int i = 0; i < num_loops; ++i) {
    loops_.push_back(new S2Loop);
    if (within_scope) {
      if (!loops_.back()->DecodeWithinScope(decoder)) return false;
    } else {
      if (!loops_.back()->Decode(decoder)) return false;
    }
    num_vertices_ += loops_.back()->num_vertices();
  }
  if (!bound_.Decode(decoder)) return false;

  DCHECK(IsValid(loops_));

  return decoder->avail() >= 0;
}

// Indexing structure to efficiently ClipEdge() of a polygon.  This is
// an abstract class because we need to use if for both polygons (for
// InitToIntersection() and friends) and for sets of vectors of points
// (for InitToSimplified()).
//
// Usage -- in your subclass:
//   - Call AddLoop() for each of your loops -- and keep them accessible in
//     your subclass.
//   - Overwrite EdgeFromTo(), calling DecodeIndex() and accessing your
//     underlying data with the resulting two indices.
class S2LoopSequenceIndex: public S2EdgeIndex {
 public:
  S2LoopSequenceIndex(): num_edges_(0), num_loops_(0) {}
  ~S2LoopSequenceIndex() {}

  void AddLoop(int num_vertices) {
    int vertices_so_far = num_edges_;
    loop_to_first_index_.push_back(vertices_so_far);
    index_to_loop_.resize(vertices_so_far + num_vertices);
    for (int i = 0; i < num_vertices; ++i) {
      index_to_loop_[vertices_so_far] = num_loops_;
      vertices_so_far++;
    }
    num_edges_ += num_vertices;
    num_loops_++;
  }

  inline void DecodeIndex(int index,
                          int* loop_index, int* vertex_in_loop) const {
    *loop_index = index_to_loop_[index];
    *vertex_in_loop = index - loop_to_first_index_[*loop_index];
  }

  // It is faster to return both vertices at once.  It makes a difference
  // for small polygons.
  virtual void EdgeFromTo(int index,
                          S2Point const* * from, S2Point const* * to) const = 0;

  int num_edges() const { return num_edges_; }

  virtual S2Point const* edge_from(int index) const {
    S2Point const* from;
    S2Point const* to;
    EdgeFromTo(index, &from, &to);
    return from;
  }

  virtual S2Point const* edge_to(int index) const {
    S2Point const* from;
    S2Point const* to;
    EdgeFromTo(index, &from, &to);
    return to;
  }

 protected:
  // Map from the unidimensional edge index to the loop this edge
  // belongs to.
  vector<int> index_to_loop_;

  // Reverse of index_to_loop_: maps a loop index to the
  // unidimensional index of the first edge in the loop.
  vector<int> loop_to_first_index_;

  // Total number of edges.
  int num_edges_;

  // Total number of loops.
  int num_loops_;
};

// Indexing structure for an S2Polygon.
class S2PolygonIndex: public S2LoopSequenceIndex {
 public:
  S2PolygonIndex(S2Polygon const* poly, bool reverse):
      poly_(poly),
      reverse_(reverse) {
    for (int iloop = 0; iloop < poly_->num_loops(); ++iloop) {
      AddLoop(poly_->loop(iloop)->num_vertices());
    }
  }

  virtual ~S2PolygonIndex() {}

  virtual void EdgeFromTo(int index,
                          S2Point const* * from, S2Point const* * to) const {
    int loop_index;
    int vertex_in_loop;
    DecodeIndex(index, &loop_index, &vertex_in_loop);
    S2Loop const* loop(poly_->loop(loop_index));
    int from_index, to_index;
    if (loop->is_hole() ^ reverse_) {
      from_index = loop->num_vertices() - 1 - vertex_in_loop;
      to_index = 2 * loop->num_vertices() - 2 - vertex_in_loop;
    } else {
      from_index = vertex_in_loop;
      to_index = vertex_in_loop + 1;
    }
    *from = &(loop->vertex(from_index));
    *to = &(loop->vertex(to_index));
  }

 private:
  S2Polygon const* poly_;
  bool reverse_;
};

// Indexing structure for a sequence of loops (not in the sense of S2:
// the loops can self-intersect).  Each loop is given in a vector<S2Point>
// where, as in S2Loop, the last vertex is implicitely joined to the first.
// Add each loop individually with AddVector().  The caller owns
// the vector<S2Point>'s.
class S2LoopsAsVectorsIndex: public S2LoopSequenceIndex {
 public:
  S2LoopsAsVectorsIndex() {}
  ~S2LoopsAsVectorsIndex() {}

  void AddVector(vector<S2Point> const* v) {
    loops_.push_back(v);
    AddLoop(v->size());
  }

  virtual void EdgeFromTo(int index,
                          S2Point const* *from, S2Point const* *to) const {
    int loop_index;
    int vertex_in_loop;
    DecodeIndex(index, &loop_index, &vertex_in_loop);
    vector<S2Point> const* loop = loops_[loop_index];
    *from = &loop->at(vertex_in_loop);
    *to = &loop->at((size_t)vertex_in_loop == loop->size() - 1
                      ? 0
                      : vertex_in_loop + 1);
  }

 private:
  vector< vector<S2Point> const* > loops_;
};

typedef vector<pair<double, S2Point> > IntersectionSet;

static void AddIntersection(S2Point const& a0, S2Point const& a1,
                            S2Point const& b0, S2Point const& b1,
                            bool add_shared_edges, int crossing,
                            IntersectionSet* intersections) {
  if (crossing > 0) {
    // There is a proper edge crossing.
    S2Point x = S2EdgeUtil::GetIntersection(a0, a1, b0, b1);
    double t = S2EdgeUtil::GetDistanceFraction(x, a0, a1);
    intersections->push_back(make_pair(t, x));

  } else if (S2EdgeUtil::VertexCrossing(a0, a1, b0, b1)) {
    // There is a crossing at one of the vertices.  The basic rule is simple:
    // if a0 equals one of the "b" vertices, the crossing occurs at t=0;
    // otherwise, it occurs at t=1.
    //
    // This has the effect that when two symmetric edges are
    // encountered (an edge an its reverse), neither one is included
    // in the output.  When two duplicate edges are encountered, both
    // are included in the output.  The "add_shared_edges" flag allows
    // one of these two copies to be removed by changing its
    // intersection parameter from 0 to 1.

    double t = (a0 == b0 || a0 == b1) ? 0 : 1;
    if (!add_shared_edges && a1 == b1) t = 1;
    intersections->push_back(make_pair(t, t == 0 ? a0 : a1));
  }
}

static void ClipEdge(S2Point const& a0, S2Point const& a1,
                     S2LoopSequenceIndex* b_index,
                     bool add_shared_edges, IntersectionSet* intersections) {
  // Find all points where the polygon B intersects the edge (a0,a1),
  // and add the corresponding parameter values (in the range [0,1]) to
  // "intersections".
  S2LoopSequenceIndex::Iterator it(b_index);
  it.GetCandidates(a0, a1);
  S2EdgeUtil::EdgeCrosser crosser(&a0, &a1, &a0);
  S2Point const* from;
  S2Point const* to = NULL;
  for (; !it.Done(); it.Next()) {
    S2Point const* const previous_to = to;
    b_index->EdgeFromTo(it.Index(), &from, &to);
    if (previous_to != from) crosser.RestartAt(from);
    int crossing = crosser.RobustCrossing(to);
    if (crossing < 0) continue;
    AddIntersection(a0, a1, *from, *to,
                    add_shared_edges, crossing, intersections);
  }
}


static void ClipBoundary(S2Polygon const* a, bool reverse_a,
                         S2Polygon const* b, bool reverse_b, bool invert_b,
                         bool add_shared_edges, S2PolygonBuilder* builder) {
  // Clip the boundary of A to the interior of B, and add the resulting edges
  // to "builder".  Shells are directed CCW and holes are directed clockwise,
  // unless "reverse_a" or "reverse_b" is true in which case these directions
  // in the corresponding polygon are reversed.  If "invert_b" is true, the
  // boundary of A is clipped to the exterior rather than the interior of B.
  // If "add_shared_edges" is true, then the output will include any edges
  // that are shared between A and B (both edges must be in the same direction
  // after any edge reversals are taken into account).

  S2PolygonIndex b_index(b, reverse_b);
  b_index.PredictAdditionalCalls(a->num_vertices());

  IntersectionSet intersections;
  for (int i = 0; i < a->num_loops(); ++i) {
    S2Loop* a_loop = a->loop(i);
    int n = a_loop->num_vertices();
    int dir = (a_loop->is_hole() ^ reverse_a) ? -1 : 1;
    bool inside = b->Contains(a_loop->vertex(0)) ^ invert_b;
    for (int j = (dir > 0) ? 0 : n; n > 0; --n, j += dir) {
      S2Point const& a0 = a_loop->vertex(j);
      S2Point const& a1 = a_loop->vertex(j + dir);
      intersections.clear();
      ClipEdge(a0, a1, &b_index, add_shared_edges, &intersections);

      if (inside) intersections.push_back(make_pair(0, a0));
      inside = (intersections.size() & 1);
      DCHECK_EQ((b->Contains(a1) ^ invert_b), inside);
      if (inside) intersections.push_back(make_pair(1, a1));
      sort(intersections.begin(), intersections.end());
      for (size_t k = 0; k < intersections.size(); k += 2) {
        if (intersections[k] == intersections[k+1]) continue;
        builder->AddEdge(intersections[k].second, intersections[k+1].second);
      }
    }
  }
}

void S2Polygon::InitToIntersection(S2Polygon const* a, S2Polygon const* b) {
  InitToIntersectionSloppy(a, b, S2EdgeUtil::kIntersectionTolerance);
}

void S2Polygon::InitToIntersectionSloppy(S2Polygon const* a, S2Polygon const* b,
                                         S1Angle vertex_merge_radius) {
  DCHECK_EQ(0, num_loops());
  if (!a->bound_.Intersects(b->bound_)) return;

  // We want the boundary of A clipped to the interior of B,
  // plus the boundary of B clipped to the interior of A,
  // plus one copy of any directed edges that are in both boundaries.

  S2PolygonBuilderOptions options(S2PolygonBuilderOptions::DIRECTED_XOR());
  options.set_vertex_merge_radius(vertex_merge_radius);
  S2PolygonBuilder builder(options);
  ClipBoundary(a, false, b, false, false, true, &builder);
  ClipBoundary(b, false, a, false, false, false, &builder);
  if (!builder.AssemblePolygon(this, NULL)) {
    S2LOG(DFATAL) << "Bad directed edges in InitToIntersection";
  }
}

void S2Polygon::InitToUnion(S2Polygon const* a, S2Polygon const* b) {
  InitToUnionSloppy(a, b, S2EdgeUtil::kIntersectionTolerance);
}

void S2Polygon::InitToUnionSloppy(S2Polygon const* a, S2Polygon const* b,
                                  S1Angle vertex_merge_radius) {
  DCHECK_EQ(0, num_loops());

  // We want the boundary of A clipped to the exterior of B,
  // plus the boundary of B clipped to the exterior of A,
  // plus one copy of any directed edges that are in both boundaries.

  S2PolygonBuilderOptions options(S2PolygonBuilderOptions::DIRECTED_XOR());
  options.set_vertex_merge_radius(vertex_merge_radius);
  S2PolygonBuilder builder(options);
  ClipBoundary(a, false, b, false, true, true, &builder);
  ClipBoundary(b, false, a, false, true, false, &builder);
  if (!builder.AssemblePolygon(this, NULL)) {
    S2LOG(DFATAL) << "Bad directed edges";
  }
}

void S2Polygon::InitToDifference(S2Polygon const* a, S2Polygon const* b) {
  InitToDifferenceSloppy(a, b, S2EdgeUtil::kIntersectionTolerance);
}

void S2Polygon::InitToDifferenceSloppy(S2Polygon const* a, S2Polygon const* b,
                                       S1Angle vertex_merge_radius) {
  DCHECK_EQ(0, num_loops());

  // We want the boundary of A clipped to the exterior of B,
  // plus the reversed boundary of B clipped to the interior of A,
  // plus one copy of any edge in A that is also a reverse edge in B.

  S2PolygonBuilderOptions options(S2PolygonBuilderOptions::DIRECTED_XOR());
  options.set_vertex_merge_radius(vertex_merge_radius);
  S2PolygonBuilder builder(options);
  ClipBoundary(a, false, b, true, true, true, &builder);
  ClipBoundary(b, true, a, false, false, false, &builder);
  if (!builder.AssemblePolygon(this, NULL)) {
    S2LOG(DFATAL) << "Bad directed edges in InitToDifference";
  }
}

// Takes a loop and simplifies it.  This may return a self-intersecting
// polyline.  Always keeps the first vertex from the loop.
vector<S2Point>* SimplifyLoopAsPolyline(S2Loop const* loop, S1Angle tolerance) {
  vector<S2Point> points(loop->num_vertices() + 1);
  // Add the first vertex at the beginning and at the end.
  for (int i = 0; i <= loop->num_vertices(); ++i) {
    points[i] = loop->vertex(i);
  }
  S2Polyline line(points);
  vector<int> indices;
  line.SubsampleVertices(tolerance, &indices);
  if (indices.size() <= 2) return NULL;
  // Add them all except the last: it is the same as the first.
  vector<S2Point>* simplified_line = new vector<S2Point>(indices.size() - 1);
  VLOG(4) << "Now simplified to: ";
  for (size_t i = 0; i + 1 < indices.size(); ++i) {
    (*simplified_line)[i] = line.vertex(indices[i]);
    VLOG(4) << S2LatLng(line.vertex(indices[i]));
  }
  return simplified_line;
}

// Takes a set of possibly intersecting edges, stored in an
// S2EdgeIndex.  Breaks the edges into small pieces so that there is
// no intersection anymore, and adds all these edges to the builder.
void BreakEdgesAndAddToBuilder(S2LoopsAsVectorsIndex* edge_index,
                               S2PolygonBuilder* builder) {
  // If there are self intersections, we add the pieces separately.
  for (int i = 0; i < edge_index->num_edges(); ++i) {
    S2Point const* from;
    S2Point const* to;
    edge_index->EdgeFromTo(i, &from, &to);

    IntersectionSet intersections;
    intersections.push_back(make_pair(0, *from));
    // add_shared_edges can be false or true: it makes no difference
    // due to the way we call ClipEdge.
    ClipEdge(*from, *to, edge_index, false, &intersections);
    intersections.push_back(make_pair(1, *to));
    sort(intersections.begin(), intersections.end());
    for (size_t k = 0; k + 1 < intersections.size(); ++k) {
      if (intersections[k] == intersections[k+1]) continue;
      builder->AddEdge(intersections[k].second, intersections[k+1].second);
    }
  }
}

// Simplifies the polygon.   The algorithm is straightforward and naive:
//   1. Simplify each loop by removing points while staying in the
//      tolerance zone.  This uses S2Polyline::SubsampleVertices(),
//      which is not guaranteed to be optimal in terms of number of
//      points.
//   2. Break any edge in pieces such that no piece intersects any
//      other.
//   3. Use the polygon builder to regenerate the full polygon.
void S2Polygon::InitToSimplified(S2Polygon const* a, S1Angle tolerance) {
  S2PolygonBuilderOptions builder_options =
      S2PolygonBuilderOptions::UNDIRECTED_XOR();
  builder_options.set_validate(false);
  // Ideally, we would want to set the vertex_merge_radius of the
  // builder roughly to tolerance (and in fact forego the edge
  // splitting step).  Alas, if we do that, we are liable to the
  // 'chain effect', where vertices are merged with closeby vertices
  // and so on, so that a vertex can move by an arbitrary distance.
  // So we remain conservative:
  builder_options.set_vertex_merge_radius(tolerance * 0.10);
  S2PolygonBuilder builder(builder_options);

  // Simplify each loop separately and add to the edge index
  S2LoopsAsVectorsIndex index;
  vector<vector<S2Point>*> simplified_loops;
  for (int i = 0; i < a->num_loops(); ++i) {
    vector<S2Point>* simpler = SimplifyLoopAsPolyline(a->loop(i), tolerance);
    if (NULL == simpler) continue;
    simplified_loops.push_back(simpler);
    index.AddVector(simpler);
  }
  if (0 != index.num_edges()) {
    BreakEdgesAndAddToBuilder(&index, &builder);

    if (!builder.AssemblePolygon(this, NULL)) {
      S2LOG(DFATAL) << "Bad edges in InitToSimplified.";
    }
  }

  for (size_t i = 0; i < simplified_loops.size(); ++i) {
    delete simplified_loops[i];
  }
  simplified_loops.clear();
}

void S2Polygon::InternalClipPolyline(bool invert,
                                     S2Polyline const* a,
                                     vector<S2Polyline*> *out,
                                     S1Angle merge_radius) const {
  // Clip the polyline A to the interior of this polygon.
  // The resulting polyline(s) will be appended to 'out'.
  // If invert is true, we clip A to the exterior of this polygon instead.
  // Vertices will be dropped such that adjacent vertices will not
  // be closer than 'merge_radius'.
  //
  // We do the intersection/subtraction by walking the polyline edges.
  // For each edge, we compute all intersections with the polygon boundary
  // and sort them in increasing order of distance along that edge.
  // We then divide the intersection points into pairs, and output a
  // clipped polyline segment for each one.
  // We keep track of whether we're inside or outside of the polygon at
  // all times to decide which segments to output.

  CHECK(out->empty());

  IntersectionSet intersections;
  vector<S2Point> vertices;
  S2PolygonIndex poly_index(this, false);
  int n = a->num_vertices();
  bool inside = Contains(a->vertex(0)) ^ invert;
  for (int j = 0; j < n-1; j++) {
    S2Point const& a0 = a->vertex(j);
    S2Point const& a1 = a->vertex(j + 1);
    ClipEdge(a0, a1, &poly_index, true, &intersections);
    if (inside) intersections.push_back(make_pair(0, a0));
    inside = (intersections.size() & 1);
    DCHECK_EQ((Contains(a1) ^ invert), inside);
    if (inside) intersections.push_back(make_pair(1, a1));
    sort(intersections.begin(), intersections.end());
    // At this point we have a sorted array of vertex pairs representing
    // the edge(s) obtained after clipping (a0,a1) against the polygon.
    for (size_t k = 0; k < intersections.size(); k += 2) {
      if (intersections[k] == intersections[k+1]) continue;
      S2Point const& v0 = intersections[k].second;
      S2Point const& v1 = intersections[k+1].second;

      // If the gap from the previous vertex to this one is large
      // enough, start a new polyline.
      if (!vertices.empty() &&
          vertices.back().Angle(v0) > merge_radius.radians()) {
        out->push_back(new S2Polyline(vertices));
        vertices.clear();
      }
      // Append this segment to the current polyline, ignoring any
      // vertices that are too close to the previous vertex.
      if (vertices.empty()) vertices.push_back(v0);
      if (vertices.back().Angle(v1) > merge_radius.radians()) {
        vertices.push_back(v1);
      }
    }
    intersections.clear();
  }
  if (!vertices.empty()) {
    out->push_back(new S2Polyline(vertices));
  }
}

void S2Polygon::IntersectWithPolyline(
    S2Polyline const* a,
    vector<S2Polyline*> *out) const {
  IntersectWithPolylineSloppy(a, out, S2EdgeUtil::kIntersectionTolerance);
}

void S2Polygon::IntersectWithPolylineSloppy(
    S2Polyline const* a,
    vector<S2Polyline*> *out,
    S1Angle vertex_merge_radius) const {
  InternalClipPolyline(false, a, out, vertex_merge_radius);
}

void S2Polygon::SubtractFromPolyline(S2Polyline const* a,
                                     vector<S2Polyline*> *out) const {
  SubtractFromPolylineSloppy(a, out, S2EdgeUtil::kIntersectionTolerance);
}

void S2Polygon::SubtractFromPolylineSloppy(
    S2Polyline const* a,
    vector<S2Polyline*> *out,
    S1Angle vertex_merge_radius) const {
  InternalClipPolyline(true, a, out, vertex_merge_radius);
}


S2Polygon* S2Polygon::DestructiveUnion(vector<S2Polygon*>* polygons) {
  return DestructiveUnionSloppy(polygons, S2EdgeUtil::kIntersectionTolerance);
}

S2Polygon* S2Polygon::DestructiveUnionSloppy(vector<S2Polygon*>* polygons,
                                             S1Angle vertex_merge_radius) {
  // Effectively create a priority queue of polygons in order of number of
  // vertices.  Repeatedly union the two smallest polygons and add the result
  // to the queue until we have a single polygon to return.
  typedef multimap<int, S2Polygon*> QueueType;
  QueueType queue;  // Map from # of vertices to polygon.
  for (size_t i = 0; i < polygons->size(); ++i)
    queue.insert(make_pair((*polygons)[i]->num_vertices(), (*polygons)[i]));
  polygons->clear();

  while (queue.size() > 1) {
    // Pop two simplest polygons from queue.
    QueueType::iterator smallest_it = queue.begin();
    int a_size = smallest_it->first;
    S2Polygon* a_polygon = smallest_it->second;
    queue.erase(smallest_it);
    smallest_it = queue.begin();
    int b_size = smallest_it->first;
    S2Polygon* b_polygon = smallest_it->second;
    queue.erase(smallest_it);

    // Union and add result back to queue.
    S2Polygon* union_polygon = new S2Polygon();
    union_polygon->InitToUnionSloppy(a_polygon, b_polygon, vertex_merge_radius);
    delete a_polygon;
    delete b_polygon;
    queue.insert(make_pair(a_size + b_size, union_polygon));
    // We assume that the number of vertices in the union polygon is the
    // sum of the number of vertices in the original polygons, which is not
    // always true, but will almost always be a decent approximation, and
    // faster than recomputing.
  }

  if (queue.empty())
    return new S2Polygon();
  else
    return queue.begin()->second;
}

void S2Polygon::InitToCellUnionBorder(S2CellUnion const& cells) {
  // Use a polygon builder to union the cells in the union.  Due to rounding
  // errors, we can't do an exact union - when a small cell is adjacent to a
  // larger cell, the shared edges can fail to line up exactly.  Two cell edges
  // cannot come closer then kMinWidth, so if we have the polygon builder merge
  // edges within half that distance, we should always merge shared edges
  // without merging different edges.
  S2PolygonBuilderOptions options(S2PolygonBuilderOptions::DIRECTED_XOR());
  double min_cell_angle = S2::kMinWidth.GetValue(S2CellId::kMaxLevel);
  options.set_vertex_merge_radius(S1Angle::Radians(min_cell_angle / 2));
  S2PolygonBuilder builder(options);
  for (int i = 0; i < cells.num_cells(); ++i) {
    S2Loop cell_loop(S2Cell(cells.cell_id(i)));
    builder.AddLoop(&cell_loop);
  }
  if (!builder.AssemblePolygon(this, NULL)) {
    S2LOG(DFATAL) << "AssemblePolygon failed in InitToCellUnionBorder";
  }
}

bool S2Polygon::IsNormalized(string* err) const {
  set<S2Point> vertices;
  S2Loop* last_parent = NULL;
  for (int i = 0; i < num_loops(); ++i) {
    S2Loop* child = loop(i);
    if (child->depth() == 0) continue;
    S2Loop* parent = loop(GetParent(i));
    if (parent != last_parent) {
      vertices.clear();
      for (int j = 0; j < parent->num_vertices(); ++j) {
        vertices.insert(parent->vertex(j));
      }
      last_parent = parent;
    }
    int count = 0;
    for (int j = 0; j < child->num_vertices(); ++j) {
      if (vertices.count(child->vertex(j)) > 0) ++count;
    }
    if (count > 1) {
      if (err) {
        *err = stream() << "Loop " << i << " shares more than one vertex"
                        << " with its parent loop " << GetParent(i);
      }
      return false;
    }
  }
  return true;
}

bool S2Polygon::BoundaryEquals(S2Polygon const* b) const {
  if (num_loops() != b->num_loops()) return false;

  for (int i = 0; i < num_loops(); ++i) {
    S2Loop* a_loop = loop(i);
    bool success = false;
    for (int j = 0; j < num_loops(); ++j) {
      S2Loop* b_loop = b->loop(j);
      if ((b_loop->depth() == a_loop->depth()) &&
          b_loop->BoundaryEquals(a_loop)) {
        success = true;
        break;
      }
    }
    if (!success) return false;
  }
  return true;
}

bool S2Polygon::BoundaryApproxEquals(S2Polygon const* b,
                                     double max_error) const {
  if (num_loops() != b->num_loops()) return false;

  // For now, we assume that there is at most one candidate match for each
  // loop.  (So far this method is just used for testing.)

  for (int i = 0; i < num_loops(); ++i) {
    S2Loop* a_loop = loop(i);
    bool success = false;
    for (int j = 0; j < num_loops(); ++j) {
      S2Loop* b_loop = b->loop(j);
      if (b_loop->depth() == a_loop->depth() &&
          b_loop->BoundaryApproxEquals(a_loop, max_error)) {
        success = true;
        break;
      }
    }
    if (!success) return false;
  }
  return true;
}

bool S2Polygon::BoundaryNear(S2Polygon const* b, double max_error) const {
  if (num_loops() != b->num_loops()) return false;

  // For now, we assume that there is at most one candidate match for each
  // loop.  (So far this method is just used for testing.)

  for (int i = 0; i < num_loops(); ++i) {
    S2Loop* a_loop = loop(i);
    bool success = false;
    for (int j = 0; j < num_loops(); ++j) {
      S2Loop* b_loop = b->loop(j);
      if (b_loop->depth() == a_loop->depth() &&
          b_loop->BoundaryNear(a_loop, max_error)) {
        success = true;
        break;
      }
    }
    if (!success) return false;
  }
  return true;
}

S2Point S2Polygon::Project(S2Point const& point) const {
  DCHECK(!loops_.empty());

  if (Contains(point)) {
    return point;
  }

  S1Angle min_distance = S1Angle::Radians(10);
  int min_loop_index = 0;
  int min_vertex_index = 0;

  for (int l = 0; l < num_loops(); ++l) {
    S2Loop *a_loop = loop(l);
    for (int v = 0; v < a_loop->num_vertices(); ++v) {
      S1Angle distance_to_segment =
          S2EdgeUtil::GetDistance(point,
                                  a_loop->vertex(v),
                                  a_loop->vertex(v + 1));
      if (distance_to_segment < min_distance) {
        min_distance = distance_to_segment;
        min_loop_index = l;
        min_vertex_index = v;
      }
    }
  }

  S2Point closest_point = S2EdgeUtil::GetClosestPoint(
      point,
      loop(min_loop_index)->vertex(min_vertex_index),
      loop(min_loop_index)->vertex(min_vertex_index + 1));

  return closest_point;
}
