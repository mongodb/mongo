// Copyright 2009 Google Inc. All Rights Reserved.

#include "s2edgeindex.h"

#include <set>
using std::set;
using std::multiset;

#include <string>
using std::string;

#include <vector>
using std::vector;


#include "base/commandlineflags.h"
#include "base/stringprintf.h"
#include "base/logging.h"
#include "testing/base/public/benchmark.h"
#include "testing/base/public/gunit.h"
#include "s2cap.h"
#include "s2cell.h"
#include "s2cellid.h"
#include "s2edgeutil.h"
#include "s2loop.h"
#include "s2testing.h"
#include "util/math/vector3-inl.h"
#include "util/math/matrix3x3-inl.h"

DECLARE_bool(always_recurse_on_children);

typedef pair<S2Point, S2Point> S2Edge;

static const double kEarthRadiusMeters = 6371000;



// Generates a random edge whose center is in the given cap.
static S2Edge RandomEdgeCrossingCap(double max_length_meters,
                                    const S2Cap& cap) {
  // Pick the edge center at random.
  S2Point edge_center = S2Testing::SamplePoint(cap);
  // Pick two random points in a suitably sized cap about the edge center.
  S2Cap edge_cap = S2Cap::FromAxisAngle(
      edge_center,
      S1Angle::Radians(max_length_meters / kEarthRadiusMeters / 2));
  S2Point p1 = S2Testing::SamplePoint(edge_cap);
  S2Point p2 = S2Testing::SamplePoint(edge_cap);
  return S2Edge(p1, p2);
}

// Generates "num_edges" random edges, of length at most
// "edge_length_meters_max" and each of whose center is in a randomly located
// cap with radius "cap_span_meters", and puts results into "edges".
static void GenerateRandomEarthEdges(double edge_length_meters_max,
                                     double cap_span_meters,
                                     int num_edges,
                                     vector<S2Edge>* edges) {
  S2Cap cap = S2Cap::FromAxisAngle(
      S2Testing::RandomPoint(),
      S1Angle::Radians(cap_span_meters / kEarthRadiusMeters));
  for (int i = 0; i < num_edges; ++i) {
    edges->push_back(RandomEdgeCrossingCap(edge_length_meters_max, cap));
  }
}

static string ToString(const S2Edge& e1, const S2Edge& e2) {
  double u1, v1, u2, v2, u3, v3, u4, v4;
  S2::XYZtoFaceUV(e1.first, &u1, &v1);
  S2::XYZtoFaceUV(e1.second, &u2, &v2);
  S2::XYZtoFaceUV(e2.first, &u3, &v3);
  S2::XYZtoFaceUV(e2.second, &u4, &v4);
  u2 = u2 - u1;
  u3 = u3 - u1;
  u4 = u4 - u1;
  v2 = v2 - v1;
  v3 = v3 - v1;
  v4 = v4 - v1;
  return StringPrintf("[%6.2f,%6.2f] 0,0 -> %3.3e,%3.3e) --"
                      " (%3.3e,%3.3e -> %3.3e %3.3e)",
                      u1, v1, u2, v2, u3, v3, u4, v4);
}

class EdgeVectorIndex: public S2EdgeIndex {
 public:
  explicit EdgeVectorIndex(vector<S2Edge> const* edges):
      edges_(edges) {}

  virtual int num_edges() const { return edges_->size(); }
  virtual S2Point const* edge_from(int index) const {
    return &((*edges_)[index].first);
  }
  virtual S2Point const* edge_to(int index) const {
    return &((*edges_)[index].second);
  }
 private:
  vector<S2Edge> const* edges_;
};

static void TestAllCrossings(vector<S2Edge> const& all_edges,
                             int min_crossings,
                             int max_checks_crossings_ratio) {
  EdgeVectorIndex index(&all_edges);
  index.ComputeIndex();
  EdgeVectorIndex::Iterator it(&index);
  double total_crossings = 0;
  double total_index_checks = 0;

  VLOG(1) << "start detailed checking\n";
  for (int in = 0; in < all_edges.size(); ++in) {
    S2Edge e = all_edges[in];

    set<int> candidate_set;

    for (it.GetCandidates(e.first, e.second); !it.Done(); it.Next()) {
      candidate_set.insert(it.Index());
      total_index_checks++;
    }

    for (int i = 0; i < all_edges.size(); ++i) {
      int crossing = S2EdgeUtil::RobustCrossing(e.first, e.second,
                                                all_edges[i].first,
                                                all_edges[i].second);
      if (crossing  >= 0) {
        EXPECT_EQ(1, candidate_set.count(i))
            << "Edge " << i <<  " is not a candidate of edge " << in
            << " (" << ToString(all_edges[i], e) << ")";
        ++total_crossings;
      }
    }
  }

  VLOG(1) << "Pairs/num crossings/check crossing ratio: "
          << all_edges.size() * all_edges.size() << "/"
          << total_crossings << "/"
          << total_index_checks / total_crossings;
  EXPECT_LE(min_crossings, total_crossings);
  EXPECT_GE(total_crossings * max_checks_crossings_ratio, total_index_checks);
}


