// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2cellid.h"

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <cstdio>
#include <hash_map>
using __gnu_cxx::hash_map;

#include <sstream>
#include <vector>
using std::vector;


#include "base/commandlineflags.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/malloc_interface.h"
#include "base/sysinfo.h"
#include "testing/base/public/gunit.h"
#include "s2.h"
#include "s2latlng.h"
#include "s2testing.h"
#include "util/math/mathutil.h"

#define int8 HTM_int8  // To avoid conflicts with our own 'int8'
#include "third_party/htm/include/SpatialIndex.h"
#include "third_party/htm/include/RangeConvex.h"
#undef int8

DEFINE_int32(iters, 20000000,
             "Number of iterations for timing tests with optimized build");

DEFINE_int32(htm_level, 29, "Maximum HTM level to use");
DEFINE_int32(build_level, 5, "HTM build level to use");

static S2CellId GetCellId(double lat_degrees, double lng_degrees) {
  S2CellId id = S2CellId::FromLatLng(S2LatLng::FromDegrees(lat_degrees,
                                                           lng_degrees));
  LOG(INFO) << hex << id.id();
  return id;
}

TEST(S2CellId, DefaultConstructor) {
  S2CellId id;
  EXPECT_EQ(id.id(), 0);
  EXPECT_FALSE(id.is_valid());
}

TEST(S2CellId, FaceDefinitions) {
  EXPECT_EQ(GetCellId(0, 0).face(), 0);
  EXPECT_EQ(GetCellId(0, 90).face(), 1);
  EXPECT_EQ(GetCellId(90, 0).face(), 2);
  EXPECT_EQ(GetCellId(0, 180).face(), 3);
  EXPECT_EQ(GetCellId(0, -90).face(), 4);
  EXPECT_EQ(GetCellId(-90, 0).face(), 5);
}

TEST(S2CellId, ParentChildRelationships) {
  S2CellId id = S2CellId::FromFacePosLevel(3, 0x12345678,
                                           S2CellId::kMaxLevel - 4);
  EXPECT_TRUE(id.is_valid());
  EXPECT_EQ(id.face(), 3);
  EXPECT_EQ(id.pos(), 0x12345700);
  EXPECT_EQ(id.level(), S2CellId::kMaxLevel - 4);
  EXPECT_FALSE(id.is_leaf());

  EXPECT_EQ(id.child_begin(id.level() + 2).pos(), 0x12345610);
  EXPECT_EQ(id.child_begin().pos(), 0x12345640);
  EXPECT_EQ(id.parent().pos(), 0x12345400);
  EXPECT_EQ(id.parent(id.level() - 2).pos(), 0x12345000);

  // Check ordering of children relative to parents.
  EXPECT_LT(id.child_begin(), id);
  EXPECT_GT(id.child_end(), id);
  EXPECT_EQ(id.child_begin().next().next().next().next(), id.child_end());
  EXPECT_EQ(id.child_begin(S2CellId::kMaxLevel), id.range_min());
  EXPECT_EQ(id.child_end(S2CellId::kMaxLevel), id.range_max().next());

  // Check that cells are represented by the position of their center
  // along the Hilbert curve.
  EXPECT_EQ(id.range_min().id() + id.range_max().id(), 2 * id.id());
}

TEST(S2CellId, Wrapping) {
  // Check wrapping from beginning of Hilbert curve to end and vice versa.
  EXPECT_EQ(S2CellId::Begin(0).prev_wrap(), S2CellId::End(0).prev());

  EXPECT_EQ(S2CellId::Begin(S2CellId::kMaxLevel).prev_wrap(),
            S2CellId::FromFacePosLevel(
                5, ~static_cast<uint64>(0) >> S2CellId::kFaceBits,
                S2CellId::kMaxLevel));
  EXPECT_EQ(S2CellId::Begin(S2CellId::kMaxLevel).advance_wrap(-1),
            S2CellId::FromFacePosLevel(
                5, ~static_cast<uint64>(0) >> S2CellId::kFaceBits,
                S2CellId::kMaxLevel));

  EXPECT_EQ(S2CellId::End(4).prev().next_wrap(), S2CellId::Begin(4));
  EXPECT_EQ(S2CellId::End(4).advance(-1).advance_wrap(1), S2CellId::Begin(4));

  EXPECT_EQ(S2CellId::End(S2CellId::kMaxLevel).prev().next_wrap(),
            S2CellId::FromFacePosLevel(0, 0, S2CellId::kMaxLevel));
  EXPECT_EQ(S2CellId::End(S2CellId::kMaxLevel).advance(-1).advance_wrap(1),
            S2CellId::FromFacePosLevel(0, 0, S2CellId::kMaxLevel));
}

