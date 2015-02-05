// Copyright 2005 Google Inc. All Rights Reserved.

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <set>
using std::set;
using std::multiset;

#include <vector>
using std::vector;

#include "s2.h"
#include "hash.h"

#include <utility>
using std::pair;
using std::make_pair;

#include "s2loop.h"

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "util/coding/coder.h"
#include "s2cap.h"
#include "s2cell.h"
#include "s2edgeindex.h"

#include "mongo/util/mongoutils/str.h"
using mongoutils::str::stream;

static const unsigned char kCurrentEncodingVersionNumber = 1;

namespace {
  stream& operator<<(stream& strStream, const S1Angle& angle) {
    std::stringstream ss;
    ss << angle;
    return strStream << ss.str();
  }
  // Reverse the output order of Lat/Lng to Lng/Lat
  stream& operator<<(stream& strStream, const S2LatLng& ll) {
      return strStream << "[" << ll.lng() << ", " << ll.lat() << "]";
  }
}

S2Point const* S2LoopIndex::edge_from(int index) const {
  return &loop_->vertex(index);
}

S2Point const* S2LoopIndex::edge_to(int index) const {
  return &loop_->vertex(index+1);
}

int S2LoopIndex::num_edges() const {
  return loop_->num_vertices();
}

S2Loop::S2Loop()
  : num_vertices_(0),
    vertices_(NULL),
    owns_vertices_(false),
    bound_(S2LatLngRect::Empty()),
    depth_(0),
    index_(this),
    num_find_vertex_calls_(0) {
}

S2Loop::S2Loop(vector<S2Point> const& vertices)
  : num_vertices_(0),
    vertices_(NULL),
    owns_vertices_(false),
    bound_(S2LatLngRect::Full()),
    depth_(0),
    index_(this),
    num_find_vertex_calls_(0) {
  Init(vertices);
}

void S2Loop::ResetMutableFields() {
  index_.Reset();
  num_find_vertex_calls_ = 0;
  vertex_to_index_.clear();
}

void S2Loop::Init(vector<S2Point> const& vertices) {
  ResetMutableFields();

  if (owns_vertices_) delete[] vertices_;
  num_vertices_ = vertices.size();
  if (vertices.empty()) {
    vertices_ = NULL;
  } else {
    vertices_ = new S2Point[num_vertices_];
    memcpy(vertices_, &vertices[0], num_vertices_ * sizeof(vertices_[0]));
  }
  owns_vertices_ = true;
  bound_ = S2LatLngRect::Full();

  // InitOrigin() must be called before InitBound() because the latter
  // function expects Contains() to work properly.
  InitOrigin();
  InitBound();
}