// Generates random edges and tests, for each edge,
// that all those that cross are candidates.
static void TestCrossingsRandomInCap(int num_edges,
                                     double edge_length_max,
                                     double cap_span_meters,
                                     int min_crossings,
                                     int max_checks_crossings_ratio) {
  vector<S2Edge> all_edges;
  GenerateRandomEarthEdges(edge_length_max, cap_span_meters, num_edges,
                           &all_edges);
  TestAllCrossings(all_edges, min_crossings, max_checks_crossings_ratio);
}


TEST(S2EdgeIndex, LoopCandidateOfItself) {
  vector<S2Point> ps;  // A diamond loop around 0,180.
  ps.push_back(S2Testing::MakePoint("0:178"));
  ps.push_back(S2Testing::MakePoint("-1:180"));
  ps.push_back(S2Testing::MakePoint("0:-179"));
  ps.push_back(S2Testing::MakePoint("1:-180"));
  vector<S2Edge> all_edges;
  for (int i = 0; i < 4; ++i) {
    all_edges.push_back(make_pair(ps[i], ps[(i+1)%4]));
  }
  TestAllCrossings(all_edges, 0, 16);
}

TEST(S2EdgeIndex, RandomEdgeCrossings) {
  TestCrossingsRandomInCap(2000, 30, 5000, 500, 2);
  TestCrossingsRandomInCap(1000, 100, 5000, 500, 3);
  TestCrossingsRandomInCap(1000, 1000, 5000, 1000, 40);
  TestCrossingsRandomInCap(500, 5000, 5000, 5000, 20);
}

TEST(S2EdgeIndex, RandomEdgeCrossingsSparse) {
  for (int i = 0; i < 5; ++i) {
    TestCrossingsRandomInCap(2000, 100, 5000, 500, 8);
    TestCrossingsRandomInCap(2000, 300, 50000, 1000, 10);
  }
}

TEST(S2EdgeIndex, DegenerateEdgeOnCellVertexIsItsOwnCandidate) {
  for (int i = 0; i < 100; ++i) {
    S2Cell cell(S2Testing::GetRandomCellId());
    S2Edge e = make_pair(cell.GetVertex(0), cell.GetVertex(0));
    vector<S2Edge> all_edges;
    all_edges.push_back(e);
    EdgeVectorIndex index(&all_edges);
    index.ComputeIndex();
    EdgeVectorIndex::Iterator it(&index);
    it.GetCandidates(e.first, e.second);
    ASSERT_FALSE(it.Done());
    it.Next();
    ASSERT_TRUE(it.Done());
  }
}

TEST(S2EdgeIndex, LongEdgeCrossesLoop) {
  vector<S2Edge> all_edges;
  S2Point loop_center = S2Testing::MakePoint("42:107");
  pair<S2Point, S2Point> test_edge = make_pair(S2::Origin(), loop_center);

  S2Loop* loop = S2Testing::MakeRegularLoop(loop_center, 4,
                                            7e-3);  // = 5km/6400km
  for (int i = 0; i < loop->num_vertices(); ++i) {
    all_edges.push_back(make_pair(loop->vertex(i), loop->vertex(i+1)));
  }
  delete loop;
  EdgeVectorIndex index(&all_edges);
  index.ComputeIndex();
  EdgeVectorIndex::Iterator it(&index);
  it.GetCandidates(test_edge.first, test_edge.second);

  ASSERT_FALSE(it.Done());  // At least one candidate
}

TEST(S2EdgeIndex, CollinearEdgesOnCellBoundaries) {
  FLAGS_always_recurse_on_children = true;
  const int kNumPointsOnEdge = 8;  // About 32 edges
  for (int level = 0; level <= S2CellId::kMaxLevel; ++level) {
    S2Cell cell(S2Testing::GetRandomCellId(level));
    int v1 = S2Testing::rnd.Uniform(4);
    int v2 = (v1 + 1) & 3;
    S2Point p1 = cell.GetVertex(v1);
    S2Point p2 = cell.GetVertex(v2);
    S2Point p2_p1 = (p2 - p1) / kNumPointsOnEdge;
    vector<S2Edge> all_edges;
    S2Point points[kNumPointsOnEdge+1];
    for (int i = 0; i <= kNumPointsOnEdge; ++i) {
      points[i] = (p1 + i * p2_p1).Normalize();
      for (int j = 0; j < i; ++j) {
        all_edges.push_back(make_pair(points[i], points[j]));
      }
    }
    TestAllCrossings(all_edges, kNumPointsOnEdge*kNumPointsOnEdge,
                     kNumPointsOnEdge*kNumPointsOnEdge);
  }
}

