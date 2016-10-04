// Copyright 2005 Google Inc. All Rights Reserved.

#include <stdio.h>

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <map>
using std::map;
using std::multimap;

#include <set>
using std::set;
using std::multiset;

#include <vector>
using std::vector;


#include "base/commandlineflags.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "testing/base/public/benchmark.h"
#include "testing/base/public/gunit.h"
#include "util/coding/coder.h"
#include "s2cell.h"
#include "s2edgeindex.h"
#include "s2edgeutil.h"
#include "s2loop.h"
#include "s2testing.h"
#include "util/math/matrix3x3.h"
#include "util/math/matrix3x3-inl.h"

DECLARE_bool(s2debug);

class S2LoopTestBase : public testing::Test {
 protected:
  // Some standard loops to use in the tests (see descriptions below).  The
  // heap-checker ignores memory that is reachable from static variables, so
  // it is not necessary to free these loops.
  S2Loop const* const north_hemi;
  S2Loop const* const north_hemi3;
  S2Loop const* const south_hemi;
  S2Loop const* const west_hemi;
  S2Loop const* const east_hemi;
  S2Loop const* const near_hemi;
  S2Loop const* const far_hemi;
  S2Loop const* const candy_cane;
  S2Loop const* const small_ne_cw;
  S2Loop const* const arctic_80;
  S2Loop const* const antarctic_80;
  S2Loop const* const line_triangle;
  S2Loop const* const skinny_chevron;
  S2Loop const* const loop_a;
  S2Loop const* const loop_b;
  S2Loop const* const a_intersect_b;
  S2Loop const* const a_union_b;
  S2Loop const* const a_minus_b;
  S2Loop const* const b_minus_a;
  S2Loop const* const loop_c;
  S2Loop const* const loop_d;

 public:
  S2LoopTestBase():
      // The northern hemisphere, defined using two pairs of antipodal points.
      north_hemi(S2Testing::MakeLoop("0:-180, 0:-90, 0:0, 0:90")),

      // The northern hemisphere, defined using three points 120 degrees apart.
      north_hemi3(S2Testing::MakeLoop("0:-180, 0:-60, 0:60")),

      // The southern hemisphere, defined using two pairs of antipodal points.
      south_hemi(S2Testing::MakeLoop("0:90, 0:0, 0:-90, 0:-180")),

      // The western hemisphere, defined using two pairs of antipodal points.
      west_hemi(S2Testing::MakeLoop("0:-180, -90:0, 0:0, 90:0")),

      // The eastern hemisphere, defined using two pairs of antipodal points.
      east_hemi(S2Testing::MakeLoop("90:0, 0:0, -90:0, 0:-180")),

      // The "near" hemisphere, defined using two pairs of antipodal points.
      near_hemi(S2Testing::MakeLoop("0:-90, -90:0, 0:90, 90:0")),

      // The "far" hemisphere, defined using two pairs of antipodal points.
      far_hemi(S2Testing::MakeLoop("90:0, 0:90, -90:0, 0:-90")),

      // A spiral stripe that slightly over-wraps the equator.
      candy_cane(S2Testing::MakeLoop("-20:150, -20:-70, 0:70, 10:-150, "
                                     "10:70, -10:-70")),

      // A small clockwise loop in the northern & eastern hemisperes.
      small_ne_cw(S2Testing::MakeLoop("35:20, 45:20, 40:25")),

      // Loop around the north pole at 80 degrees.
      arctic_80(S2Testing::MakeLoop("80:-150, 80:-30, 80:90")),

      // Loop around the south pole at 80 degrees.
      antarctic_80(S2Testing::MakeLoop("-80:120, -80:0, -80:-120")),

      // A completely degenerate triangle along the equator that RobustCCW()
      // considers to be CCW.
      line_triangle(S2Testing::MakeLoop("0:1, 0:3, 0:2")),

      // A nearly-degenerate CCW chevron near the equator with very long sides
      // (about 80 degrees).  Its area is less than 1e-640, which is too small
      // to represent in double precision.
      skinny_chevron(S2Testing::MakeLoop("0:0, -1e-320:80, 0:1e-320, 1e-320:80")),

      // A diamond-shaped loop around the point 0:180.
      loop_a(S2Testing::MakeLoop("0:178, -1:180, 0:-179, 1:-180")),

      // Another diamond-shaped loop around the point 0:180.
      loop_b(S2Testing::MakeLoop("0:179, -1:180, 0:-178, 1:-180")),