bool S2Loop::IsValid(string* err) const {
  // Loops must have at least 3 vertices.
  if (num_vertices() < 3) {
    VLOG(2) << "Degenerate loop";
    if (err) *err = "Degenerate loop";
    return false;
  }
  // All vertices must be unit length.
  for (int i = 0; i < num_vertices(); ++i) {
    if (!S2::IsUnitLength(vertex(i))) {
      VLOG(2) << "Vertex " << i << " is not unit length";
      if (err) *err = stream() << "Vertex " << i << " is not unit length";
      return false;
    }
  }
  // Loops are not allowed to have any duplicate vertices.
  hash_map<S2Point, int> vmap;
  for (int i = 0; i < num_vertices(); ++i) {
    if (!vmap.insert(make_pair(vertex(i), i)).second) {
      VLOG(2) << "Duplicate vertices: " << vmap[vertex(i)] << " and " << i;
      if (err) *err = stream() << "Duplicate vertices: " << vmap[vertex(i)] << " and " << i;
      return false;
    }
  }
  // Non-adjacent edges are not allowed to intersect.
  bool crosses = false;
  index_.PredictAdditionalCalls(num_vertices());
  S2EdgeIndex::Iterator it(&index_);
  for (int i = 0; i < num_vertices(); ++i) {
    S2EdgeUtil::EdgeCrosser crosser(&vertex(i), &vertex(i+1), &vertex(0));
    int previous_index = -2;
    for (it.GetCandidates(vertex(i), vertex(i+1)); !it.Done(); it.Next()) {
      int ai = it.Index();
      // There is no need to test the same thing twice.  Moreover, two edges
      // that abut at ai+1 will have been tested for equality above.
      if (ai > i+1) {
        if (previous_index != ai) crosser.RestartAt(&vertex(ai));
        // Beware, this may return the loop is valid if there is a
        // "vertex crossing".
        // TODO(user): Fix that.
        crosses = crosser.RobustCrossing(&vertex(ai+1)) > 0;
        previous_index = ai + 1;
        if (crosses) {
          VLOG(2) << "Edges " << i << " and " << ai << " cross";
          // additional debugging information, reverse Lat/Lng order.
          string errDetail = stream()
             << "Edge locations in degrees: "
             << S2LatLng(vertex(i)) << "-" << S2LatLng(vertex(i + 1))
             << " and "
             << S2LatLng(vertex(ai)) << "-" << S2LatLng(vertex(ai + 1));
          VLOG(2) << errDetail;
          if (NULL != err) {
            *err = stream()
               << "Edges " << i << " and " << ai << " cross. " << errDetail;
          }
          break;
        }
      }
    }
    if (crosses) break;
  }

  return !crosses;
}

bool S2Loop::IsValid(vector<S2Point> const& vertices, int max_adjacent) {
  if (vertices.size() < 3) return false;

  S2Loop loop(vertices);
  return loop.IsValid();
}

bool S2Loop::IsValid(int max_adjacent) const {
  return IsValid();
}

void S2Loop::InitOrigin() {
  // The bounding box does not need to be correct before calling this
  // function, but it must at least contain vertex(1) since we need to
  // do a Contains() test on this point below.
  DCHECK(bound_.Contains(vertex(1)));

  // To ensure that every point is contained in exactly one face of a
  // subdivision of the sphere, all containment tests are done by counting the
  // edge crossings starting at a fixed point on the sphere (S2::Origin()).
  // We need to know whether this point is inside or outside of the loop.
  // We do this by first guessing that it is outside, and then seeing whether
  // we get the correct containment result for vertex 1.  If the result is
  // incorrect, the origin must be inside the loop.
  //
  // A loop with consecutive vertices A,B,C contains vertex B if and only if
  // the fixed vector R = S2::Ortho(B) is on the left side of the wedge ABC.
  // The test below is written so that B is inside if C=R but not if A=R.

  origin_inside_ = false;  // Initialize before calling Contains().
  bool v1_inside = S2::OrderedCCW(S2::Ortho(vertex(1)), vertex(0), vertex(2),
                                  vertex(1));
  if (v1_inside != Contains(vertex(1)))
    origin_inside_ = true;
}

void S2Loop::InitBound() {
  // The bounding rectangle of a loop is not necessarily the same as the
  // bounding rectangle of its vertices.  First, the loop may wrap entirely
  // around the sphere (e.g. a loop that defines two revolutions of a
  // candy-cane stripe).  Second, the loop may include one or both poles.
  // Note that a small clockwise loop near the equator contains both poles.

  S2EdgeUtil::RectBounder bounder;
  for (int i = 0; i <= num_vertices(); ++i) {
    bounder.AddPoint(&vertex(i));
  }
  S2LatLngRect b = bounder.GetBound();
  // Note that we need to initialize bound_ with a temporary value since
  // Contains() does a bounding rectangle check before doing anything else.
  bound_ = S2LatLngRect::Full();
  if (Contains(S2Point(0, 0, 1))) {
    b = S2LatLngRect(R1Interval(b.lat().lo(), M_PI_2), S1Interval::Full());
  }
  // If a loop contains the south pole, then either it wraps entirely
  // around the sphere (full longitude range), or it also contains the
  // north pole in which case b.lng().is_full() due to the test above.
  // Either way, we only need to do the south pole containment test if
  // b.lng().is_full().
  if (b.lng().is_full() && Contains(S2Point(0, 0, -1))) {
    b.mutable_lat()->set_lo(-M_PI_2);
  }
  bound_ = b;
}

