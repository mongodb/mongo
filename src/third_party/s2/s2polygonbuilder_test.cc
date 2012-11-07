// Copyright 2006 Google Inc. All Rights Reserved.

#include "s2polygonbuilder.h"

#include <set>
using std::set;
using std::multiset;


#include "base/logging.h"
#include "base/macros.h"
#include "base/scoped_ptr.h"
#include "strings/stringprintf.h"
#include "testing/base/public/gunit.h"
#include "s2cap.h"
#include "s2polygon.h"
#include "s2polyline.h"
#include "s2testing.h"
#include "util/math/matrix3x3-inl.h"

namespace {

struct Chain {
  // A chain represents either a polyline or a loop, depending
  // on whether "closed" is true.
  char const* str;
  bool closed;
};

struct TestCase {
  int undirected_edges;
  // +1 = undirected, -1 = directed, 0 = either one

  int xor_edges;
  // +1 = XOR, -1 = don't XOR, 0 = either one

  bool can_split;
  // Can edges be split for this test case?

  double min_merge, max_merge;
  // Min and max vertex merge radius for this test case in degrees.

  double min_vertex_angle;
  // Minimum angle in degrees between any two edges *after* vertex merging.

  Chain chains_in[20];
  // Each test case consists of a set of input loops and polylines.

  char const* loops_out[10];
  // The expected set of output loops, directed appropriately.