      // The intersection of A and B.
      a_intersect_b(S2Testing::MakeLoop("0:179, -1:180, 0:-179, 1:-180")),

      // The union of A and B.
      a_union_b(S2Testing::MakeLoop("0:178, -1:180, 0:-178, 1:-180")),

      // A minus B (concave).
      a_minus_b(S2Testing::MakeLoop("0:178, -1:180, 0:179, 1:-180")),

      // B minus A (concave).
      b_minus_a(S2Testing::MakeLoop("0:-179, -1:180, 0:-178, 1:-180")),

      // A shape gotten from a by adding one triangle to one edge, and
      // subtracting another triangle on an opposite edge.
      loop_c(S2Testing::MakeLoop(
          "0:178, 0:180, -1:180, 0:-179, 1:-179, 1:-180")),

      // A shape gotten from a by adding one triangle to one edge, and
      // adding another triangle on an opposite edge.
      loop_d(S2Testing::MakeLoop(
          "0:178, -1:178, -1:180, 0:-179, 1:-179, 1:-180"))

  {}

  ~S2LoopTestBase() {
    delete north_hemi;
    delete north_hemi3;
    delete south_hemi;
    delete west_hemi;
    delete east_hemi;
    delete near_hemi;
    delete far_hemi;
    delete candy_cane;
    delete small_ne_cw;
    delete arctic_80;
    delete antarctic_80;
    delete line_triangle;
    delete skinny_chevron;
    delete loop_a;
    delete loop_b;
    delete a_intersect_b;
    delete a_union_b;
    delete a_minus_b;
    delete b_minus_a;
    delete loop_c;
    delete loop_d;
  }
};

TEST_F(S2LoopTestBase, GetRectBound) {
  EXPECT_TRUE(candy_cane->GetRectBound().lng().is_full());
  EXPECT_LT(candy_cane->GetRectBound().lat_lo().degrees(), -20);
  EXPECT_GT(candy_cane->GetRectBound().lat_hi().degrees(), 10);
  EXPECT_TRUE(small_ne_cw->GetRectBound().is_full());
  EXPECT_EQ(arctic_80->GetRectBound(),
            S2LatLngRect(S2LatLng::FromDegrees(80, -180),
                         S2LatLng::FromDegrees(90, 180)));
  EXPECT_EQ(antarctic_80->GetRectBound(),
            S2LatLngRect(S2LatLng::FromDegrees(-90, -180),
                         S2LatLng::FromDegrees(-80, 180)));

  // Create a loop that contains the complement of the "arctic_80" loop.
  scoped_ptr<S2Loop> arctic_80_inv(arctic_80->Clone());
  arctic_80_inv->Invert();
  // The highest latitude of each edge is attained at its midpoint.
  S2Point mid = 0.5 * (arctic_80_inv->vertex(0) + arctic_80_inv->vertex(1));
  EXPECT_DOUBLE_EQ(arctic_80_inv->GetRectBound().lat_hi().radians(),
                   S2LatLng(mid).lat().radians());

  EXPECT_TRUE(south_hemi->GetRectBound().lng().is_full());
  EXPECT_EQ(south_hemi->GetRectBound().lat(), R1Interval(-M_PI_2, 0));
}