S2Loop::S2Loop(S2Cell const& cell)
    : bound_(cell.GetRectBound()),
      index_(this),
      num_find_vertex_calls_(0) {
  num_vertices_ = 4;
  vertices_ = new S2Point[num_vertices_];
  depth_ = 0;
  for (int i = 0; i < 4; ++i) {
    vertices_[i] = cell.GetVertex(i);
  }
  owns_vertices_ = true;
  InitOrigin();
  InitBound();
}

S2Loop::~S2Loop() {
  if (owns_vertices_) {
    delete[] vertices_;
  }
}

S2Loop::S2Loop(S2Loop const* src)
  : num_vertices_(src->num_vertices_),
    vertices_(new S2Point[num_vertices_]),
    owns_vertices_(true),
    bound_(src->bound_),
    origin_inside_(src->origin_inside_),
    depth_(src->depth_),
    index_(this),
    num_find_vertex_calls_(0) {
  memcpy(vertices_, src->vertices_, num_vertices_ * sizeof(vertices_[0]));
}

S2Loop* S2Loop::Clone() const {
  return new S2Loop(this);
}

int S2Loop::FindVertex(S2Point const& p) const {
  num_find_vertex_calls_++;
  if (num_vertices() < 10 || num_find_vertex_calls_ < 20) {
    // Exhaustive search
    for (int i = 1; i <= num_vertices(); ++i) {
      if (vertex(i) == p) return i;
    }
    return -1;
  }

  if (vertex_to_index_.empty()) {  // We haven't computed it yet.
    for (int i = num_vertices(); i > 0; --i) {
      vertex_to_index_[vertex(i)] = i;
    }
  }

  map<S2Point, int>::const_iterator it;
  it = vertex_to_index_.find(p);
  if (it == vertex_to_index_.end()) return -1;
  return it->second;
}


bool S2Loop::IsNormalized() const {
  // Optimization: if the longitude span is less than 180 degrees, then the
  // loop covers less than half the sphere and is therefore normalized.
  if (bound_.lng().GetLength() < M_PI) return true;

  // We allow some error so that hemispheres are always considered normalized.
  // TODO(user): This might not be necessary once S2Polygon is enhanced so
  // that it does not require its input loops to be normalized.
  return GetTurningAngle() >= -1e-14;
}

void S2Loop::Normalize() {
  CHECK(owns_vertices_);
  if (!IsNormalized()) Invert();
  DCHECK(IsNormalized());
}

void S2Loop::Invert() {
  CHECK(owns_vertices_);

  ResetMutableFields();
  reverse(vertices_, vertices_ + num_vertices());
  origin_inside_ ^= true;
  if (bound_.lat().lo() > -M_PI_2 && bound_.lat().hi() < M_PI_2) {
    // The complement of this loop contains both poles.
    bound_ = S2LatLngRect::Full();
  } else {
    InitBound();
  }
}

double S2Loop::GetArea() const {
  double area = GetSurfaceIntegral(S2::SignedArea);
  // The signed area should be between approximately -4*Pi and 4*Pi.
  DCHECK_LE(fabs(area), 4 * M_PI + 1e-12);
  if (area < 0) {
    // We have computed the negative of the area of the loop exterior.
    area += 4 * M_PI;
  }
  return max(0.0, min(4 * M_PI, area));
}

