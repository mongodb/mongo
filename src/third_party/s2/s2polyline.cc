// Copyright 2005 Google Inc. All Rights Reserved.

#include <set>
using std::set;
using std::multiset;

#include <vector>
using std::vector;

#include "s2.h"
#include "base/logging.h"
#include "util/math/matrix3x3-inl.h"
#include "s2polyline.h"

#include "util/coding/coder.h"
#include "s2cap.h"
#include "s2cell.h"
#include "s2latlng.h"
#include "s2edgeutil.h"

#include "mongo/util/mongoutils/str.h"
using mongoutils::str::stream;

static const unsigned char kCurrentEncodingVersionNumber = 1;

S2Polyline::S2Polyline()
  : num_vertices_(0),
    vertices_(NULL) {
}

S2Polyline::S2Polyline(vector<S2Point> const& vertices)
  : num_vertices_(0),
    vertices_(NULL) {
  Init(vertices);
}

S2Polyline::S2Polyline(vector<S2LatLng> const& vertices)
  : num_vertices_(0),
    vertices_(NULL) {
  Init(vertices);
}

S2Polyline::~S2Polyline() {
  delete[] vertices_;
}

void S2Polyline::Init(vector<S2Point> const& vertices) {
  if (S2::debug) {
      CHECK(IsValid(vertices));
  }

  delete[] vertices_;
  num_vertices_ = vertices.size();
  vertices_ = new S2Point[num_vertices_];
  // Check (num_vertices_ > 0) to avoid invalid reference to vertices[0].
  if (num_vertices_ > 0) {
    memcpy(vertices_, &vertices[0], num_vertices_ * sizeof(vertices_[0]));
  }
}

void S2Polyline::Init(vector<S2LatLng> const& vertices) {
  delete[] vertices_;
  num_vertices_ = vertices.size();
  vertices_ = new S2Point[num_vertices_];
  for (int i = 0; i < num_vertices_; ++i) {
    vertices_[i] = vertices[i].ToPoint();
  }
  if (S2::debug) {
    vector<S2Point> vertex_vector(vertices_, vertices_ + num_vertices_);
    CHECK(IsValid(vertex_vector));
  }
}

bool S2Polyline::IsValid(vector<S2Point> const& v, string* err) {
  // All vertices must be unit length.
  int n = v.size();
  for (int i = 0; i < n; ++i) {
    if (!S2::IsUnitLength(v[i])) {
      S2LOG(INFO) << "Vertex " << i << " is not unit length";
      if (err) {
        *err = stream() << "Vertex " << i << " is not unit length";
      }
      return false;
    }
  }

  // Adjacent vertices must not be identical or antipodal.
  for (int i = 1; i < n; ++i) {
    if (v[i-1] == v[i] || v[i-1] == -v[i]) {
      S2LOG(INFO) << "Vertices " << (i - 1) << " and " << i
                << " are identical or antipodal";
      if (err) {
        *err = stream() << "Vertices " << (i - 1) << " and " << i
                        << " are identical or antipodal";
      }
      return false;
    }
  }
  return true;
}


S2Polyline::S2Polyline(S2Polyline const* src)
  : num_vertices_(src->num_vertices_),
    vertices_(new S2Point[num_vertices_]) {
  memcpy(vertices_, src->vertices_, num_vertices_ * sizeof(vertices_[0]));
}

S2Polyline* S2Polyline::Clone() const {
  return new S2Polyline(this);
}

S1Angle S2Polyline::GetLength() const {
  S1Angle length;
  for (int i = 1; i < num_vertices(); ++i) {
    length += S1Angle(vertex(i-1), vertex(i));
  }
  return length;
}

S2Point S2Polyline::GetCentroid() const {
  S2Point centroid;
  for (int i = 1; i < num_vertices(); ++i) {
    // The centroid (multiplied by length) is a vector toward the midpoint
    // of the edge, whose length is twice the sin of half the angle between
    // the two vertices.  Defining theta to be this angle, we have:
    S2Point vsum = vertex(i-1) + vertex(i);    // Length == 2*cos(theta)
    S2Point vdiff = vertex(i-1) - vertex(i);   // Length == 2*sin(theta)
    double cos2 = vsum.Norm2();
    double sin2 = vdiff.Norm2();
    DCHECK_GT(cos2, 0);  // Otherwise edge is undefined, and result is NaN.
    centroid += sqrt(sin2 / cos2) * vsum;  // Length == 2*sin(theta)
  }
  return centroid;
}