TEST_F(S2LoopTestBase, GetAreaAndCentroid) {
  EXPECT_DOUBLE_EQ(north_hemi->GetArea(), 2 * M_PI);
  EXPECT_LE(east_hemi->GetArea(), 2 * M_PI + 1e-12);
  EXPECT_GE(east_hemi->GetArea(), 2 * M_PI - 1e-12);

  // Construct spherical caps of random height, and approximate their boundary
  // with closely spaces vertices.  Then check that the area and centroid are
  // correct.

  for (int i = 0; i < 100; ++i) {
    // Choose a coordinate frame for the spherical cap.
    S2Point x, y, z;
    S2Testing::GetRandomFrame(&x, &y, &z);

    // Given two points at latitude phi and whose longitudes differ by dtheta,
    // the geodesic between the two points has a maximum latitude of
    // atan(tan(phi) / cos(dtheta/2)).  This can be derived by positioning
    // the two points at (-dtheta/2, phi) and (dtheta/2, phi).
    //
    // We want to position the vertices close enough together so that their
    // maximum distance from the boundary of the spherical cap is kMaxDist.
    // Thus we want fabs(atan(tan(phi) / cos(dtheta/2)) - phi) <= kMaxDist.
    static double const kMaxDist = 1e-6;
    double height = 2 * S2Testing::rnd.RandDouble();
    double phi = asin(1 - height);
    double max_dtheta = 2 * acos(tan(fabs(phi)) / tan(fabs(phi) + kMaxDist));
    max_dtheta = min(M_PI, max_dtheta);  // At least 3 vertices.

    vector<S2Point> vertices;
    for (double theta = 0; theta < 2 * M_PI;
         theta += S2Testing::rnd.RandDouble() * max_dtheta) {
      vertices.push_back(cos(theta) * cos(phi) * x +
                         sin(theta) * cos(phi) * y +
                         sin(phi) * z);
    }
    S2Loop loop(vertices);
    double area = loop.GetArea();
    S2Point centroid = loop.GetCentroid();
    double expected_area = 2 * M_PI * height;
    EXPECT_LE(fabs(area - expected_area), 2 * M_PI * kMaxDist);
    EXPECT_GE(fabs(area - expected_area), 0.01 * kMaxDist); // high probability
    S2Point expected_centroid = expected_area * (1 - 0.5 * height) * z;
    EXPECT_LE((centroid - expected_centroid).Norm(), 2 * kMaxDist);
  }
}

static void Rotate(scoped_ptr<S2Loop>* ptr) {
  S2Loop* loop = ptr->get();
  vector<S2Point> vertices;
  for (int i = 1; i <= loop->num_vertices(); ++i) {
    vertices.push_back(loop->vertex(i));
  }
  ptr->reset(new S2Loop(vertices));
}

// Check that the turning angle is *identical* when the vertex order is
// rotated, and that the sign is inverted when the vertices are reversed.
static void CheckTurningAngleInvariants(S2Loop const* loop) {
  double expected = loop->GetTurningAngle();
  scoped_ptr<S2Loop> loop_copy(loop->Clone());
  for (int i = 0; i < loop->num_vertices(); ++i) {
    loop_copy->Invert();
    EXPECT_EQ(-expected, loop_copy->GetTurningAngle());
    loop_copy->Invert();
    Rotate(&loop_copy);
    EXPECT_EQ(expected, loop_copy->GetTurningAngle());
  }
}

TEST_F(S2LoopTestBase, GetTurningAngle) {
  EXPECT_NEAR(0, north_hemi3->GetTurningAngle(), 1e-15);
  CheckTurningAngleInvariants(north_hemi3);

  EXPECT_NEAR(0, west_hemi->GetTurningAngle(), 1e-15);
  CheckTurningAngleInvariants(west_hemi);

  // We don't have an easy way to estimate the turning angle of this loop, but
  // we can still check that the expected invariants hold.
  CheckTurningAngleInvariants(candy_cane);

  EXPECT_DOUBLE_EQ(-2 * M_PI, line_triangle->GetTurningAngle());
  CheckTurningAngleInvariants(line_triangle);

  EXPECT_DOUBLE_EQ(2 * M_PI, skinny_chevron->GetTurningAngle());
  CheckTurningAngleInvariants(skinny_chevron);
}

// Checks that if a loop is normalized, it doesn't contain a
// point outside of it, and vice versa.
static void CheckNormalizeAndContain(S2Loop const* loop) {
  S2Point p = S2Testing::MakePoint("40:40");

  scoped_ptr<S2Loop> flip(loop->Clone());
  flip->Invert();
  EXPECT_TRUE(loop->IsNormalized() ^ loop->Contains(p));
  EXPECT_TRUE(flip->IsNormalized() ^ flip->Contains(p));

  EXPECT_TRUE(loop->IsNormalized() ^ flip->IsNormalized());

  flip->Normalize();
  EXPECT_FALSE(flip->Contains(p));
}

TEST_F(S2LoopTestBase, NormalizedCompatibleWithContains) {
  CheckNormalizeAndContain(line_triangle);
  CheckNormalizeAndContain(skinny_chevron);
}

extern void DeleteLoop(S2Loop* loop) {
  delete loop;
}