TEST(S2CellId, Advance) {
  S2CellId id = S2CellId::FromFacePosLevel(3, 0x12345678,
                                           S2CellId::kMaxLevel - 4);
  // Check basic properties of advance().
  EXPECT_EQ(S2CellId::Begin(0).advance(7), S2CellId::End(0));
  EXPECT_EQ(S2CellId::Begin(0).advance(12), S2CellId::End(0));
  EXPECT_EQ(S2CellId::End(0).advance(-7), S2CellId::Begin(0));
  EXPECT_EQ(S2CellId::End(0).advance(-12000000), S2CellId::Begin(0));
  int num_level_5_cells = 6 << (2 * 5);
  EXPECT_EQ(S2CellId::Begin(5).advance(500),
            S2CellId::End(5).advance(500 - num_level_5_cells));
  EXPECT_EQ(id.child_begin(S2CellId::kMaxLevel).advance(256),
            id.next().child_begin(S2CellId::kMaxLevel));
  EXPECT_EQ(S2CellId::FromFacePosLevel(1, 0, S2CellId::kMaxLevel)
            .advance(static_cast<int64>(4) << (2 * S2CellId::kMaxLevel)),
            S2CellId::FromFacePosLevel(5, 0, S2CellId::kMaxLevel));

  // Check basic properties of advance_wrap().
  EXPECT_EQ(S2CellId::Begin(0).advance_wrap(7),
            S2CellId::FromFacePosLevel(1, 0, 0));
  EXPECT_EQ(S2CellId::Begin(0).advance_wrap(12), S2CellId::Begin(0));
  EXPECT_EQ(S2CellId::FromFacePosLevel(5, 0, 0).advance_wrap(-7),
            S2CellId::FromFacePosLevel(4, 0, 0));
  EXPECT_EQ(S2CellId::Begin(0).advance_wrap(-12000000), S2CellId::Begin(0));
  EXPECT_EQ(S2CellId::Begin(5).advance_wrap(6644),
            S2CellId::Begin(5).advance_wrap(-11788));
  EXPECT_EQ(id.child_begin(S2CellId::kMaxLevel).advance_wrap(256),
            id.next().child_begin(S2CellId::kMaxLevel));
  EXPECT_EQ(S2CellId::FromFacePosLevel(5, 0, S2CellId::kMaxLevel)
            .advance_wrap(static_cast<int64>(2) << (2 * S2CellId::kMaxLevel)),
            S2CellId::FromFacePosLevel(1, 0, S2CellId::kMaxLevel));
}

TEST(S2CellId, Inverses) {
  // Check the conversion of random leaf cells to S2LatLngs and back.
  for (int i = 0; i < 200000; ++i) {
    S2CellId id = S2Testing::GetRandomCellId(S2CellId::kMaxLevel);
    EXPECT_TRUE(id.is_leaf());
    EXPECT_EQ(id.level(), S2CellId::kMaxLevel);
    S2LatLng center = id.ToLatLng();
    EXPECT_EQ(S2CellId::FromLatLng(center).id(), id.id());
  }
}

TEST(S2CellId, Tokens) {
  // Test random cell ids at all levels.
  for (int i = 0; i < 10000; ++i) {
    S2CellId id = S2Testing::GetRandomCellId();
    string token = id.ToToken();
    EXPECT_LE(token.size(), 16);
    EXPECT_EQ(S2CellId::FromToken(token), id);
  }
  // Check that invalid cell ids can be encoded.
  string token = S2CellId::None().ToToken();
  EXPECT_EQ(S2CellId::FromToken(token), S2CellId::None());
}


static const int kMaxExpandLevel = 3;

static void ExpandCell(S2CellId const& parent, vector<S2CellId>* cells,
                       hash_map<S2CellId, S2CellId>* parent_map) {
  cells->push_back(parent);
  if (parent.level() == kMaxExpandLevel) return;
  int i, j, orientation;
  int face = parent.ToFaceIJOrientation(&i, &j, &orientation);
  EXPECT_EQ(face, parent.face());

  S2CellId child = parent.child_begin();
  for (int pos = 0; child != parent.child_end(); child = child.next(), ++pos) {
    // Do some basic checks on the children
    EXPECT_EQ(parent.child(pos), child);
    EXPECT_EQ(child.level(), parent.level() + 1);
    EXPECT_FALSE(child.is_leaf());
    int child_orientation;
    EXPECT_EQ(child.ToFaceIJOrientation(&i, &j, &child_orientation), face);
    EXPECT_EQ(child_orientation, orientation ^ S2::kPosToOrientation[pos]);

    (*parent_map)[child] = parent;
    ExpandCell(child, cells, parent_map);
  }
}