S2Point S2Polyline::GetSuffix(double fraction, int* next_vertex) const {
  DCHECK_GT(num_vertices(), 0);
  // We intentionally let the (fraction >= 1) case fall through, since
  // we need to handle it in the loop below in any case because of
  // possible roundoff errors.
  if (fraction <= 0) {
    *next_vertex = 1;
    return vertex(0);
  }
  S1Angle length_sum;
  for (int i = 1; i < num_vertices(); ++i) {
    length_sum += S1Angle(vertex(i-1), vertex(i));
  }
  S1Angle target = fraction * length_sum;
  for (int i = 1; i < num_vertices(); ++i) {
    S1Angle length(vertex(i-1), vertex(i));
    if (target < length) {
      // This interpolates with respect to arc length rather than
      // straight-line distance, and produces a unit-length result.
      S2Point result = S2EdgeUtil::InterpolateAtDistance(target, vertex(i-1),
                                                         vertex(i), length);
      // It is possible that (result == vertex(i)) due to rounding errors.
      *next_vertex = (result == vertex(i)) ? (i + 1) : i;
      return result;
    }
    target -= length;
  }
  *next_vertex = num_vertices();
  return vertex(num_vertices() - 1);
}

S2Point S2Polyline::Interpolate(double fraction) const {
  int next_vertex;
  return GetSuffix(fraction, &next_vertex);
}

double S2Polyline::UnInterpolate(S2Point const& point, int next_vertex) const {
  DCHECK_GT(num_vertices(), 0);
  if (num_vertices() < 2) {
    return 0;
  }
  S1Angle length_sum;
  for (int i = 1; i < next_vertex; ++i) {
    length_sum += S1Angle(vertex(i-1), vertex(i));
  }
  S1Angle length_to_point = length_sum + S1Angle(vertex(next_vertex-1), point);
  for (int i = next_vertex; i < num_vertices(); ++i) {
    length_sum += S1Angle(vertex(i-1), vertex(i));
  }
  // The ratio can be greater than 1.0 due to rounding errors or because the
  // point is not exactly on the polyline.
  return min(1.0, length_to_point / length_sum);
}

S2Point S2Polyline::Project(S2Point const& point, int* next_vertex) const {
  DCHECK_GT(num_vertices(), 0);

  if (num_vertices() == 1) {
    // If there is only one vertex, it is always closest to any given point.
    *next_vertex = 1;
    return vertex(0);
  }

  // Initial value larger than any possible distance on the unit sphere.
  S1Angle min_distance = S1Angle::Radians(10);
  int min_index = -1;

  // Find the line segment in the polyline that is closest to the point given.
  for (int i = 1; i < num_vertices(); ++i) {
    S1Angle distance_to_segment = S2EdgeUtil::GetDistance(point, vertex(i-1),
                                                          vertex(i));
    if (distance_to_segment < min_distance) {
      min_distance = distance_to_segment;
      min_index = i;
    }
  }
  DCHECK_NE(min_index, -1);

  // Compute the point on the segment found that is closest to the point given.
  S2Point closest_point = S2EdgeUtil::GetClosestPoint(
      point, vertex(min_index-1), vertex(min_index));

  *next_vertex = min_index + (closest_point == vertex(min_index) ? 1 : 0);
  return closest_point;
}

bool S2Polyline::IsOnRight(S2Point const& point) const {
  DCHECK_GE(num_vertices(), 2);

  int next_vertex;
  S2Point closest_point = Project(point, &next_vertex);

  DCHECK_GE(next_vertex, 1);
  DCHECK_LE(next_vertex, num_vertices());

  // If the closest point C is an interior vertex of the polyline, let B and D
  // be the previous and next vertices.  The given point P is on the right of
  // the polyline (locally) if B, P, D are ordered CCW around vertex C.
  if (closest_point == vertex(next_vertex-1) && next_vertex > 1 &&
      next_vertex < num_vertices()) {
    if (point == vertex(next_vertex-1))
      return false;  // Polyline vertices are not on the RHS.
    return S2::OrderedCCW(vertex(next_vertex-2), point, vertex(next_vertex),
                          vertex(next_vertex-1));
  }

  // Otherwise, the closest point C is incident to exactly one polyline edge.
  // We test the point P against that edge.
  if (next_vertex == num_vertices())
    --next_vertex;

  return S2::RobustCCW(point, vertex(next_vertex), vertex(next_vertex-1)) > 0;
}