TEST_F(S2LoopTestBase, Contains) {
  EXPECT_TRUE(candy_cane->Contains(S2LatLng::FromDegrees(5, 71).ToPoint()));

  // Create copies of these loops so that we can change the vertex order.
  scoped_ptr<S2Loop> north_copy(north_hemi->Clone());
  scoped_ptr<S2Loop> south_copy(south_hemi->Clone());
  scoped_ptr<S2Loop> west_copy(west_hemi->Clone());
  scoped_ptr<S2Loop> east_copy(east_hemi->Clone());
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(north_copy->Contains(S2Point(0, 0, 1)));
    EXPECT_FALSE(north_copy->Contains(S2Point(0, 0, -1)));
    EXPECT_FALSE(south_copy->Contains(S2Point(0, 0, 1)));
    EXPECT_TRUE(south_copy->Contains(S2Point(0, 0, -1)));
    EXPECT_FALSE(west_copy->Contains(S2Point(0, 1, 0)));
    EXPECT_TRUE(west_copy->Contains(S2Point(0, -1, 0)));
    EXPECT_TRUE(east_copy->Contains(S2Point(0, 1, 0)));
    EXPECT_FALSE(east_copy->Contains(S2Point(0, -1, 0)));
    Rotate(&north_copy);
    Rotate(&south_copy);
    Rotate(&east_copy);
    Rotate(&west_copy);
  }

  // This code checks each cell vertex is contained by exactly one of
  // the adjacent cells.
  for (int level = 0; level < 3; ++level) {
    vector<S2Loop*> loops;
    vector<S2Point> loop_vertices;
    set<S2Point> points;
    for (S2CellId id = S2CellId::Begin(level);
         id != S2CellId::End(level); id = id.next()) {
      S2Cell cell(id);
      points.insert(cell.GetCenter());
      for (int k = 0; k < 4; ++k) {
        loop_vertices.push_back(cell.GetVertex(k));
        points.insert(cell.GetVertex(k));
      }
      loops.push_back(new S2Loop(loop_vertices));
      loop_vertices.clear();
    }
    for (set<S2Point>::const_iterator i = points.begin();
        i != points.end(); ++i) {
      int count = 0;
      for (int j = 0; j < loops.size(); ++j) {
        if (loops[j]->Contains(*i)) ++count;
        // Contains and VirtualContainsPoint should have identical
        // implementation.
        EXPECT_EQ(loops[j]->Contains(*i), loops[j]->VirtualContainsPoint(*i));
      }
      EXPECT_EQ(count, 1);
    }

    for_each(loops.begin(), loops.end(), DeleteLoop);
    loops.clear();
  }
}

static void TestRelationWithDesc(S2Loop const* a, S2Loop const* b,
                                 int contains_or_crosses, bool intersects,
                                 bool nestable,
                                 const char* test_description) {
  SCOPED_TRACE(test_description);
  EXPECT_EQ(a->Contains(b), contains_or_crosses == 1);
  EXPECT_EQ(a->Intersects(b), intersects);
  if (nestable) EXPECT_EQ(a->ContainsNested(b), a->Contains(b));
  if (contains_or_crosses >= -1) {
    EXPECT_EQ(a->ContainsOrCrosses(b), contains_or_crosses);
  }
}