S2Point S2Loop::GetCentroid() const {
  // GetSurfaceIntegral() returns either the integral of position over loop
  // interior, or the negative of the integral of position over the loop
  // exterior.  But these two values are the same (!), because the integral of
  // position over the entire sphere is (0, 0, 0).
  return GetSurfaceIntegral(S2::TrueCentroid);
}

// Return (first, dir) such that first..first+n*dir are valid indices.
int S2Loop::GetCanonicalFirstVertex(int* dir) const {
  int first = 0;
  int n = num_vertices();
  for (int i = 1; i < n; ++i) {
    if (vertex(i) < vertex(first)) first = i;
  }
  if (vertex(first + 1) < vertex(first + n - 1)) {
    *dir = 1;
    // 0 <= first <= n-1, so (first+n*dir) <= 2*n-1.
  } else {
    *dir = -1;
    first += n;
    // n <= first <= 2*n-1, so (first+n*dir) >= 0.
  }
  return first;
}

double S2Loop::GetTurningAngle() const {
  // Don't crash even if the loop is not well-defined.
  if (num_vertices() < 3) return 0;

  // To ensure that we get the same result when the loop vertex order is
  // rotated, and that we get the same result with the opposite sign when the
  // vertices are reversed, we need to be careful to add up the individual
  // turn angles in a consistent order.  In general, adding up a set of
  // numbers in a different order can change the sum due to rounding errors.
  int n = num_vertices();
  int dir, i = GetCanonicalFirstVertex(&dir);
  double angle = S2::TurnAngle(vertex((i + n - dir) % n), vertex(i),
                               vertex((i + dir) % n));
  while (--n > 0) {
    i += dir;
    angle += S2::TurnAngle(vertex(i - dir), vertex(i), vertex(i + dir));
  }
  return dir * angle;
}

S2Cap S2Loop::GetCapBound() const {
  return bound_.GetCapBound();
}

bool S2Loop::Contains(S2Cell const& cell) const {
  // A future optimization could also take advantage of the fact than an S2Cell
  // is convex.

  // It's not necessarily true that bound_.Contains(cell.GetRectBound())
  // because S2Cell bounds are slightly conservative.
  if (!bound_.Contains(cell.GetCenter())) return false;
  S2Loop cell_loop(cell);
  return Contains(&cell_loop);
}

bool S2Loop::MayIntersect(S2Cell const& cell) const {
  // It is faster to construct a bounding rectangle for an S2Cell than for
  // a general polygon.  A future optimization could also take advantage of
  // the fact than an S2Cell is convex.

  if (!bound_.Intersects(cell.GetRectBound())) return false;
  return S2Loop(cell).Intersects(this);
}

bool S2Loop::Contains(S2Point const& p) const {
  if (!bound_.Contains(p)) return false;

  bool inside = origin_inside_;
  S2Point origin = S2::Origin();
  S2EdgeUtil::EdgeCrosser crosser(&origin, &p, &vertex(0));

  // The s2edgeindex library is not optimized yet for long edges,
  // so the tradeoff to using it comes later.
  if (num_vertices() < 2000) {
    for (int i = 1; i <= num_vertices(); ++i) {
      inside ^= crosser.EdgeOrVertexCrossing(&vertex(i));
    }
    return inside;
  }

  S2EdgeIndex::Iterator it(&index_);
  int previous_index = -2;
  for (it.GetCandidates(origin, p); !it.Done(); it.Next()) {
    int ai = it.Index();
    if (previous_index != ai - 1) crosser.RestartAt(&vertex(ai));
    previous_index = ai;
    inside ^= crosser.EdgeOrVertexCrossing(&vertex(ai+1));
  }
  return inside;
}

void S2Loop::Encode(Encoder* const encoder) const {
  encoder->Ensure(num_vertices_ * sizeof(*vertices_) + 20);  // sufficient

  encoder->put8(kCurrentEncodingVersionNumber);
  encoder->put32(num_vertices_);
  encoder->putn(vertices_, sizeof(*vertices_) * num_vertices_);
  encoder->put8(origin_inside_);
  encoder->put32(depth_);
  DCHECK_GE(encoder->avail(), 0);

  bound_.Encode(encoder);
}