TEST(S2CellId, Containment) {
  // Test contains() and intersects().
  hash_map<S2CellId, S2CellId> parent_map;
  vector<S2CellId> cells;
  for (int face = 0; face < 6; ++face) {
    ExpandCell(S2CellId::FromFacePosLevel(face, 0, 0), &cells, &parent_map);
  }
  for (int i = 0; i < cells.size(); ++i) {
    for (int j = 0; j < cells.size(); ++j) {
      bool contained = true;
      for (S2CellId id = cells[j]; id != cells[i]; id = parent_map[id]) {
        if (parent_map.find(id) == parent_map.end()) {
          contained = false;
          break;
        }
      }
      EXPECT_EQ(cells[i].contains(cells[j]), contained);
      EXPECT_EQ(cells[j] >= cells[i].range_min() &&
                cells[j] <= cells[i].range_max(), contained);
      EXPECT_EQ(cells[i].intersects(cells[j]),
                cells[i].contains(cells[j]) || cells[j].contains(cells[i]));
    }
  }
}

static int const kMaxWalkLevel = 8;

TEST(S2CellId, Continuity) {
  // Make sure that sequentially increasing cell ids form a continuous
  // path over the surface of the sphere, i.e. there are no
  // discontinuous jumps from one region to another.

  double max_dist = S2::kMaxEdge.GetValue(kMaxWalkLevel);
  S2CellId end = S2CellId::End(kMaxWalkLevel);
  S2CellId id = S2CellId::Begin(kMaxWalkLevel);
  for (; id != end; id = id.next()) {
    EXPECT_LE(id.ToPointRaw().Angle(id.next_wrap().ToPointRaw()), max_dist);
    EXPECT_EQ(id.advance_wrap(1), id.next_wrap());
    EXPECT_EQ(id.next_wrap().advance_wrap(-1), id);

    // Check that the ToPointRaw() returns the center of each cell
    // in (s,t) coordinates.
    double u, v;
    S2::XYZtoFaceUV(id.ToPointRaw(), &u, &v);
    static double const kCellSize = 1.0 / (1 << kMaxWalkLevel);
    EXPECT_NEAR(drem(S2::UVtoST(u), 0.5 * kCellSize), 0.0, 1e-15);
    EXPECT_NEAR(drem(S2::UVtoST(v), 0.5 * kCellSize), 0.0, 1e-15);
  }
}

TEST(S2CellId, Coverage) {
  // Make sure that random points on the sphere can be represented to the
  // expected level of accuracy, which in the worst case is sqrt(2/3) times
  // the maximum arc length between the points on the sphere associated with
  // adjacent values of "i" or "j".  (It is sqrt(2/3) rather than 1/2 because
  // the cells at the corners of each face are stretched -- they have 60 and
  // 120 degree angles.)

  double max_dist = 0.5 * S2::kMaxDiag.GetValue(S2CellId::kMaxLevel);
  for (int i = 0; i < 1000000; ++i) {
    S2Point p = S2Testing::RandomPoint();
    S2Point q = S2CellId::FromPoint(p).ToPointRaw();
    EXPECT_LE(p.Angle(q), max_dist);
  }
}

static void TestAllNeighbors(S2CellId const& id, int level) {
  DCHECK_GE(level, id.level());
  DCHECK_LT(level, S2CellId::kMaxLevel);

  // We compute AppendAllNeighbors, and then add in all the children of "id"
  // at the given level.  We then compare this against the result of finding
  // all the vertex neighbors of all the vertices of children of "id" at the
  // given level.  These should give the same result.
  vector<S2CellId> all, expected;
  id.AppendAllNeighbors(level, &all);
  S2CellId end = id.child_end(level + 1);
  for (S2CellId c = id.child_begin(level + 1); c != end; c = c.next()) {
    all.push_back(c.parent());
    c.AppendVertexNeighbors(level, &expected);
  }
  // Sort the results and eliminate duplicates.
  sort(all.begin(), all.end());
  sort(expected.begin(), expected.end());
  all.erase(unique(all.begin(), all.end()), all.end());
  expected.erase(unique(expected.begin(), expected.end()), expected.end());
  EXPECT_EQ(expected, all);
}