bool S2Polyline::Intersects(S2Polyline const* line) const {
  if (num_vertices() <= 0 || line->num_vertices() <= 0) {
    return false;
  }

  if (!GetRectBound().Intersects(line->GetRectBound())) {
    return false;
  }

  // TODO(user) look into S2EdgeIndex to make this near linear in performance.
  for (int i = 1; i < num_vertices(); ++i) {
    S2EdgeUtil::EdgeCrosser crosser(
        &vertex(i - 1), &vertex(i), &line->vertex(0));
    for (int j = 1; j < line->num_vertices(); ++j) {
      if (crosser.RobustCrossing(&line->vertex(j)) >= 0) {
        return true;
      }
    }
  }
  return false;
}

void S2Polyline::Reverse() {
  reverse(vertices_, vertices_ + num_vertices_);
}

S2LatLngRect S2Polyline::GetRectBound() const {
  S2EdgeUtil::RectBounder bounder;
  for (int i = 0; i < num_vertices(); ++i) {
    bounder.AddPoint(&vertex(i));
  }
  return bounder.GetBound();
}

S2Cap S2Polyline::GetCapBound() const {
  return GetRectBound().GetCapBound();
}

bool S2Polyline::MayIntersect(S2Cell const& cell) const {
  if (num_vertices() == 0) return false;

  // We only need to check whether the cell contains vertex 0 for correctness,
  // but these tests are cheap compared to edge crossings so we might as well
  // check all the vertices.
  for (int i = 0; i < num_vertices(); ++i) {
    if (cell.Contains(vertex(i))) return true;
  }
  S2Point cell_vertices[4];
  for (int i = 0; i < 4; ++i) {
    cell_vertices[i] = cell.GetVertex(i);
  }
  for (int j = 0; j < 4; ++j) {
    S2EdgeUtil::EdgeCrosser crosser(&cell_vertices[j], &cell_vertices[(j+1)&3],
                                    &vertex(0));
    for (int i = 1; i < num_vertices(); ++i) {
      if (crosser.RobustCrossing(&vertex(i)) >= 0) {
        // There is a proper crossing, or two vertices were the same.
        return true;
      }
    }
  }
  return false;
}

void S2Polyline::Encode(Encoder* const encoder) const {
  encoder->Ensure(num_vertices_ * sizeof(*vertices_) + 10);  // sufficient

  encoder->put8(kCurrentEncodingVersionNumber);
  encoder->put32(num_vertices_);
  encoder->putn(vertices_, sizeof(*vertices_) * num_vertices_);

  DCHECK_GE(encoder->avail(), 0);
}

bool S2Polyline::Decode(Decoder* const decoder) {
  unsigned char version = decoder->get8();
  if (version > kCurrentEncodingVersionNumber) return false;

  num_vertices_ = decoder->get32();
  delete[] vertices_;
  vertices_ = new S2Point[num_vertices_];
  decoder->getn(vertices_, num_vertices_ * sizeof(*vertices_));

  if (S2::debug) {
    vector<S2Point> vertex_vector(vertices_, vertices_ + num_vertices_);
    CHECK(IsValid(vertex_vector));
  }

  return decoder->avail() >= 0;
}