TEST(S2EdgeIndex, QuadTreeGetsComputedAutomatically) {
  int const kNumVertices = 200;
  S2Point loop_center = S2Testing::MakePoint("42:107");
  S2Loop* loop = S2Testing::MakeRegularLoop(loop_center, kNumVertices,
                                            7e-3);  // = 5km/6400km
  S2Point q = S2Testing::MakePoint("5:5");

  S2LoopIndex index(loop);
  S2LoopIndex::Iterator it(&index);

  int num_candidates = 0;
  it.GetCandidates(q, q);
  for (; !it.Done(); it.Next()) {
    num_candidates++;
  }
  EXPECT_EQ(kNumVertices, num_candidates);

  bool computed = false;
  for (int i = 0; i < 500; ++i) {  // Trigger the quad tree computation
    num_candidates = 0;
    for (it.GetCandidates(q, q); !it.Done(); it.Next()) num_candidates++;
    if (!computed && num_candidates == 0) {
      LOG(INFO) << "Index was computed at iteration " << i;
      computed = true;
    }
  }

  num_candidates = 0;
  for (it.GetCandidates(q, q); !it.Done(); it.Next()) num_candidates++;
  EXPECT_EQ(0, num_candidates);

  delete loop;
}


// MICROBENCHMARKS (and related structures)

// Generates a bunch of random edges and tests each against all others for
// crossings. This is just for benchmarking; there's no correctness testing in
// this function. Set "cutoff_level" negative to apply brute force checking.
static void ComputeCrossings(int num_edges,
                             double edge_length_max,
                             double cap_span_meters,
                             int brute_force) {
  StopBenchmarkTiming();
  vector<S2Edge> all_edges;
  GenerateRandomEarthEdges(edge_length_max, cap_span_meters, num_edges,
                           &all_edges);
  StartBenchmarkTiming();
  if (brute_force) {
    for (vector<S2Edge>::const_iterator it = all_edges.begin();
         it != all_edges.end(); ++it) {
      for (vector<S2Edge>::const_iterator it2 = all_edges.begin();
           it2 != all_edges.end(); ++it2) {
        S2EdgeUtil::RobustCrossing(it->first, it->second,
                                   it2->first, it2->second);
      }
    }
  } else {
    EdgeVectorIndex index(&all_edges);
    index.ComputeIndex();
    EdgeVectorIndex::Iterator can_it(&index);
    for (vector<S2Edge>::const_iterator it = all_edges.begin();
         it != all_edges.end(); ++it) {
      for (can_it.GetCandidates(it->first, it->second);
           !can_it.Done(); can_it.Next()) {
        int in = can_it.Index();
        S2EdgeUtil::RobustCrossing(all_edges[in].first, all_edges[in].second,
                                   it->first,
                                   it->second);
      }
    }
  }
}

// "Sparse" tests are those where we expect relatively few segment crossings.
// In general the segment lengths are short (< 300m) and the random segments
// are distributed over caps of radius 5-50km.

static void BM_TestCrossingsSparse(int iters, int num_edges, int brute_force) {
  for (int i = 0; i < iters; ++i) {
    ComputeCrossings(num_edges, 30,  5000,  brute_force);
    ComputeCrossings(num_edges, 100, 5000,  brute_force);
  }
}
BENCHMARK(BM_TestCrossingsSparse)
    ->ArgPair(10, true)
    ->ArgPair(10, false)
    ->ArgPair(50, true)
    ->ArgPair(50, false)
    ->ArgPair(100, true)
    ->ArgPair(100, false)
    ->ArgPair(300, true)
    ->ArgPair(300, false)
    ->ArgPair(600, true)
    ->ArgPair(600, false)
    ->ArgPair(1000, true)
    ->ArgPair(1000, false)
    ->ArgPair(2000, false)
    ->ArgPair(5000, false)
    ->ArgPair(10000, false)
    ->ArgPair(20000, false)
    ->ArgPair(50000, false)
    ->ArgPair(100000, false);

// These benchmarks are used to find the right trade-off between brute force
// and quad tree.

// To compute kQuadTreeInsertionCost
static void BM_QuadTreeInsertionCost(int iters) {
  const int kNumVertices = 1000;
  StopBenchmarkTiming();
  S2Point loop_center = S2Testing::MakePoint("42:107");
  S2Loop* loop = S2Testing::MakeRegularLoop(loop_center, kNumVertices,
                                            7e-3);  // = 5km/6400km
  StartBenchmarkTiming();

  for (int i = 0; i < iters; ++i) {
    S2LoopIndex index(loop);
    index.ComputeIndex();
  }
  delete loop;
}
BENCHMARK(BM_QuadTreeInsertionCost);

// To compute kQuadTreeFindCost
static void BM_QuadTreeFindCost(int iters, int num_vertices) {
  StopBenchmarkTiming();
  S2Point loop_center = S2Testing::MakePoint("42:107");
  S2Loop* loop = S2Testing::MakeRegularLoop(loop_center, num_vertices,
                                            7e-3);  // = 5km/6400km
  S2Point p(S2Testing::MakePoint("42:106.99"));
  S2Point q(S2Testing::MakePoint("42:107.01"));
  S2LoopIndex index(loop);
  index.ComputeIndex();
  EdgeVectorIndex::Iterator can_it(&index);
  StartBenchmarkTiming();

  for (int i = 0; i < iters; ++i) {
    can_it.GetCandidates(p, q);
  }
  delete loop;
}
BENCHMARK(BM_QuadTreeFindCost)
  ->Arg(10)
  ->Arg(100)
  ->Arg(1000)
  ->Arg(10000);