TEST_F(S2LoopTestBase, LoopRelations) {
#define TestRelation(a, b, contains, intersects, nestable)                   \
  TestRelationWithDesc(a, b, contains, intersects, nestable, "args " #a ", " #b)

  TestRelation(north_hemi, north_hemi, 1, true, false);
  TestRelation(north_hemi, south_hemi, 0, false, false);
  TestRelation(north_hemi, east_hemi, -1, true, false);
  TestRelation(north_hemi, arctic_80, 1, true, true);
  TestRelation(north_hemi, antarctic_80, 0, false, true);
  TestRelation(north_hemi, candy_cane, -1, true, false);

  // We can't compare north_hemi3 vs. north_hemi or south_hemi.
  TestRelation(north_hemi3, north_hemi3, 1, true, false);
  TestRelation(north_hemi3, east_hemi, -1, true, false);
  TestRelation(north_hemi3, arctic_80, 1, true, true);
  TestRelation(north_hemi3, antarctic_80, 0, false, true);
  TestRelation(north_hemi3, candy_cane, -1, true, false);

  TestRelation(south_hemi, north_hemi, 0, false, false);
  TestRelation(south_hemi, south_hemi, 1, true, false);
  TestRelation(south_hemi, far_hemi, -1, true, false);
  TestRelation(south_hemi, arctic_80, 0, false, true);
  TestRelation(south_hemi, antarctic_80, 1, true, true);
  TestRelation(south_hemi, candy_cane, -1, true, false);

  TestRelation(candy_cane, north_hemi, -1, true, false);
  TestRelation(candy_cane, south_hemi, -1, true, false);
  TestRelation(candy_cane, arctic_80, 0, false, true);
  TestRelation(candy_cane, antarctic_80, 0, false, true);
  TestRelation(candy_cane, candy_cane, 1, true, false);

  TestRelation(near_hemi, west_hemi, -1, true, false);

  TestRelation(small_ne_cw, south_hemi, 1, true, false);
  TestRelation(small_ne_cw, west_hemi, 1, true, false);
  TestRelation(small_ne_cw, north_hemi, -2, true, false);
  TestRelation(small_ne_cw, east_hemi, -2, true, false);

  TestRelation(loop_a, loop_a, 1, true, false);
  TestRelation(loop_a, loop_b, -1, true, false);
  TestRelation(loop_a, a_intersect_b, 1, true, false);
  TestRelation(loop_a, a_union_b, 0, true, false);
  TestRelation(loop_a, a_minus_b, 1, true, false);
  TestRelation(loop_a, b_minus_a, 0, false, false);

  TestRelation(loop_b, loop_a, -1, true, false);
  TestRelation(loop_b, loop_b, 1, true, false);
  TestRelation(loop_b, a_intersect_b, 1, true, false);
  TestRelation(loop_b, a_union_b, 0, true, false);
  TestRelation(loop_b, a_minus_b, 0, false, false);
  TestRelation(loop_b, b_minus_a, 1, true, false);

  TestRelation(a_intersect_b, loop_a, 0, true, false);
  TestRelation(a_intersect_b, loop_b, 0, true, false);
  TestRelation(a_intersect_b, a_intersect_b, 1, true, false);
  TestRelation(a_intersect_b, a_union_b, 0, true, true);
  TestRelation(a_intersect_b, a_minus_b, 0, false, false);
  TestRelation(a_intersect_b, b_minus_a, 0, false, false);

  TestRelation(a_union_b, loop_a, 1, true, false);
  TestRelation(a_union_b, loop_b, 1, true, false);
  TestRelation(a_union_b, a_intersect_b, 1, true, true);
  TestRelation(a_union_b, a_union_b, 1, true, false);
  TestRelation(a_union_b, a_minus_b, 1, true, false);
  TestRelation(a_union_b, b_minus_a, 1, true, false);

  TestRelation(a_minus_b, loop_a, 0, true, false);
  TestRelation(a_minus_b, loop_b, 0, false, false);
  TestRelation(a_minus_b, a_intersect_b, 0, false, false);
  TestRelation(a_minus_b, a_union_b, 0, true, false);
  TestRelation(a_minus_b, a_minus_b, 1, true, false);
  TestRelation(a_minus_b, b_minus_a, 0, false, true);

  TestRelation(b_minus_a, loop_a, 0, false, false);
  TestRelation(b_minus_a, loop_b, 0, true, false);
  TestRelation(b_minus_a, a_intersect_b, 0, false, false);
  TestRelation(b_minus_a, a_union_b, 0, true, false);
  TestRelation(b_minus_a, a_minus_b, 0, false, true);
  TestRelation(b_minus_a, b_minus_a, 1, true, false);

  // Added in 2010/08: Make sure the relations are correct if the loop
  // crossing happens on two ends of a shared boundary segment.
  TestRelation(loop_a, loop_c, -1, true, false);
  TestRelation(loop_c, loop_a, -1, true, false);
  TestRelation(loop_a, loop_d, 0, true, false);
  TestRelation(loop_d, loop_a, 1, true, false);
}

static S2Loop* MakeCellLoop(S2CellId const& begin, S2CellId const& end) {
  // Construct a CCW polygon whose boundary is the union of the cell ids
  // in the range [begin, end).  We add the edges one by one, removing
  // any edges that are already present in the opposite direction.

  map<S2Point, set<S2Point> > edges;
  for (S2CellId id = begin; id != end; id = id.next()) {
    S2Cell cell(id);
    for (int k = 0; k < 4; ++k) {
      S2Point a = cell.GetVertex(k);
      S2Point b = cell.GetVertex((k + 1) & 3);
      if (edges[b].erase(a) == 0) {
        edges[a].insert(b);
      } else if (edges[b].empty()) {
        edges.erase(b);
      }
    }
  }

  // The remaining edges form a single loop.  We simply follow it starting
  // at an arbitrary vertex and build up a list of vertices.

  vector<S2Point> vertices;
  S2Point p = edges.begin()->first;
  while (!edges.empty()) {
    DCHECK_EQ(1, edges[p].size());
    S2Point next = *edges[p].begin();
    vertices.push_back(p);
    edges.erase(p);
    p = next;
  }

  return new S2Loop(vertices);
}

TEST(S2Loop, LoopRelations2) {
  // Construct polygons consisting of a sequence of adjacent cell ids
  // at some fixed level.  Comparing two polygons at the same level
  // ensures that there are no T-vertices.
  for (int iter = 0; iter < 1000; ++iter) {
    S2Testing::Random& rnd = S2Testing::rnd;
    S2CellId begin = S2CellId(rnd.Rand64() | 1);
    if (!begin.is_valid()) continue;
    begin = begin.parent(rnd.Uniform(S2CellId::kMaxLevel));
    S2CellId a_begin = begin.advance(rnd.Skewed(6));
    S2CellId a_end = a_begin.advance(rnd.Skewed(6) + 1);
    S2CellId b_begin = begin.advance(rnd.Skewed(6));
    S2CellId b_end = b_begin.advance(rnd.Skewed(6) + 1);
    if (!a_end.is_valid() || !b_end.is_valid()) continue;

    scoped_ptr<S2Loop> a(MakeCellLoop(a_begin, a_end));
    scoped_ptr<S2Loop> b(MakeCellLoop(b_begin, b_end));
    if (a.get() && b.get()) {
      bool contained = (a_begin <= b_begin && b_end <= a_end);
      bool intersects = (a_begin < b_end && b_begin < a_end);
      VLOG(1) << "Checking " << a->num_vertices() << " vs. "
              << b->num_vertices() << ", contained = " << contained
              << ", intersects = " << intersects;
      EXPECT_EQ(a->Contains(b.get()), contained);
      EXPECT_EQ(a->Intersects(b.get()), intersects);
    } else {
      VLOG(1) << "MakeCellLoop failed to create a loop.";
    }
  }
}

void DebugDumpCrossings(S2Loop const* loop) {
  // This function is useful for debugging.

  LOG(INFO) << "Ortho(v1): " << S2::Ortho(loop->vertex(1));
  printf("Contains(kOrigin): %d\n", loop->Contains(S2::Origin()));
  for (int i = 1; i <= loop->num_vertices(); ++i) {
    S2Point a = S2::Ortho(loop->vertex(i));
    S2Point b = loop->vertex(i-1);
    S2Point c = loop->vertex(i+1);
    S2Point o = loop->vertex(i);
    printf("Vertex %d: [%.17g, %.17g, %.17g], "
           "%d%dR=%d, %d%d%d=%d, R%d%d=%d, inside: %d\n",
           i, loop->vertex(i).x(), loop->vertex(i).y(), loop->vertex(i).z(),
           i-1, i, S2::RobustCCW(b, o, a),
           i+1, i, i-1, S2::RobustCCW(c, o, b),
           i, i+1, S2::RobustCCW(a, o, c),
           S2::OrderedCCW(a, b, c, o));
  }
  for (int i = 0; i < loop->num_vertices() + 2; ++i) {
    S2Point orig = S2::Origin();
    S2Point dest;
    if (i < loop->num_vertices()) {
      dest = loop->vertex(i);
      printf("Origin->%d crosses:", i);
    } else {
      dest = S2Point(0, 0, 1);
      if (i == loop->num_vertices() + 1) orig = loop->vertex(1);
      printf("Case %d:", i);
    }
    for (int j = 0; j < loop->num_vertices(); ++j) {
      printf(" %d", S2EdgeUtil::EdgeOrVertexCrossing(
                 orig, dest, loop->vertex(j), loop->vertex(j+1)));
    }
    printf("\n");
  }
  for (int i = 0; i <= 2; i += 2) {
    printf("Origin->v1 crossing v%d->v1: ", i);
    S2Point a = S2::Ortho(loop->vertex(1));
    S2Point b = loop->vertex(i);
    S2Point c = S2::Origin();
    S2Point o = loop->vertex(1);
    printf("%d1R=%d, M1%d=%d, R1M=%d, crosses: %d\n",
           i, S2::RobustCCW(b, o, a),
           i, S2::RobustCCW(c, o, b),
           S2::RobustCCW(a, o, c),
           S2EdgeUtil::EdgeOrVertexCrossing(c, o, b, a));
  }
}

static void TestNear(char const* a_str, char const* b_str,
                     double max_error, bool expected) {
  scoped_ptr<S2Loop> a(S2Testing::MakeLoop(a_str));
  scoped_ptr<S2Loop> b(S2Testing::MakeLoop(b_str));
  EXPECT_EQ(a->BoundaryNear(b.get(), max_error), expected);
  EXPECT_EQ(b->BoundaryNear(a.get(), max_error), expected);
}

TEST(S2Loop, BoundaryNear) {
  double degree = S1Angle::Degrees(1).radians();

  TestNear("0:0, 0:10, 5:5",
           "0:0.1, -0.1:9.9, 5:5.2",
           0.5 * degree, true);
  TestNear("0:0, 0:3, 0:7, 0:10, 3:7, 5:5",
           "0:0, 0:10, 2:8, 5:5, 4:4, 3:3, 1:1",
           1e-3, true);

  // All vertices close to some edge, but not equivalent.
  TestNear("0:0, 0:2, 2:2, 2:0",
           "0:0, 1.9999:1, 0:2, 2:2, 2:0",
           0.5 * degree, false);

  // Two triangles that backtrack a bit on different edges.  A simple
  // greedy matching algorithm would fail on this example.
  const char* t1 = "0.1:0, 0.1:1, 0.1:2, 0.1:3, 0.1:4, 1:4, 2:4, 3:4, "
                   "2:4.1, 1:4.1, 2:4.2, 3:4.2, 4:4.2, 5:4.2";
  char const* t2 = "0:0, 0:1, 0:2, 0:3, 0.1:2, 0.1:1, 0.2:2, 0.2:3, "
                   "0.2:4, 1:4.1, 2:4, 3:4, 4:4, 5:4";
  TestNear(t1, t2, 1.5 * degree, true);
  TestNear(t1, t2, 0.5 * degree, false);
}

TEST(S2Loop, EncodeDecode) {
  scoped_ptr<S2Loop> l(S2Testing::MakeLoop("30:20, 40:20, 39:43, 33:35"));
  l->set_depth(3);

  Encoder encoder;
  l->Encode(&encoder);

  Decoder decoder(encoder.base(), encoder.length());

  S2Loop decoded_loop;
  ASSERT_TRUE(decoded_loop.Decode(&decoder));
  EXPECT_TRUE(l->BoundaryEquals(&decoded_loop));
  EXPECT_EQ(l->depth(), decoded_loop.depth());
  EXPECT_EQ(l->GetRectBound(), decoded_loop.GetRectBound());
}

TEST(S2Loop, EncodeDecodeWithinScope) {
  scoped_ptr<S2Loop> l(S2Testing::MakeLoop("30:20, 40:20, 39:43, 33:35"));
  l->set_depth(3);
  Encoder encoder;
  l->Encode(&encoder);
  Decoder decoder1(encoder.base(), encoder.length());

  // Initialize the loop using DecodeWithinScope and check that it is the
  // same as the original loop.
  S2Loop loop1;
  ASSERT_TRUE(loop1.DecodeWithinScope(&decoder1));
  EXPECT_TRUE(l->BoundaryEquals(&loop1));
  EXPECT_EQ(l->depth(), loop1.depth());
  EXPECT_EQ(l->GetRectBound(), loop1.GetRectBound());

  // Initialize the same loop using Init with a vector of vertices, and
  // check that it doesn't deallocate the original memory.
  vector<S2Point> vertices;
  vertices.push_back(loop1.vertex(0));
  vertices.push_back(loop1.vertex(2));
  vertices.push_back(loop1.vertex(3));
  loop1.Init(vertices);
  Decoder decoder2(encoder.base(), encoder.length());
  S2Loop loop2;
  ASSERT_TRUE(loop2.DecodeWithinScope(&decoder2));
  EXPECT_TRUE(l->BoundaryEquals(&loop2));
  EXPECT_EQ(l->vertex(1), loop2.vertex(1));
  EXPECT_NE(loop1.vertex(1), loop2.vertex(1));

  // Initialize loop2 using Decode with a decoder on different data.
  // Check that the original memory is not deallocated or overwritten.
  scoped_ptr<S2Loop> l2(S2Testing::MakeLoop("30:40, 40:75, 39:43, 80:35"));
  l2->set_depth(2);
  Encoder encoder2;
  l2->Encode(&encoder2);
  Decoder decoder3(encoder2.base(), encoder2.length());
  ASSERT_TRUE(loop2.Decode(&decoder3));
  Decoder decoder4(encoder.base(), encoder.length());
  ASSERT_TRUE(loop1.DecodeWithinScope(&decoder4));
  EXPECT_TRUE(l->BoundaryEquals(&loop1));
  EXPECT_EQ(l->vertex(1), loop1.vertex(1));
  EXPECT_NE(loop1.vertex(1), loop2.vertex(1));
}

// This test checks that S2Loops created directly from S2Cells behave
// identically to S2Loops created from the vertices of those cells; this
// previously was not the case, because S2Cells calculate their bounding
// rectangles slightly differently, and S2Loops created from them just copied
// the S2Cell bounds.
TEST(S2Loop, S2CellConstructorAndContains) {
  S2Cell cell(S2CellId::FromLatLng(S2LatLng::FromE6(40565459, -74645276)));
  S2Loop cell_as_loop(cell);

  vector<S2Point> vertices;
  for (int i = 0; i < cell_as_loop.num_vertices(); ++i) {
    vertices.push_back(cell_as_loop.vertex(i));
  }
  S2Loop loop_copy(vertices);
  EXPECT_TRUE(loop_copy.Contains(&cell_as_loop));
  EXPECT_TRUE(cell_as_loop.Contains(&loop_copy));
  EXPECT_TRUE(loop_copy.Contains(cell));

  // Demonstrates the reason for this test; the cell bounds are more
  // conservative than the resulting loop bounds.
  EXPECT_FALSE(loop_copy.GetRectBound().Contains(cell.GetRectBound()));
}

TEST(s2loop, IsValidDetectsInvalidLoops) {
  // Only two vertices
  S2Loop* l1 = S2Testing::MakeLoop("20:20, 21:21");
  ASSERT_FALSE(l1->IsValid());
  delete l1;

  // Even if you disable s2debug, non-unit-length vertices break RobustCCW,
  // so there is no point in testing it here.
  // TODO(user): Check if the unit length test in IsValid is redundant
  // and remove it if so.

  // There is a duplicate vertex
  S2Loop* l2 = S2Testing::MakeLoop("20:20, 21:21, 21:20, 20:20, 20:21");
  ASSERT_FALSE(l2->IsValid());
  delete l2;

  // Some edges intersect
  S2Loop* l3 = S2Testing::MakeLoop("20:20, 21:21, 21:20.5, 21:20, 20:21");
  ASSERT_FALSE(l3->IsValid());
  delete l3;
}

TEST(s2loop, IsValidDetectsLargeInvalidLoops) {
}

static void BM_ContainsOrCrosses(int iters, int num_vertices) {
  StopBenchmarkTiming();
  S2Loop* p1 = S2Testing::MakeRegularLoop(S2::Origin(), num_vertices, 0.005);
  S2Loop* p2 = S2Testing::MakeRegularLoop(
      (S2::Origin() + S2Point(0, 0, 0.003)).Normalize(),
      num_vertices,
      0.005);
  StartBenchmarkTiming();
  for (int i = 0; i < iters; ++i) {
    p1->ContainsOrCrosses(p2);
  }
  StopBenchmarkTiming();
  delete p1;
  delete p2;
}
BENCHMARK_RANGE(BM_ContainsOrCrosses, 8, 8192);

static void BM_IsValid(int iters, int num_vertices) {
  StopBenchmarkTiming();
  S2Loop* l = S2Testing::MakeRegularLoop(S2::Origin(), num_vertices, 0.001);
  StartBenchmarkTiming();
  int r = 0;
  for (int i = 0; i < iters; ++i) {
    r += l->IsValid();
  }
  CHECK(r != INT_MAX);
  delete l;
}
BENCHMARK(BM_IsValid)
  ->Arg(32)
  ->Arg(64)
  ->Arg(128)
  ->Arg(256)
  ->Arg(512)
  ->Arg(4096)
  ->Arg(32768);

static void BM_ContainsQuery(int iters, int num_vertices) {
  StopBenchmarkTiming();
  S2Point loop_center = S2Testing::MakePoint("42:10");
  S2Loop* loop = S2Testing::MakeRegularLoop(loop_center,
                                            num_vertices,
                                            7e-3);  // = 5km/6400km
  StartBenchmarkTiming();
  int count = 0;
  for (int i = 0; i < iters; ++i) {
    count += 1 + loop->Contains(loop_center);
  }
  CHECK_LE(0, count);
  delete loop;
}
BENCHMARK_RANGE(BM_ContainsQuery, 4, 1 << 16);