namespace {

// Given a polyline, a tolerance distance, and a start index, this function
// returns the maximal end index such that the line segment between these two
// vertices passes within "tolerance" of all interior vertices, in order.
int FindEndVertex(S2Polyline const& polyline,
                  S1Angle const& tolerance, int index) {
  DCHECK_GE(tolerance.radians(), 0);
  DCHECK_LT((index + 1), polyline.num_vertices());

  // The basic idea is to keep track of the "pie wedge" of angles from the
  // starting vertex such that a ray from the starting vertex at that angle
  // will pass through the discs of radius "tolerance" centered around all
  // vertices processed so far.

  // First we define a "coordinate frame" for the tangent and normal spaces
  // at the starting vertex.  Essentially this means picking three
  // orthonormal vectors X,Y,Z such that X and Y span the tangent plane at
  // the starting vertex, and Z is "up".  We use the coordinate frame to
  // define a mapping from 3D direction vectors to a one-dimensional "ray
  // angle" in the range (-Pi, Pi].  The angle of a direction vector is
  // computed by transforming it into the X,Y,Z basis, and then calculating
  // atan2(y,x).  This mapping allows us to represent a wedge of angles as a
  // 1D interval.  Since the interval wraps around, we represent it as an
  // S1Interval, i.e. an interval on the unit circle.
  Matrix3x3_d frame;
  S2Point const& origin = polyline.vertex(index);
  S2::GetFrame(origin, &frame);

  // As we go along, we keep track of the current wedge of angles and the
  // distance to the last vertex (which must be non-decreasing).
  S1Interval current_wedge = S1Interval::Full();
  double last_distance = 0;

  for (++index; index < polyline.num_vertices(); ++index) {
    S2Point const& candidate = polyline.vertex(index);
    double distance = origin.Angle(candidate);

    // We don't allow simplification to create edges longer than 90 degrees,
    // to avoid numeric instability as lengths approach 180 degrees.  (We do
    // need to allow for original edges longer than 90 degrees, though.)
    if (distance > M_PI/2 && last_distance > 0) break;

    // Vertices must be in increasing order along the ray, except for the
    // initial disc around the origin.
    if (distance < last_distance && last_distance > tolerance.radians()) break;
    last_distance = distance;

    // Points that are within the tolerance distance of the origin do not
    // constrain the ray direction, so we can ignore them.
    if (distance <= tolerance.radians()) continue;

    // If the current wedge of angles does not contain the angle to this
    // vertex, then stop right now.  Note that the wedge of possible ray
    // angles is not necessarily empty yet, but we can't continue unless we
    // are willing to backtrack to the last vertex that was contained within
    // the wedge (since we don't create new vertices).  This would be more
    // complicated and also make the worst-case running time more than linear.
    S2Point direction = S2::ToFrame(frame, candidate);
    double center = atan2(direction.y(), direction.x());
    if (!current_wedge.Contains(center)) break;

    // To determine how this vertex constrains the possible ray angles,
    // consider the triangle ABC where A is the origin, B is the candidate
    // vertex, and C is one of the two tangent points between A and the
    // spherical cap of radius "tolerance" centered at B.  Then from the
    // spherical law of sines, sin(a)/sin(A) = sin(c)/sin(C), where "a" and
    // "c" are the lengths of the edges opposite A and C.  In our case C is a
    // 90 degree angle, therefore A = asin(sin(a) / sin(c)).  Angle A is the
    // half-angle of the allowable wedge.

    double half_angle = asin(sin(tolerance.radians()) / sin(distance));
    S1Interval target = S1Interval::FromPoint(center).Expanded(half_angle);
    current_wedge = current_wedge.Intersection(target);
    DCHECK(!current_wedge.is_empty());
  }
  // We break out of the loop when we reach a vertex index that can't be
  // included in the line segment, so back up by one vertex.
  return index - 1;
}
}

void S2Polyline::SubsampleVertices(S1Angle const& tolerance,
                                   vector<int>* indices) const {
  indices->clear();
  if (num_vertices() == 0) return;

  indices->push_back(0);
  S1Angle clamped_tolerance = max(tolerance, S1Angle::Radians(0));
  for (int index = 0; index + 1 < num_vertices(); ) {
    int next_index = FindEndVertex(*this, clamped_tolerance, index);
    // Don't create duplicate adjacent vertices.
    if (vertex(next_index) != vertex(index)) {
      indices->push_back(next_index);
    }
    index = next_index;
  }
}

bool S2Polyline::ApproxEquals(S2Polyline const* b, double max_error) const {
  if (num_vertices() != b->num_vertices()) return false;
  for (int offset = 0; offset < num_vertices(); ++offset) {
    if (!S2::ApproxEquals(vertex(offset), b->vertex(offset), max_error)) {
      return false;
    }
  }
  return true;
}

namespace {
// Return the first i > "index" such that the ith vertex of "pline" is not at
// the same point as the "index"th vertex.  Returns pline.num_vertices() if
// there is no such value of i.
inline int NextDistinctVertex(S2Polyline const& pline, int index) {
  S2Point const& initial = pline.vertex(index);
  do {
    ++index;
  } while (index < pline.num_vertices() && pline.vertex(index) == initial);
  return index;
}

// This struct represents a search state in the NearlyCoversPolyline algorithm
// below.  See the description of the algorithm for details.
struct SearchState {
  inline SearchState(int i_val, int j_val, bool i_in_progress_val)
      : i(i_val), j(j_val), i_in_progress(i_in_progress_val) {}

  int i;
  int j;
  bool i_in_progress;
};
}  // namespace

namespace std {
template<>
struct less<SearchState> {
  // This operator is needed for storing SearchStates in a set.  The ordering
  // chosen has no special meaning.
  inline bool operator()(SearchState const& lhs, SearchState const& rhs) const {
    if (lhs.i < rhs.i) return true;
    if (lhs.i > rhs.i) return false;
    if (lhs.j < rhs.j) return true;
    if (lhs.j > rhs.j) return false;
    return !lhs.i_in_progress && rhs.i_in_progress;
  }
};
}  // namespace std