TEST(S2CellId, Neighbors) {
  // Check the edge neighbors of face 1.
  static int out_faces[] = { 5, 3, 2, 0 };
  S2CellId face_nbrs[4];
  S2CellId::FromFacePosLevel(1, 0, 0).GetEdgeNeighbors(face_nbrs);
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(face_nbrs[i].is_face());
    EXPECT_EQ(face_nbrs[i].face(), out_faces[i]);
  }

  // Check the vertex neighbors of the center of face 2 at level 5.
  vector<S2CellId> nbrs;
  S2CellId::FromPoint(S2Point(0, 0, 1)).AppendVertexNeighbors(5, &nbrs);
  sort(nbrs.begin(), nbrs.end());
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(nbrs[i], S2CellId::FromFaceIJ(
                 2, (1 << 29) - (i < 2), (1 << 29) - (i == 0 || i == 3))
             .parent(5));
  }
  nbrs.clear();

  // Check the vertex neighbors of the corner of faces 0, 4, and 5.
  S2CellId id = S2CellId::FromFacePosLevel(0, 0, S2CellId::kMaxLevel);
  id.AppendVertexNeighbors(0, &nbrs);
  sort(nbrs.begin(), nbrs.end());
  EXPECT_EQ(nbrs.size(), 3);
  EXPECT_EQ(nbrs[0], S2CellId::FromFacePosLevel(0, 0, 0));
  EXPECT_EQ(nbrs[1], S2CellId::FromFacePosLevel(4, 0, 0));
  EXPECT_EQ(nbrs[2], S2CellId::FromFacePosLevel(5, 0, 0));

  // Check that AppendAllNeighbors produces results that are consistent
  // with AppendVertexNeighbors for a bunch of random cells.
  for (int i = 0; i < 1000; ++i) {
    S2CellId id = S2Testing::GetRandomCellId();
    if (id.is_leaf()) id = id.parent();

    // TestAllNeighbors computes approximately 2**(2*(diff+1)) cell ids,
    // so it's not reasonable to use large values of "diff".
    int max_diff = min(6, S2CellId::kMaxLevel - id.level() - 1);
    int level = id.level() + S2Testing::rnd.Uniform(max_diff);
    TestAllNeighbors(id, level);
  }
}

TEST(S2CellId, OutputOperator) {
  S2CellId cell(0xbb04000000000000ULL);
  ostringstream s;
  s << cell;
  EXPECT_EQ("5/31200", s.str());
}

TEST(S2CellId, ToPointBenchmark) {
  // This "test" is really a benchmark, so skip it unless we're optimized.
  if (DEBUG_MODE) return;

  // Test speed of conversions from points to leaf cells.
  double control_start = S2Testing::GetCpuTime();
  S2CellId begin = S2CellId::Begin(S2CellId::kMaxLevel);
  S2CellId end = S2CellId::End(S2CellId::kMaxLevel);
  uint64 delta = (begin.id() - end.id()) / FLAGS_iters;
  delta &= ~static_cast<uint64>(1);  // Make sure all ids are leaf cells.

  S2CellId id = begin;
  double sum = 0;
  for (int i = FLAGS_iters; i > 0; --i) {
    sum += static_cast<double>(id.id());
    id = S2CellId(id.id() + delta);
  }
  double control_time = S2Testing::GetCpuTime() - control_start;
  printf("\tControl:    %8.3f usecs\n", 1e6 * control_time / FLAGS_iters);
  EXPECT_NE(sum, 0);  // Don't let the loop get optimized away.

  double test_start = S2Testing::GetCpuTime();
  sum = 0;
  id = begin;
  for (int i = FLAGS_iters; i > 0; --i) {
    sum += id.ToPointRaw()[0];
    id = S2CellId(id.id() + delta);
  }
  double test_time = S2Testing::GetCpuTime() - test_start - control_time;
  printf("\tToPointRaw: %8.3f usecs\n", 1e6 * test_time / FLAGS_iters);
  EXPECT_NE(sum, 0);  // Don't let the loop get optimized away.

  test_start = S2Testing::GetCpuTime();
  sum = 0;
  id = begin;
  for (int i = FLAGS_iters; i > 0; --i) {
    sum += id.ToPoint()[0];
    id = S2CellId(id.id() + delta);
  }
  test_time = S2Testing::GetCpuTime() - test_start - control_time;
  printf("\tToPoint:    %8.3f usecs\n", 1e6 * test_time / FLAGS_iters);
  EXPECT_NE(sum, 0);  // Don't let the loop get optimized away.
}