bool S2Loop::Decode(Decoder* const decoder) {
  return DecodeInternal(decoder, false);
}

bool S2Loop::DecodeWithinScope(Decoder* const decoder) {
  return DecodeInternal(decoder, true);
}

bool S2Loop::DecodeInternal(Decoder* const decoder,
                            bool within_scope) {
  unsigned char version = decoder->get8();
  if (version > kCurrentEncodingVersionNumber) return false;

  num_vertices_ = decoder->get32();
  if (owns_vertices_) delete[] vertices_;
  if (within_scope) {
    vertices_ = const_cast<S2Point *>(reinterpret_cast<S2Point const*>(
                    decoder->ptr()));
    decoder->skip(num_vertices_ * sizeof(*vertices_));
    owns_vertices_ = false;
  } else {
    vertices_ = new S2Point[num_vertices_];
    decoder->getn(vertices_, num_vertices_ * sizeof(*vertices_));
    owns_vertices_ = true;
  }
  origin_inside_ = decoder->get8();
  depth_ = decoder->get32();
  if (!bound_.Decode(decoder)) return false;

  DCHECK(IsValid());

  return decoder->avail() >= 0;
}

// This is a helper class for the AreBoundariesCrossing function.
// Each time there is a point in common between the two loops passed
// as parameters, the two associated wedges centered at this point are
// passed to the ProcessWedge function of this processor. The function
// updates an internal state based on the wedges, and returns true to
// signal that no further processing is needed.
//
// To use AreBoundariesCrossing, subclass this class and keep an
// internal state that you update each time ProcessWedge is called,
// then query this internal state in the function that called
// AreBoundariesCrossing.
class WedgeProcessor {
 public:
  virtual ~WedgeProcessor() { }

  virtual bool ProcessWedge(S2Point const& a0, S2Point const& ab1,
                            S2Point const& a2, S2Point const& b0,
                            S2Point const& b2) = 0;
};

bool S2Loop::AreBoundariesCrossing(
    S2Loop const* b, WedgeProcessor* wedge_processor) const {
  // See the header file for a description of what this method does.
  index_.PredictAdditionalCalls(b->num_vertices());
  S2EdgeIndex::Iterator it(&index_);
  for (int j = 0; j < b->num_vertices(); ++j) {
    S2EdgeUtil::EdgeCrosser crosser(&b->vertex(j), &b->vertex(j+1),
                                    &b->vertex(0));
    int previous_index = -2;
    for (it.GetCandidates(b->vertex(j), b->vertex(j+1));
         !it.Done(); it.Next()) {
      int ai = it.Index();
      if (previous_index != ai - 1) crosser.RestartAt(&vertex(ai));
      previous_index = ai;
      int crossing = crosser.RobustCrossing(&vertex(ai + 1));
      if (crossing < 0) continue;
      if (crossing > 0) return true;
      // We only need to check each shared vertex once, so we only
      // consider the case where vertex(i+1) == b->vertex(j+1).
      if (vertex(ai+1) == b->vertex(j+1) &&
          wedge_processor->ProcessWedge(vertex(ai), vertex(ai+1), vertex(ai+2),
                                        b->vertex(j), b->vertex(j+2))) {
        return false;
      }
    }
  }
  return false;
}

// WedgeProcessor to be used to check if loop A contains loop B.
// DoesntContain() then returns true if there is a wedge of B not
// contained in the associated wedge of A (and hence loop B is not
// contained in loop A).
class ContainsWedgeProcessor: public WedgeProcessor {
 public:
  ContainsWedgeProcessor(): doesnt_contain_(false) {}
  bool DoesntContain() { return doesnt_contain_; }