bool S2Polyline::NearlyCoversPolyline(S2Polyline const& covered,
                                      S1Angle const& max_error) const {
  // NOTE: This algorithm is described assuming that adjacent vertices in a
  // polyline are never at the same point.  That is, the ith and i+1th vertices
  // of a polyline are never at the same point in space.  The implementation
  // does not make this assumption.

  // DEFINITIONS:
  //   - edge "i" of a polyline is the edge from the ith to i+1th vertex.
  //   - covered_j is a polyline consisting of edges 0 through j of "covered."
  //   - this_i is a polyline consisting of edges 0 through i of this polyline.
  //
  // A search state is represented as an (int, int, bool) tuple, (i, j,
  // i_in_progress).  Using the "drive a car" analogy from the header comment, a
  // search state signifies that you can drive one car along "covered" from its
  // first vertex through a point on its jth edge, and another car along this
  // polyline from some point on or before its ith edge to a to a point on its
  // ith edge, such that no car ever goes backward, and the cars are always
  // within "max_error" of each other.  If i_in_progress is true, it means that
  // you can definitely drive along "covered" through the jth vertex (beginning
  // of the jth edge). Otherwise, you can definitely drive along "covered"
  // through the point on the jth edge of "covered" closest to the ith vertex of
  // this polyline.
  //
  // The algorithm begins by finding all edges of this polyline that are within
  // "max_error" of the first vertex of "covered," and adding search states
  // representing all of these possible starting states to the stack of
  // "pending" states.
  //
  // The algorithm proceeds by popping the next pending state,
  // (i,j,i_in_progress), off of the stack.  First it checks to see if that
  // state represents finding a valid covering of "covered" and returns true if
  // so.  Next, if the state represents reaching the end of this polyline
  // without finding a successful covering, the algorithm moves on to the next
  // state in the stack.  Otherwise, if state (i+1,j,false) is valid, it is
  // added to the stack of pending states.  Same for state (i,j+1,true).
  //
  // We need the stack because when "i" and "j" can both be incremented,
  // sometimes only one choice leads to a solution.  We use a set to keep track
  // of visited states to avoid duplicating work.  With the set, the worst-case
  // number of states examined is O(n+m) where n = this->num_vertices() and m =
  // covered.num_vertices().  Without it, the amount of work could be as high as
  // O((n*m)^2).  Using set, the running time is O((n*m) log (n*m)).
  //
  // TODO(user): Benchmark this, and see if the set is worth it.
  vector<SearchState> pending;
  set<SearchState> done;

  // Find all possible starting states.
  for (int i = 0, next_i = NextDistinctVertex(*this, 0);
       next_i < this->num_vertices();
       i = next_i, next_i = NextDistinctVertex(*this, next_i)) {
    S2Point closest_point = S2EdgeUtil::GetClosestPoint(
        covered.vertex(0), this->vertex(i), this->vertex(next_i));
    if (closest_point != this->vertex(next_i) &&
        S1Angle(closest_point, covered.vertex(0)) <= max_error) {
      pending.push_back(SearchState(i, 0, true));
    }
  }

  while (!pending.empty()) {
    SearchState const state = pending.back();
    pending.pop_back();
    if (!done.insert(state).second) continue;

    int const next_i = NextDistinctVertex(*this, state.i);
    int const next_j = NextDistinctVertex(covered, state.j);
    if (next_j == covered.num_vertices()) {
      return true;
    } else if (next_i == this->num_vertices()) {
      continue;
    }

    S2Point i_begin, j_begin;
    if (state.i_in_progress) {
      j_begin = covered.vertex(state.j);
      i_begin = S2EdgeUtil::GetClosestPoint(
          j_begin, this->vertex(state.i), this->vertex(next_i));
    } else {
      i_begin = this->vertex(state.i);
      j_begin = S2EdgeUtil::GetClosestPoint(
          i_begin, covered.vertex(state.j), covered.vertex(next_j));
    }

    if (S2EdgeUtil::IsEdgeBNearEdgeA(j_begin, covered.vertex(next_j),
                                     i_begin, this->vertex(next_i),
                                     max_error)) {
      pending.push_back(SearchState(next_i, state.j, false));
    }
    if (S2EdgeUtil::IsEdgeBNearEdgeA(i_begin, this->vertex(next_i),
                                     j_begin, covered.vertex(next_j),
                                     max_error)) {
      pending.push_back(SearchState(state.i, next_j, true));
    }
  }
  return false;
}