TEST(S2CellId, FromPointBenchmark) {
  // This "test" is really a benchmark, so skip it unless we're optimized.
  if (DEBUG_MODE) return;

  // The sample points follow a spiral curve that completes one revolution
  // around the z-axis every 1/dt samples.  The z-coordinate increases
  // from -4 to +4 over FLAGS_iters samples.

  S2Point start(1, 0, -4);
  double dz = (-2 * start.z()) / FLAGS_iters;
  double dt = 1.37482937133e-4;

  // Test speed of conversions from leaf cells to points.
  double control_start = S2Testing::GetCpuTime();
  uint64 isum = 0;
  S2Point p = start;
  for (int i = FLAGS_iters; i > 0; --i) {
    // Cheap rotation around the z-axis (spirals inward slightly
    // each revolution).
    p += S2Point(-dt * p.y(), dt * p.x(), dz);
    isum += MathUtil::FastIntRound(p[0] + p[1] + p[2]);
  }
  double control_time = S2Testing::GetCpuTime() - control_start;
  printf("\tControl:    %8.3f usecs\n", 1e6 * control_time / FLAGS_iters);
  EXPECT_NE(isum, 0);  // Don't let the loop get optimized away.

  double test_start = S2Testing::GetCpuTime();
  isum = 0;
  p = start;
  for (int i = FLAGS_iters; i > 0; --i) {
    p += S2Point(-dt * p.y(), dt * p.x(), dz);
    isum += S2CellId::FromPoint(p).id();
  }
  double test_time = S2Testing::GetCpuTime() - test_start - control_time;
  printf("\tFromPoint:  %8.3f usecs\n", 1e6 * test_time / FLAGS_iters);
  EXPECT_NE(isum, 0);  // Don't let the loop get optimized away.
}

TEST(S2CellId, HtmBenchmark) {
  // This "test" is really a benchmark, so skip it unless we're optimized.
  if (DEBUG_MODE) return;

  // The HTM methods are about 100 times slower than the S2CellId methods,
  // so we adjust the number of iterations accordingly.
  int htm_iters = FLAGS_iters / 100;

  SpatialVector start(1, 0, -4);
  double dz = (-2 * start.z()) / htm_iters;
  double dt = 1.37482937133e-4;

  double test_start = S2Testing::GetCpuTime();
  uint64 mem_start = MemoryUsage(0);
  MallocInterface* mi = MallocInterface::instance();
  size_t heap_start, heap_end;
  CHECK(mi->GetNumericProperty("generic.current_allocated_bytes", &heap_start));
  SpatialIndex htm(FLAGS_htm_level, FLAGS_build_level);
  double constructor_time = S2Testing::GetCpuTime() - test_start;
  printf("\tHTM constructor time:  %12.3f ms\n", 1e3 * constructor_time);
  printf("\tHTM heap size increase:   %9lld\n", MemoryUsage(0) - mem_start);
  CHECK(mi->GetNumericProperty("generic.current_allocated_bytes", &heap_end));
  printf("\tHTM heap bytes allocated: %9u\n", heap_end - heap_start);

  test_start = S2Testing::GetCpuTime();
  double sum = 0;
  SpatialVector v = start;
  for (int i = htm_iters; i > 0; --i) {
    v.set(v.x() - dt * v.y(), v.y() + dt * v.x(), v.z() + dz);
    sum += v.x();
  }
  double htm_control = S2Testing::GetCpuTime() - test_start;
  printf("\tHTM Control:   %8.3f usecs\n", 1e6 * htm_control / htm_iters);
  EXPECT_NE(sum, 0);  // Don't let the loop get optimized away.

  // Keeping the returned ids in a vector adds a negligible amount of time
  // to the idByPoint test and makes it much easier to test pointById.
  vector<uint64> ids(htm_iters);
  test_start = S2Testing::GetCpuTime();
  v = start;
  for (int i = htm_iters; i > 0; --i) {
    v.set(v.x() - dt * v.y(), v.y() + dt * v.x(), v.z() + dz);
    ids[i-1] = htm.idByPoint(v);
  }
  double idByPoint_time = S2Testing::GetCpuTime() - test_start - htm_control;
  printf("\tHTM FromPoint: %8.3f usecs\n",
          1e6 * idByPoint_time / htm_iters);

  test_start = S2Testing::GetCpuTime();
  sum = 0;
  v = start;
  for (int i = htm_iters; i > 0; --i) {
    SpatialVector v2;
    htm.pointById(v2, ids[i-1]);
    sum += v2.x();
  }
  double pointById_time = S2Testing::GetCpuTime() - test_start;
  printf("\tHTM ToPoint:   %8.3f usecs\n",
          1e6 * pointById_time / htm_iters);
  EXPECT_NE(sum, 0);  // Don't let the loop get optimized away.
}