 protected:
  virtual bool ProcessWedge(S2Point const& a0, S2Point const& ab1,
                            S2Point const& a2, S2Point const& b0,
                            S2Point const& b2) {
    doesnt_contain_ = !S2EdgeUtil::WedgeContains(a0, ab1, a2, b0, b2);
    return doesnt_contain_;
  }

 private:
  bool doesnt_contain_;
};

bool S2Loop::Contains(S2Loop const* b) const {
  // For this loop A to contains the given loop B, all of the following must
  // be true:
  //
  //  (1) There are no edge crossings between A and B except at vertices.
  //
  //  (2) At every vertex that is shared between A and B, the local edge
  //      ordering implies that A contains B.
  //
  //  (3) If there are no shared vertices, then A must contain a vertex of B
  //      and B must not contain a vertex of A.  (An arbitrary vertex may be
  //      chosen in each case.)
  //
  // The second part of (3) is necessary to detect the case of two loops whose
  // union is the entire sphere, i.e. two loops that contains each other's
  // boundaries but not each other's interiors.

  if (!bound_.Contains(b->bound_)) return false;

  // Unless there are shared vertices, we need to check whether A contains a
  // vertex of B.  Since shared vertices are rare, it is more efficient to do
  // this test up front as a quick rejection test.
  if (!Contains(b->vertex(0)) && FindVertex(b->vertex(0)) < 0)
    return false;

  // Now check whether there are any edge crossings, and also check the loop
  // relationship at any shared vertices.
  ContainsWedgeProcessor p_wedge;
  if (AreBoundariesCrossing(b, &p_wedge) || p_wedge.DoesntContain()) {
    return false;
  }

  // At this point we know that the boundaries of A and B do not intersect,
  // and that A contains a vertex of B.  However we still need to check for
  // the case mentioned above, where (A union B) is the entire sphere.
  // Normally this check is very cheap due to the bounding box precondition.
  if (bound_.Union(b->bound_).is_full()) {
    if (b->Contains(vertex(0)) && b->FindVertex(vertex(0)) < 0) return false;
  }
  return true;
}

// WedgeProcessor to be used to check if loop A intersects loop B.
// Intersects() then returns true when A and B have at least one pair
// of associated wedges that intersect.
class IntersectsWedgeProcessor: public WedgeProcessor {
 public:
  IntersectsWedgeProcessor(): intersects_(false) {}
  bool Intersects() { return intersects_; }

 protected:
  virtual bool ProcessWedge(S2Point const& a0, S2Point const& ab1,
                            S2Point const& a2, S2Point const& b0,
                            S2Point const& b2) {
    intersects_ = S2EdgeUtil::WedgeIntersects(a0, ab1, a2, b0, b2);
    return intersects_;
  }

 private:
  bool intersects_;
};

bool S2Loop::Intersects(S2Loop const* b) const {
  // a->Intersects(b) if and only if !a->Complement()->Contains(b).
  // This code is similar to Contains(), but is optimized for the case
  // where both loops enclose less than half of the sphere.

  // The largest of the two loops should be edgeindex'd.
  if (b->num_vertices() > num_vertices()) return b->Intersects(this);

  if (!bound_.Intersects(b->bound_)) return false;

  // Unless there are shared vertices, we need to check whether A contains a
  // vertex of B.  Since shared vertices are rare, it is more efficient to do
  // this test up front as a quick acceptance test.
  if (Contains(b->vertex(0)) && FindVertex(b->vertex(0)) < 0)
    return true;

  // Now check whether there are any edge crossings, and also check the loop
  // relationship at any shared vertices.
  IntersectsWedgeProcessor p_wedge;
  if (AreBoundariesCrossing(b, &p_wedge) || p_wedge.Intersects()) {
    return true;
  }

  // We know that A does not contain a vertex of B, and that there are no edge
  // crossings.  Therefore the only way that A can intersect B is if B
  // entirely contains A.  We can check this by testing whether B contains an
  // arbitrary non-shared vertex of A.  Note that this check is usually cheap
  // because of the bounding box precondition.
  if (b->bound_.Contains(bound_)) {
    if (b->Contains(vertex(0)) && b->FindVertex(vertex(0)) < 0) return true;
  }
  return false;
}