  int num_unused_edges;
  // The expected number of unused edges.
};

TestCase test_cases[] = {
  // 0: No loops.
  { 0, 0, true, 0.0, 10.0, 90.0,
    { { NULL, false } },
    { NULL }, 0 },

  // 1: One loop with some extra edges.
  { 0, 0, true, 0.0, 4.0, 15.0,
    { { "0:0, 0:10, 10:5", true },
      { "0:0, 5:5", false },
      { "10:5, 20:7, 30:10, 40:15, 50:3, 60:-20", false } },
    { "0:0, 0:10, 10:5" }, 6 },

  // 2: One loop that has an edge removed by XORing, plus lots of extra edges.
  { 0, 1, true, 0.0, 1.0, 45.0,  // XOR
    { { "0:0, 0:10, 5:15, 10:10, 10:0", true },
      { "10:10, 12:12, 14:14, 16:16, 18:18", false },
      { "14:14, 14:16, 14:18, 14:20", false },
      { "14:18, 16:20, 18:22", false },
      { "18:12, 16:12, 14:12, 12:12", false },
      { "20:18, 18:16, 16:14, 14:12", false },
      { "20:14, 18:14, 16:14", false },
      { "5:15, 0:10", false } },
    { NULL }, 21 },

  // 3: Three loops (two shells and one hole) that combine into one.
  { 0, 1, true, 0.0, 4.0, 90.0,  // XOR
    { { "0:0, 0:10, 5:10, 10:10, 10:5, 10:0", true },
      { "0:10, 0:15, 5:15, 5:10", true },
      { "10:10, 5:10, 5:5, 10:5", true } },
    { "0:0, 0:10, 0:15, 5:15, 5:10, 5:5, 10:5, 10:0" }, 0 },

  // 4: A big CCW triangle contained 3 CW triangular holes.  The whole thing
  // looks like a pyramid of nine small triangles (with two extra edges).
  { -1, 0, true, 0.0, 0.9, 30.0,  // Directed edges required for unique result.
    { { "0:0, 0:2, 0:4, 0:6, 1:5, 2:4, 3:3, 2:2, 1:1", true },
      { "0:2, 1:1, 1:3", true },
      { "0:4, 1:3, 1:5", true },
      { "1:3, 2:2, 2:4", true },
      { "0:0, -1:1", false },
      { "3:3, 5:5", false } },
    { "0:0, 0:2, 1:1",
      "0:2, 0:4, 1:3",
      "0:4, 0:6, 1:5",
      "1:1, 1:3, 2:2",
      "1:3, 1:5, 2:4",
      "2:2, 2:4, 3:3" }, 2 },

  // 5: A square divided into four subsquares.  In this case we want
  // to extract the four loops rather than taking their union.
  // There are four extra edges as well.
  { 0, -1, true, 0.0, 4.0, 90.0,  // Don't XOR
    { { "0:0, 0:5, 5:5, 5:0", true },
      { "0:5, 0:10, 5:10, 5:5", true },
      { "5:0, 5:5, 10:5, 10:0", true },
      { "5:5, 5:10, 10:10, 10:5", true },
      { "0:10, 0:15, 0:20", false },
      { "20:0, 15:0, 10:0", false } },
    { "0:0, 0:5, 5:5, 5:0",
      "0:5, 0:10, 5:10, 5:5",
      "5:0, 5:5, 10:5, 10:0",
      "5:5, 5:10, 10:10, 10:5" }, 4 },

  // 6: Five nested loops that touch at a point.
  { 0, 0, true, 0.0, 0.8, 5.0,
    { { "0:0, 0:10, 10:10, 10:0", true },
      { "0:0, 1:9, 9:9, 9:1", true },
      { "0:0, 2:8, 8:8, 8:2", true },
      { "0:0, 3:7, 7:7, 7:3", true },
      { "0:0, 4:6, 6:6, 6:4", true } },
    { "0:0, 0:10, 10:10, 10:0",
      "0:0, 1:9, 9:9, 9:1",
      "0:0, 2:8, 8:8, 8:2",
      "0:0, 3:7, 7:7, 7:3",
      "0:0, 4:6, 6:6, 6:4" }, 0 },

  // 7: Four diamonds nested within each other touching at two points.
  { -1, 0, true, 0.0, 4.0, 15.0,  // Directed edges required for unique result.
    { { "0:-20, -10:0, 0:20, 10:0", true },
      { "0:10, -10:0, 0:-10, 10:0", true },
      { "0:-10, -5:0, 0:10, 5:0", true },
      { "0:5, -5:0, 0:-5, 5:0", true } },
    { "0:-20, -10:0, 0:-10, 10:0",
      "0:-10, -5:0, 0:-5, 5:0",
      "0:5, -5:0, 0:10, 5:0",
      "0:10, -10:0, 0:20, 10:0" }, 0 },

  // 8: Seven diamonds nested within each other touching at one
  // point between each nested pair.
  { 0, 0, true, 0.0, 9.0, 4.0,
    { { "0:-70, -70:0, 0:70, 70:0", true },
      { "0:-70, -60:0, 0:60, 60:0", true },
      { "0:-50, -60:0, 0:50, 50:0", true },
      { "0:-40, -40:0, 0:50, 40:0", true },
      { "0:-30, -30:0, 0:30, 40:0", true },
      { "0:-20, -20:0, 0:30, 20:0", true },
      { "0:-10, -20:0, 0:10, 10:0", true } },
    { "0:-70, -70:0, 0:70, 70:0",
      "0:-70, -60:0, 0:60, 60:0",
      "0:-50, -60:0, 0:50, 50:0",
      "0:-40, -40:0, 0:50, 40:0",
      "0:-30, -30:0, 0:30, 40:0",
      "0:-20, -20:0, 0:30, 20:0",
      "0:-10, -20:0, 0:10, 10:0" }, 0 },

  // 9: A triangle and a self-intersecting bowtie.
  { 0, 0, false, 0.0, 4.0, 45.0,
    { { "0:0, 0:10, 5:5", true },
      { "0:20, 0:30, 10:20", false },
      { "10:20, 10:30, 0:20", false } },
    { "0:0, 0:10, 5:5" }, 4 },

  // 10: Two triangles that intersect each other.
  { 0, 0, false, 0.0, 2.0, 45.0,
    { { "0:0, 0:12, 6:6", true },
      { "3:6, 3:18, 9:12", true } },
    { NULL }, 6 },

  // 11: Four squares that combine to make a big square.  The nominal edges of
  // the square are at +/-8.5 degrees in latitude and longitude.  All vertices
  // except the center vertex are perturbed by up to 0.5 degrees in latitude
  // and/or longitude.  The various copies of the center vertex are misaligned
  // by more than this (i.e. they are structured as a tree where adjacent
  // vertices are separated by at most 1 degree in latitude and/or longitude)
  // so that the clustering algorithm needs more than one iteration to find
  // them all.  Note that the merged position of this vertex doesn't matter
  // because it is XORed away in the output.  However, it's important that
  // all edge pairs that need to be XORed are separated by no more than
  // 'min_merge' below.

  { 0, 1, true, 1.7, 5.8, 70.0,  // XOR, min_merge > sqrt(2), max_merge < 6.
    { { "-8:-8, -8:0", false },
      { "-8:1, -8:8", false },
      { "0:-9, 1:-1", false },
      { "1:2, 1:9", false },
      { "0:8, 2:2", false },
      { "0:-2, 1:-8", false },
      { "8:9, 9:1", false },
      { "9:0, 8:-9", false },
      { "9:-9, 0:-8", false },
      { "1:-9, -9:-9", false },
      { "8:0, 1:0", false },
      { "-1:1, -8:0", false },
      { "-8:1, -2:0", false },
      { "0:1, 8:1", false },
      { "-9:8, 1:8", false },
      { "0:9, 8:8", false } },
    { "8.5:8.5, 8.5:0.5, 8.5:-8.5, 0.5:-8.5, "
      "-8.5:-8.5, -8.5:0.5, -8.5:8.5, 0.5:8.5" }, 0 },
};

S2Point Perturb(S2Point const& x, double max_perturb) {
  // Perturb the point "x" randomly within a radius of max_perturb.

  if (max_perturb == 0) return x;
  return S2Testing::SamplePoint(
      S2Cap::FromAxisAngle(x.Normalize(),
                           S1Angle::Radians(max_perturb)));
}

void GetVertices(char const* str, Matrix3x3_d const& m,
                 vector<S2Point>* vertices) {
  // Parse the vertices and transform them into the given frame.

  scoped_ptr<S2Polyline> line(S2Testing::MakePolyline(str));
  for (int i = 0; i < line->num_vertices(); ++i) {
    vertices->push_back((m * line->vertex(i)).Normalize());
  }
}

void AddEdge(S2Point const& v0, S2Point const& v1,
             int max_splits, double max_perturb, double min_edge,
             S2PolygonBuilder* builder) {
  // Adds an edge from "v0" to "v1", possibly splitting it recursively up to
  // "max_splits" times, and perturbing each vertex up to a distance of
  // "max_perturb".  No edge shorter than "min_edge" will be created due to
  // splitting.

  double length = v0.Angle(v1);
  if (max_splits > 0 && S2Testing::rnd.OneIn(2) && length >= 2 * min_edge) {
    // Choose an interpolation parameter such that the length of each
    // piece is at least min_edge.
    double f = min_edge / length;
    double t = f + (1 - 2 * f) * S2Testing::rnd.RandDouble();

    // Now add the two sub-edges recursively.
    S2Point vmid = S2EdgeUtil::Interpolate(t, v0, v1);
    AddEdge(v0, vmid, max_splits - 1, max_perturb, min_edge, builder);
    AddEdge(vmid, v1, max_splits - 1, max_perturb, min_edge, builder);
  } else {
    builder->AddEdge(Perturb(v0, max_perturb),
                     Perturb(v1, max_perturb));
  }
}

void AddChain(Chain const& chain, Matrix3x3_d const& m,
              int max_splits, double max_perturb, double min_edge,
              S2PolygonBuilder* builder) {
  // Transform the given edge chain to the frame (x,y,z), optionally split
  // each edge into pieces and/or perturb the vertices up to the given
  // radius, and add them to the builder.

  vector<S2Point> vertices;
  GetVertices(chain.str, m, &vertices);
  if (chain.closed) vertices.push_back(vertices[0]);
  for (int i = 1; i < vertices.size(); ++i) {
    AddEdge(vertices[i-1], vertices[i], max_splits, max_perturb, min_edge,
            builder);
  }
}

bool FindLoop(S2Loop const* loop, vector<S2Loop*> const& candidates,
              int max_splits, double max_error) {
  // Return true if "loop" matches any of the given candidates.  The type
  // of matching depends on whether any edge splitting was done.

  for (int i = 0; i < candidates.size(); ++i) {
    if (max_splits == 0) {
      // The two loops should match except for vertex perturbations.
      if (loop->BoundaryApproxEquals(candidates[i], max_error)) return true;
    } else {
      // The two loops may have different numbers of vertices.
      if (loop->BoundaryNear(candidates[i], max_error)) return true;
    }
  }
  return false;
}

bool FindMissingLoops(vector<S2Loop*> const& actual,
                      vector<S2Loop*> const& expected,
                      Matrix3x3_d const& m,
                      int max_splits, double max_error,
                      char const* label) {
  // Dump any loops from "actual" that are not present in "expected".
  bool found = false;
  for (int i = 0; i < actual.size(); ++i) {
    if (FindLoop(actual[i], expected, max_splits, max_error))
      continue;

    fprintf(stderr, "%s loop %d:\n", label, i);
    S2Loop* loop = actual[i];
    for (int j = 0; j < loop->num_vertices(); ++j) {
      S2LatLng ll(m.Transpose() * loop->vertex(j));
      fprintf(stderr, "   [%.6f, %.6f]\n",
              ll.lat().degrees(), ll.lng().degrees());
    }
    found = true;
  }
  return found;
}

bool UnexpectedUnusedEdgeCount(int num_actual, int num_expected,
                               int max_splits) {
  // Return true if the actual number of unused edges is inconsistent
  // with the expected number of unused edges.
  //
  // If there are no splits, the number of unused edges should match exactly.
  // Otherwise, both values should be zero or both should be non-zero.
  if (max_splits == 0) {
    return num_actual != num_expected;
  } else {
    return (num_actual > 0) != (num_expected > 0);
  }
}

void DumpUnusedEdges(vector<pair<S2Point, S2Point> > const& unused_edges,
                     Matrix3x3_d const& m, int num_expected) {

  // Print the unused edges, transformed back into their original
  // latitude-longitude space in degrees.

  if (unused_edges.size() == num_expected) return;
  fprintf(stderr,
          "Wrong number of unused edges (%d expected, %"PRIuS" actual):\n",
          num_expected, unused_edges.size());
  for (int i = 0; i < unused_edges.size(); ++i) {
    S2LatLng p0(m.Transpose() * unused_edges[i].first);
    S2LatLng p1(m.Transpose() * unused_edges[i].second);
    fprintf(stderr, "  [%.6f, %.6f] -> [%.6f, %.5f]\n",
            p0.lat().degrees(), p0.lng().degrees(),
            p1.lat().degrees(), p1.lng().degrees());
  }
}

bool EvalTristate(int state) {
  return (state > 0) ? true : (state < 0) ? false : S2Testing::rnd.OneIn(2);
}

double SmallFraction() {
  // Returns a fraction between 0 and 1 where small values are more
  // likely.  In particular it often returns exactly 0, and often
  // returns a fraction whose logarithm is uniformly distributed
  // over some interval.

  double r = S2Testing::rnd.RandDouble();
  double u = S2Testing::rnd.RandDouble();
  if (r < 0.3) return 0.0;
  if (r < 0.6) return u;
  return pow(1e-10, u);
}

extern void DeleteLoop(S2Loop* loop) {
  delete loop;
}
extern void DeleteLoopsInVector(vector<S2Loop*>* loops) {
  for_each(loops->begin(), loops->end(), DeleteLoop);
  loops->clear();
}

bool TestBuilder(TestCase const* test) {
  for (int iter = 0; iter < 250; ++iter) {
    S2PolygonBuilderOptions options;
    options.set_undirected_edges(EvalTristate(test->undirected_edges));
    options.set_xor_edges(EvalTristate(test->xor_edges));

    // Each test has a minimum and a maximum merge radius.  The merge
    // radius must be at least the given minimum to ensure that all expected
    // merging will take place, and it must be at most the given maximum to
    // ensure that no unexpected merging takes place.
    //
    // If the minimum and maximum values are different, we have some latitude
    // to perturb the vertices as long as the merge radius is adjusted
    // appropriately.  If "p" is the maximum perturbation radius, "m" and
    // "M" are the min/max merge radii, and "v" is the vertex merge radius
    // for this test, we require that
    //
    //       v >= m + 2*p    and    v <= M - 2*p .
    //
    // This implies that we can choose "v" in the range [m,M], and then choose
    //
    //       p <= 0.5 * min(v - m, M - v) .
    //
    // Things get more complicated when we turn on edge splicing.  Since the
    // min/max merge radii apply to vertices, we need to adjust them to ensure
    // that vertices are not accidentally spliced into nearby edges.  Recall
    // that the edge splice radius is defined as (e = v * f) where "f" is the
    // edge splice fraction.  Letting "a" be the minimum angle between two
    // edges at a vertex, we need to ensure that
    //
    //     e <= M * sin(a) - 2*p .
    //
    // The right-hand side is a lower bound on the distance from a vertex to a
    // non-incident edge.  (To simplify things, we ignore this case and fold
    // it into the case below.)
    //
    // If we also split edges by introducing new vertices, things get even
    // more complicated.  First, the vertex merge radius "v" must be chosen
    // such that
    //
    //      e >= m + 2*p    and  v <= M * sin(a) - 2*p .
    //
    // Note that the right-hand inequality now applies to "v" rather than "e",
    // since a new vertex can be introduced anywhere along a split edge.
    //
    // Finally, we need to ensure that the new edges created by splitting an
    // edge are not too short, otherwise unbounded vertex merging and/or edge
    // splicing can occur.  Letting "g" be the minimum distance (gap) between
    // vertices along a split edge, we require that
    //
    //      2 * sin(a/2) * (g - m) - 2*p >= v
    //
    // which is satisfied whenever
    //
    //      g >= m + (v + 2*p) / sin(a)
    //
    // This inequality is derived by considering two edges of length "g"
    // meeting at an angle "a", where both vertices are perturbed by distance
    // "p" toward each other, and the shared vertex is perturbed by the
    // minimum merge radius "m" along one of the two edges.

    double min_merge = S1Angle::Degrees(test->min_merge).radians();
    double max_merge = S1Angle::Degrees(test->max_merge).radians();
    double min_sin = sin(S1Angle::Degrees(test->min_vertex_angle).radians());

    // Half of the time we allow edges to be split into smaller pieces
    // (up to 5 levels, i.e. up to 32 pieces).
    int max_splits = max(0, S2Testing::rnd.Uniform(10) - 4);
    if (!test->can_split) max_splits = 0;

    // We choosen randomly among two different values for the edge fraction,
    // just to exercise that code.
    double edge_fraction = options.edge_splice_fraction();
    double vertex_merge, max_perturb;
    if (min_sin < edge_fraction && S2Testing::rnd.OneIn(2)) {
      edge_fraction = min_sin;
    }
    if (max_splits == 0 && S2Testing::rnd.OneIn(2)) {
      // Turn off edge splicing completely.
      edge_fraction = 0;
      vertex_merge = min_merge + SmallFraction() * (max_merge - min_merge);
      max_perturb = 0.5 * min(vertex_merge - min_merge,
                              max_merge - vertex_merge);
    } else {
      // Splice edges.  These bounds also assume that edges may be split
      // (see detailed comments above).
      //
      // If edges are actually split, need to bump up the minimum merge radius
      // to ensure that split edges in opposite directions are unified.
      // Otherwise there will be tiny degenerate loops created.
      if (max_splits > 0) min_merge += 1e-15;
      min_merge /= edge_fraction;
      max_merge *= min_sin;
      DCHECK_GE(max_merge, min_merge);

      vertex_merge = min_merge + SmallFraction() * (max_merge - min_merge);
      max_perturb = 0.5 * min(edge_fraction * (vertex_merge - min_merge),
                              max_merge - vertex_merge);
    }

    // We can perturb by any amount up to the maximum, but choosing a
    // lower maximum decreases the error bounds when checking the output.
    max_perturb *= SmallFraction();

    // This is the minimum length of a split edge to prevent unexpected
    // merging and/or splicing (the "g" value mentioned above).
    double min_edge = min_merge + (vertex_merge + 2 * max_perturb) / min_sin;

    options.set_vertex_merge_radius(S1Angle::Radians(vertex_merge));
    options.set_edge_splice_fraction(edge_fraction);
    options.set_validate(true);
    S2PolygonBuilder builder(options);

    // On each iteration we randomly rotate the test case around the sphere.
    // This causes the S2PolygonBuilder to choose different first edges when
    // trying to build loops.
    S2Point x, y, z;
    S2Testing::GetRandomFrame(&x, &y, &z);
    Matrix3x3_d m = Matrix3x3_d::FromCols(x, y, z);
    builder.set_debug_matrix(m);

    for (int i = 0; test->chains_in[i].str; ++i) {
      AddChain(test->chains_in[i], m, max_splits, max_perturb, min_edge,
               &builder);
    }
    vector<S2Loop*> loops;
    S2PolygonBuilder::EdgeList unused_edges;
    if (test->xor_edges < 0) {
      builder.AssembleLoops(&loops, &unused_edges);
    } else {
      S2Polygon polygon;
      builder.AssemblePolygon(&polygon, &unused_edges);
      polygon.Release(&loops);
    }
    vector<S2Loop*> expected;
    for (int i = 0; test->loops_out[i]; ++i) {
      vector<S2Point> vertices;
      GetVertices(test->loops_out[i], m, &vertices);
      expected.push_back(new S2Loop(vertices));
    }
    // We assume that the vertex locations in the expected output polygon
    // are separated from the corresponding vertex locations in the input
    // edges by at most half of the minimum merge radius.  Essentially
    // this means that the expected output vertices should be near the
    // centroid of the various input vertices.
    //
    // If any edges were split, we need to allow a bit more error due to
    // inaccuracies in the interpolated positions.  Similarly, if any vertices
    // were perturbed, we need to bump up the error to allow for numerical
    // errors in the actual perturbation.
    double max_error = 0.5 * min_merge + max_perturb;
    if (max_splits > 0 || max_perturb > 0) max_error += 1e-15;

    // Note the single "|" below so that we print both sets of loops.
    if (FindMissingLoops(loops, expected, m,
                         max_splits, max_error, "Actual") |
        FindMissingLoops(expected, loops, m,
                         max_splits, max_error, "Expected") |
        UnexpectedUnusedEdgeCount(unused_edges.size(), test->num_unused_edges,
                                  max_splits)) {

      // We found a problem.  Print out the relevant parameters.
      DumpUnusedEdges(unused_edges, m, test->num_unused_edges);
      fprintf(stderr, "During iteration %d:\n  undirected: %d\n  xor: %d\n"
              "  max_splits: %d\n  max_perturb: %.6g\n"
              "  vertex_merge_radius: %.6g\n  edge_splice_fraction: %.6g\n"
              "  min_edge: %.6g\n  max_error: %.6g\n\n",
              iter, options.undirected_edges(), options.xor_edges(),
              max_splits, S1Angle::Radians(max_perturb).degrees(),
              options.vertex_merge_radius().degrees(),
              options.edge_splice_fraction(),
              S1Angle::Radians(min_edge).degrees(),
              S1Angle::Radians(max_error).degrees());
      DeleteLoopsInVector(&loops);
      DeleteLoopsInVector(&expected);
      return false;
    }
    DeleteLoopsInVector(&loops);
    DeleteLoopsInVector(&expected);
  }
  return true;
}

TEST(S2PolygonBuilder, AssembleLoops) {
  for (int i = 0; i < arraysize(test_cases); ++i) {
    SCOPED_TRACE(StringPrintf("Test case %d", i));
    EXPECT_TRUE(TestBuilder(&test_cases[i]));
  }
}

}  // namespace