// WedgeProcessor to be used to check if the interior of loop A
// contains the interior of loop B, or their boundaries cross each
// other (therefore they have a proper intersection).
// CrossesOrMayContain() then returns -1 if A crossed B, 0 if it is
// not possible for A to contain B, and 1 otherwise.
class ContainsOrCrossesProcessor: public WedgeProcessor {
 public:
  ContainsOrCrossesProcessor():
      has_boundary_crossing_(false),
      a_has_strictly_super_wedge_(false), b_has_strictly_super_wedge_(false),
      has_disjoint_wedge_(false) {}

  int CrossesOrMayContain() {
    if (has_boundary_crossing_) return -1;
    if (has_disjoint_wedge_ || b_has_strictly_super_wedge_) return 0;
    return 1;
  }

 protected:
  virtual bool ProcessWedge(S2Point const& a0, S2Point const& ab1,
                            S2Point const& a2, S2Point const& b0,
                            S2Point const& b2) {
    const S2EdgeUtil::WedgeRelation wedge_relation =
        S2EdgeUtil::GetWedgeRelation(a0, ab1, a2, b0, b2);
    if (wedge_relation == S2EdgeUtil::WEDGE_PROPERLY_OVERLAPS) {
      has_boundary_crossing_ = true;
      return true;
    }

    a_has_strictly_super_wedge_ |=
        (wedge_relation == S2EdgeUtil::WEDGE_PROPERLY_CONTAINS);
    b_has_strictly_super_wedge_ |=
        (wedge_relation == S2EdgeUtil::WEDGE_IS_PROPERLY_CONTAINED);
    if (a_has_strictly_super_wedge_ && b_has_strictly_super_wedge_) {
      has_boundary_crossing_ = true;
      return true;
    }

    has_disjoint_wedge_ |= (wedge_relation == S2EdgeUtil::WEDGE_IS_DISJOINT);
    return false;
  }

 private:
  // True if any crossing on the boundary is discovered.
  bool has_boundary_crossing_;
  // True if A (B) has a strictly superwedge on a pair of wedges that
  // share a common center point.
  bool a_has_strictly_super_wedge_;
  bool b_has_strictly_super_wedge_;
  // True if there is a pair of disjoint wedges with common center
  // point.
  bool has_disjoint_wedge_;
};

int S2Loop::ContainsOrCrosses(S2Loop const* b) const {
  // There can be containment or crossing only if the bounds intersect.
  if (!bound_.Intersects(b->bound_)) return 0;

  // Now check whether there are any edge crossings, and also check the loop
  // relationship at any shared vertices.  Note that unlike Contains() or
  // Intersects(), we can't do a point containment test as a shortcut because
  // we need to detect whether there are any edge crossings.
  ContainsOrCrossesProcessor p_wedge;
  if (AreBoundariesCrossing(b, &p_wedge)) {
    return -1;
  }
  const int result = p_wedge.CrossesOrMayContain();
  if (result <= 0) return result;

  // At this point we know that the boundaries do not intersect, and we are
  // given that (A union B) is a proper subset of the sphere.  Furthermore
  // either A contains B, or there are no shared vertices (due to the check
  // above).  So now we just need to distinguish the case where A contains B
  // from the case where B contains A or the two loops are disjoint.
  if (!bound_.Contains(b->bound_)) return 0;
  if (!Contains(b->vertex(0)) && FindVertex(b->vertex(0)) < 0) return 0;
  return 1;
}

bool S2Loop::ContainsNested(S2Loop const* b) const {
  if (!bound_.Contains(b->bound_)) return false;

  // We are given that A and B do not share any edges, and that either one
  // loop contains the other or they do not intersect.
  int m = FindVertex(b->vertex(1));
  if (m < 0) {
    // Since b->vertex(1) is not shared, we can check whether A contains it.
    return Contains(b->vertex(1));
  }
  // Check whether the edge order around b->vertex(1) is compatible with
  // A containing B.
  return S2EdgeUtil::WedgeContains(vertex(m-1), vertex(m), vertex(m+1),
                                   b->vertex(0), b->vertex(2));
}

bool S2Loop::BoundaryEquals(S2Loop const* b) const {
  if (num_vertices() != b->num_vertices()) return false;
  for (int offset = 0; offset < num_vertices(); ++offset) {
    if (vertex(offset) == b->vertex(0)) {
      // There is at most one starting offset since loop vertices are unique.
      for (int i = 0; i < num_vertices(); ++i) {
        if (vertex(i + offset) != b->vertex(i)) return false;
      }
      return true;
    }
  }
  return false;
}

bool S2Loop::BoundaryApproxEquals(S2Loop const* b, double max_error) const {
  if (num_vertices() != b->num_vertices()) return false;
  for (int offset = 0; offset < num_vertices(); ++offset) {
    if (S2::ApproxEquals(vertex(offset), b->vertex(0), max_error)) {
      bool success = true;
      for (int i = 0; i < num_vertices(); ++i) {
        if (!S2::ApproxEquals(vertex(i + offset), b->vertex(i), max_error)) {
          success = false;
          break;
        }
      }
      if (success) return true;
      // Otherwise continue looping.  There may be more than one candidate
      // starting offset since vertices are only matched approximately.
    }
  }
  return false;
}

static bool MatchBoundaries(S2Loop const* a, S2Loop const* b, int a_offset,
                            double max_error) {
  // The state consists of a pair (i,j).  A state transition consists of
  // incrementing either "i" or "j".  "i" can be incremented only if
  // a(i+1+a_offset) is near the edge from b(j) to b(j+1), and a similar rule
  // applies to "j".  The function returns true iff we can proceed all the way
  // around both loops in this way.
  //
  // Note that when "i" and "j" can both be incremented, sometimes only one
  // choice leads to a solution.  We handle this using a stack and
  // backtracking.  We also keep track of which states have already been
  // explored to avoid duplicating work.

  vector<pair<int, int> > pending;
  set<pair<int, int> > done;
  pending.push_back(make_pair(0, 0));
  while (!pending.empty()) {
    int i = pending.back().first;
    int j = pending.back().second;
    pending.pop_back();
    if (i == a->num_vertices() && j == b->num_vertices()) {
      return true;
    }
    done.insert(make_pair(i, j));

    // If (i == na && offset == na-1) where na == a->num_vertices(), then
    // then (i+1+offset) overflows the [0, 2*na-1] range allowed by vertex().
    // So we reduce the range if necessary.
    int io = i + a_offset;
    if (io >= a->num_vertices()) io -= a->num_vertices();

    if (i < a->num_vertices() && done.count(make_pair(i+1, j)) == 0 &&
        S2EdgeUtil::GetDistance(a->vertex(io+1),
                                b->vertex(j),
                                b->vertex(j+1)).radians() <= max_error) {
      pending.push_back(make_pair(i+1, j));
    }
    if (j < b->num_vertices() && done.count(make_pair(i, j+1)) == 0 &&
        S2EdgeUtil::GetDistance(b->vertex(j+1),
                                a->vertex(io),
                                a->vertex(io+1)).radians() <= max_error) {
      pending.push_back(make_pair(i, j+1));
    }
  }
  return false;
}

bool S2Loop::BoundaryNear(S2Loop const* b, double max_error) const {
  for (int a_offset = 0; a_offset < num_vertices(); ++a_offset) {
    if (MatchBoundaries(this, b, a_offset, max_error)) return true;
  }
  return false;
}
