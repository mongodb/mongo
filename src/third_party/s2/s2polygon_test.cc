// Copyright 2005 Google Inc. All Rights Reserved.
//
// To run the benchmarks, use:

#include "s2polygon.h"

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <cstdio>
#include <string>
using std::string;

#include <vector>
using std::vector;


#include "base/commandlineflags.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/scoped_ptr.h"
#include "strings/stringprintf.h"
#include "testing/base/public/benchmark.h"
#include "testing/base/public/gunit.h"
#include "util/coding/coder.h"
#include "s2.h"
#include "s2cap.h"
#include "s2cellunion.h"
#include "s2latlng.h"
#include "s2loop.h"
#include "s2polygonbuilder.h"
#include "s2polyline.h"
#include "s2regioncoverer.h"
#include "s2testing.h"
#include "util/math/matrix3x3.h"
#include "util/math/matrix3x3-inl.h"

DEFINE_int32(num_loops_per_polygon_for_bm,
             10,
             "Number of loops per polygon to use for an s2polygon "
             "encode/decode benchmark. Can be a maximum of 90.");

// A set of nested loops around the point 0:0 (lat:lng).
// Every vertex of kNear0 is a vertex of kNear1.
char const kNearPoint[] = "0:0";
string const kNear0 = "-1:0, 0:1, 1:0, 0:-1;";
string const kNear1 = "-1:-1, -1:0, -1:1, 0:1, 1:1, 1:0, 1:-1, 0:-1;";
string const kNear2 = "5:-2, -2:5, -1:-2;";
string const kNear3 = "6:-3, -3:6, -2:-2;";
string const kNearHemi = "0:-90, -90:0, 0:90, 90:0;";

// A set of nested loops around the point 0:180 (lat:lng).
// Every vertex of kFar0 and kFar2 belongs to kFar1, and all
// the loops except kFar2 are non-convex.
string const kFar0 = "0:179, 1:180, 0:-179, 2:-180;";
string const kFar1 =
  "0:179, -1:179, 1:180, -1:-179, 0:-179, 3:-178, 2:-180, 3:178;";
string const kFar2 = "-1:-179, -1:179, 3:178, 3:-178;";  // opposite direction
string const kFar3 = "-3:-178, -2:179, -3:178, 4:177, 4:-177;";
string const kFarHemi = "0:-90, 60:90, -60:90;";

// A set of nested loops around the point -90:0 (lat:lng).
string const kSouthPoint = "-89.9999:0.001";
string const kSouth0a = "-90:0, -89.99:0, -89.99:0.01;";
string const kSouth0b = "-90:0, -89.99:0.02, -89.99:0.03;";
string const kSouth0c = "-90:0, -89.99:0.04, -89.99:0.05;";
string const kSouth1 = "-90:0, -89.9:-0.1, -89.9:0.1;";
string const kSouth2 = "-90:0, -89.8:-0.2, -89.8:0.2;";
string const kSouthHemi = "0:-180, 0:60, 0:-60;";

// Two different loops that surround all the Near and Far loops except
// for the hemispheres.
string const kNearFar1 = "-1:-9, -9:-9, -9:9, 9:9, 9:-9, 1:-9, "
                         "1:-175, 9:-175, 9:175, -9:175, -9:-175, -1:-175;";
string const kNearFar2 = "-8:-4, 8:-4, 2:15, 2:170, "
                         "8:-175, -8:-175, -2:170, -2:15;";

// Loops that result from intersection of other loops.
string const kFarHSouthH = "0:-180, 0:90, -60:90, 0:-90;";

// Rectangles that form a cross, with only shared vertices, no crossing edges.
// Optional holes outside the intersecting region.
string const kCross1 = "-2:1, -1:1, 1:1, 2:1, 2:-1, 1:-1, -1:-1, -2:-1;";
string const kCross1SideHole = "-1.5:0.5, -1.2:0.5, -1.2:-0.5, -1.5:-0.5;";
string const kCross2 = "1:-2, 1:-1, 1:1, 1:2, -1:2, -1:1, -1:-1, -1:-2;";
string const kCross2SideHole = "0.5:-1.5, 0.5:-1.2, -0.5:-1.2, -0.5:-1.5;";
string const kCrossCenterHole = "-0.5:0.5, 0.5:0.5, 0.5:-0.5, -0.5:-0.5;";

// Two rectangles that intersect, but no edges cross and there's always
// local containment (rather than crossing) at each shared vertex.
// In this ugly ASCII art, 1 is A+B, 2 is B+C:
//      +---+---+---+
//      | A | B | C |
//      +---+---+---+
string const kOverlap1 = "0:1, 1:1, 2:1, 2:0, 1:0, 0:0;";
string const kOverlap1SideHole = "0.2:0.8, 0.8:0.8, 0.8:0.2, 0.2:0.2;";
string const kOverlap2 = "1:1, 2:1, 3:1, 3:0, 2:0, 1:0;";
string const kOverlap2SideHole = "2.2:0.8, 2.8:0.8, 2.8:0.2, 2.2:0.2;";
string const kOverlapCenterHole = "1.2:0.8, 1.8:0.8, 1.8:0.2, 1.2:0.2;";

class S2PolygonTestBase : public testing::Test {
 public:
  S2PolygonTestBase();
  ~S2PolygonTestBase();
 protected:
  // Some standard polygons to use in the tests.
  S2Polygon const* const near_0;
  S2Polygon const* const near_10;
  S2Polygon const* const near_30;
  S2Polygon const* const near_32;
  S2Polygon const* const near_3210;
  S2Polygon const* const near_H3210;

  S2Polygon const* const far_10;
  S2Polygon const* const far_21;
  S2Polygon const* const far_321;
  S2Polygon const* const far_H20;
  S2Polygon const* const far_H3210;

  S2Polygon const* const south_0ab;
  S2Polygon const* const south_2;
  S2Polygon const* const south_210b;
  S2Polygon const* const south_H21;
  S2Polygon const* const south_H20abc;

  S2Polygon const* const nf1_n10_f2_s10abc;

  S2Polygon const* const nf2_n2_f210_s210ab;

  S2Polygon const* const f32_n0;
  S2Polygon const* const n32_s0b;

  S2Polygon const* const cross1;
  S2Polygon const* const cross1_side_hole;
  S2Polygon const* const cross1_center_hole;
  S2Polygon const* const cross2;
  S2Polygon const* const cross2_side_hole;
  S2Polygon const* const cross2_center_hole;

  S2Polygon const* const overlap1;
  S2Polygon const* const overlap1_side_hole;
  S2Polygon const* const overlap1_center_hole;
  S2Polygon const* const overlap2;
  S2Polygon const* const overlap2_side_hole;
  S2Polygon const* const overlap2_center_hole;

  S2Polygon const* const far_H;
  S2Polygon const* const south_H;
  S2Polygon const* const far_H_south_H;
};

static S2Polygon* MakePolygon(string const& str) {
  scoped_ptr<S2Polygon> polygon(S2Testing::MakePolygon(str));
  Encoder encoder;
  polygon->Encode(&encoder);
  Decoder decoder(encoder.base(), encoder.length());
  scoped_ptr<S2Polygon> decoded_polygon(new S2Polygon);
  decoded_polygon->Decode(&decoder);
  return decoded_polygon.release();
}

static void CheckContains(string const& a_str, string const& b_str) {
  S2Polygon* a = MakePolygon(a_str);
  S2Polygon* b = MakePolygon(b_str);
  scoped_ptr<S2Polygon> delete_a(a);
  scoped_ptr<S2Polygon> delete_b(b);
  EXPECT_TRUE(a->Contains(b));
  EXPECT_TRUE(a->ApproxContains(b, S1Angle::Radians(1e-15)));
}

static void CheckContainsPoint(string const& a_str, string const& b_str) {
  scoped_ptr<S2Polygon> a(S2Testing::MakePolygon(a_str));
  EXPECT_TRUE(a->VirtualContainsPoint(S2Testing::MakePoint(b_str)))
    << " " << a_str << " did not contain " << b_str;
}

TEST(S2Polygon, Init) {
  CheckContains(kNear1, kNear0);
  CheckContains(kNear2, kNear1);
  CheckContains(kNear3, kNear2);
  CheckContains(kNearHemi, kNear3);
  CheckContains(kFar1, kFar0);
  CheckContains(kFar2, kFar1);
  CheckContains(kFar3, kFar2);
  CheckContains(kFarHemi, kFar3);
  CheckContains(kSouth1, kSouth0a);
  CheckContains(kSouth1, kSouth0b);
  CheckContains(kSouth1, kSouth0c);
  CheckContains(kSouthHemi, kSouth2);
  CheckContains(kNearFar1, kNear3);
  CheckContains(kNearFar1, kFar3);
  CheckContains(kNearFar2, kNear3);
  CheckContains(kNearFar2, kFar3);

  CheckContainsPoint(kNear0, kNearPoint);
  CheckContainsPoint(kNear1, kNearPoint);
  CheckContainsPoint(kNear2, kNearPoint);
  CheckContainsPoint(kNear3, kNearPoint);
  CheckContainsPoint(kNearHemi, kNearPoint);
  CheckContainsPoint(kSouth0a, kSouthPoint);
  CheckContainsPoint(kSouth1, kSouthPoint);
  CheckContainsPoint(kSouth2, kSouthPoint);
  CheckContainsPoint(kSouthHemi, kSouthPoint);
}

S2PolygonTestBase::S2PolygonTestBase():
    near_0(MakePolygon(kNear0)),
    near_10(MakePolygon(kNear0 + kNear1)),
    near_30(MakePolygon(kNear3 + kNear0)),
    near_32(MakePolygon(kNear2 + kNear3)),
    near_3210(MakePolygon(kNear0 + kNear2 + kNear3 + kNear1)),
    near_H3210(MakePolygon(kNear0 + kNear2 + kNear3 + kNearHemi + kNear1)),

    far_10(MakePolygon(kFar0 + kFar1)),
    far_21(MakePolygon(kFar2 + kFar1)),
    far_321(MakePolygon(kFar2 + kFar3 + kFar1)),
    far_H20(MakePolygon(kFar2 + kFarHemi + kFar0)),
    far_H3210(MakePolygon(kFar2 + kFarHemi + kFar0 + kFar1 + kFar3)),

    south_0ab(MakePolygon(kSouth0a + kSouth0b)),
    south_2(MakePolygon(kSouth2)),
    south_210b(MakePolygon(kSouth2 + kSouth0b + kSouth1)),
    south_H21(MakePolygon(kSouth2 + kSouthHemi + kSouth1)),
    south_H20abc(MakePolygon(
                     kSouth2 + kSouth0b + kSouthHemi + kSouth0a + kSouth0c)),

    nf1_n10_f2_s10abc(MakePolygon(kSouth0c + kFar2 + kNear1 + kNearFar1 +
                                  kNear0 + kSouth1 + kSouth0b + kSouth0a)),

    nf2_n2_f210_s210ab(MakePolygon(kFar2 + kSouth0a + kFar1 + kSouth1 + kFar0 +
                                   kSouth0b + kNearFar2 + kSouth2 + kNear2)),

    f32_n0(MakePolygon(kFar2 + kNear0 + kFar3)),
    n32_s0b(MakePolygon(kNear3 + kSouth0b + kNear2)),

    cross1(MakePolygon(kCross1)),
    cross1_side_hole(MakePolygon(kCross1 + kCross1SideHole)),
    cross1_center_hole(MakePolygon(kCross1 + kCrossCenterHole)),
    cross2(MakePolygon(kCross2)),
    cross2_side_hole(MakePolygon(kCross2 + kCross2SideHole)),
    cross2_center_hole(MakePolygon(kCross2 + kCrossCenterHole)),

    overlap1(MakePolygon(kOverlap1)),
    overlap1_side_hole(MakePolygon(kOverlap1 + kOverlap1SideHole)),
    overlap1_center_hole(MakePolygon(kOverlap1 + kOverlapCenterHole)),
    overlap2(MakePolygon(kOverlap2)),
    overlap2_side_hole(MakePolygon(kOverlap2 + kOverlap2SideHole)),
    overlap2_center_hole(MakePolygon(kOverlap2 + kOverlapCenterHole)),

    far_H(MakePolygon(kFarHemi)),
    south_H(MakePolygon(kSouthHemi)),
    far_H_south_H(MakePolygon(kFarHSouthH))
{}

S2PolygonTestBase::~S2PolygonTestBase() {
  delete near_0;
  delete near_10;
  delete near_30;
  delete near_32;
  delete near_3210;
  delete near_H3210;

  delete far_10;
  delete far_21;
  delete far_321;
  delete far_H20;
  delete far_H3210;

  delete south_0ab;
  delete south_2;
  delete south_210b;
  delete south_H21;
  delete south_H20abc;

  delete nf1_n10_f2_s10abc;

  delete nf2_n2_f210_s210ab;

  delete f32_n0;
  delete n32_s0b;

  delete cross1;
  delete cross1_side_hole;
  delete cross1_center_hole;
  delete cross2;
  delete cross2_side_hole;
  delete cross2_center_hole;

  delete overlap1;
  delete overlap1_side_hole;
  delete overlap1_center_hole;
  delete overlap2;
  delete overlap2_side_hole;
  delete overlap2_center_hole;

  delete far_H;
  delete south_H;
  delete far_H_south_H;
}

static void CheckEqual(S2Polygon const* a, S2Polygon const* b,
                       double max_error = 1e-31) {
  if (a->IsNormalized() && b->IsNormalized()) {
    ASSERT_TRUE(a->BoundaryApproxEquals(b, max_error));
  } else {
    S2PolygonBuilder builder(S2PolygonBuilderOptions::DIRECTED_XOR());
    S2Polygon a2, b2;
    builder.AddPolygon(a);
    ASSERT_TRUE(builder.AssemblePolygon(&a2, NULL));
    builder.AddPolygon(b);
    ASSERT_TRUE(builder.AssemblePolygon(&b2, NULL));
    ASSERT_TRUE(a2.BoundaryApproxEquals(&b2, max_error));
  }
}

static void TestUnion(S2Polygon const* a, S2Polygon const* b) {
  S2Polygon c_union;
  c_union.InitToUnion(a, b);

  vector<S2Polygon*> polygons;
  polygons.push_back(new S2Polygon);
  polygons.back()->Copy(a);
  polygons.push_back(new S2Polygon);
  polygons.back()->Copy(b);
  scoped_ptr<S2Polygon> c_destructive_union(
      S2Polygon::DestructiveUnion(&polygons));

  CheckEqual(&c_union, c_destructive_union.get());
}

static void TestContains(S2Polygon const* a, S2Polygon const* b) {
  S2Polygon c, d, e;
  c.InitToUnion(a, b);
  CheckEqual(&c, a);
  TestUnion(a, b);

  d.InitToIntersection(a, b);
  CheckEqual(&d, b);

  e.InitToDifference(b, a);
  EXPECT_EQ(0, e.num_loops());
}

TEST(S2Polygon, TestApproxContains) {
  // Get a random S2Cell as a polygon.
  S2CellId id = S2CellId::FromLatLng(S2LatLng::FromE6(40565459, -74645276));
  S2Cell cell(id.parent(10));
  S2Polygon cell_as_polygon(cell);

  // We want to roughly bisect the polygon, so we make a rectangle that is the
  // top half of the current polygon's bounding rectangle.
  S2LatLngRect const& bounds = cell_as_polygon.GetRectBound();
  S2LatLngRect upper_half = bounds;
  upper_half.mutable_lat()->set_lo(bounds.lat().GetCenter());

  // Turn the S2LatLngRect into an S2Polygon
  vector<S2Point> points;
  for (int i = 0; i < 4; i++)
    points.push_back(upper_half.GetVertex(i).ToPoint());
  vector<S2Loop*> loops;
  loops.push_back(new S2Loop(points));
  S2Polygon upper_half_polygon(&loops);

  // Get the intersection. There is no guarantee that the intersection will be
  // contained by A or B.
  S2Polygon intersection;
  intersection.InitToIntersection(&cell_as_polygon, &upper_half_polygon);
  EXPECT_FALSE(cell_as_polygon.Contains(&intersection));

  EXPECT_TRUE(
      cell_as_polygon.ApproxContains(&intersection,
                                     S2EdgeUtil::kIntersectionTolerance));
}

static void TestDisjoint(S2Polygon const* a, S2Polygon const* b) {
  S2PolygonBuilder builder(S2PolygonBuilderOptions::DIRECTED_XOR());
  builder.AddPolygon(a);
  builder.AddPolygon(b);
  S2Polygon ab;
  ASSERT_TRUE(builder.AssemblePolygon(&ab, NULL));

  S2Polygon c, d, e, f;
  c.InitToUnion(a, b);
  CheckEqual(&c, &ab);
  TestUnion(a, b);

  d.InitToIntersection(a, b);
  EXPECT_EQ(0, d.num_loops());

  e.InitToDifference(a, b);
  CheckEqual(&e, a);

  f.InitToDifference(b, a);
  CheckEqual(&f, b);
}

static void TestRelationWithDesc(S2Polygon const* a, S2Polygon const* b,
                                 int contains, bool intersects,
                                 const char *test_description) {
  SCOPED_TRACE(test_description);
  EXPECT_EQ(contains > 0, a->Contains(b));
  EXPECT_EQ(contains < 0, b->Contains(a));
  EXPECT_EQ(intersects, a->Intersects(b));
  if (contains > 0) {
    TestContains(a, b);
  } else if (contains < 0) {
    TestContains(b, a);
  }
  if (!intersects) {
    TestDisjoint(a, b);
  }
}

TEST_F(S2PolygonTestBase, Relations) {
#define TestRelation(a, b, contains, intersects) \
    TestRelationWithDesc(a, b, contains, intersects, "args " #a ", " #b)
  TestRelation(near_10, near_30, -1, true);
  TestRelation(near_10, near_32, 0, false);
  TestRelation(near_10, near_3210, -1, true);
  TestRelation(near_10, near_H3210, 0, false);
  TestRelation(near_30, near_32, 1, true);
  TestRelation(near_30, near_3210, 1, true);
  TestRelation(near_30, near_H3210, 0, true);
  TestRelation(near_32, near_3210, -1, true);
  TestRelation(near_32, near_H3210, 0, false);
  TestRelation(near_3210, near_H3210, 0, false);

  TestRelation(far_10, far_21, 0, false);
  TestRelation(far_10, far_321, -1, true);
  TestRelation(far_10, far_H20, 0, false);
  TestRelation(far_10, far_H3210, 0, false);
  TestRelation(far_21, far_321, 0, false);
  TestRelation(far_21, far_H20, 0, false);
  TestRelation(far_21, far_H3210, -1, true);
  TestRelation(far_321, far_H20, 0, true);
  TestRelation(far_321, far_H3210, 0, true);
  TestRelation(far_H20, far_H3210, 0, true);

  TestRelation(south_0ab, south_2, -1, true);
  TestRelation(south_0ab, south_210b, 0, true);
  TestRelation(south_0ab, south_H21, -1, true);
  TestRelation(south_0ab, south_H20abc, -1, true);
  TestRelation(south_2, south_210b, 1, true);
  TestRelation(south_2, south_H21, 0, true);
  TestRelation(south_2, south_H20abc, 0, true);
  TestRelation(south_210b, south_H21, 0, true);
  TestRelation(south_210b, south_H20abc, 0, true);
  TestRelation(south_H21, south_H20abc, 1, true);

  TestRelation(nf1_n10_f2_s10abc, nf2_n2_f210_s210ab, 0, true);
  TestRelation(nf1_n10_f2_s10abc, near_32, 1, true);
  TestRelation(nf1_n10_f2_s10abc, far_21, 0, false);
  TestRelation(nf1_n10_f2_s10abc, south_0ab, 0, false);
  TestRelation(nf1_n10_f2_s10abc, f32_n0, 1, true);

  TestRelation(nf2_n2_f210_s210ab, near_10, 0, false);
  TestRelation(nf2_n2_f210_s210ab, far_10, 1, true);
  TestRelation(nf2_n2_f210_s210ab, south_210b, 1, true);
  TestRelation(nf2_n2_f210_s210ab, south_0ab, 1, true);
  TestRelation(nf2_n2_f210_s210ab, n32_s0b, 1, true);

  TestRelation(cross1, cross2, 0, true);
  TestRelation(cross1_side_hole, cross2, 0, true);
  TestRelation(cross1_center_hole, cross2, 0, true);
  TestRelation(cross1, cross2_side_hole, 0, true);
  TestRelation(cross1, cross2_center_hole, 0, true);
  TestRelation(cross1_side_hole, cross2_side_hole, 0, true);
  TestRelation(cross1_center_hole, cross2_side_hole, 0, true);
  TestRelation(cross1_side_hole, cross2_center_hole, 0, true);
  TestRelation(cross1_center_hole, cross2_center_hole, 0, true);

  // These cases, when either polygon has a hole, test a different code path
  // from the other cases.
  TestRelation(overlap1, overlap2, 0, true);
  TestRelation(overlap1_side_hole, overlap2, 0, true);
  TestRelation(overlap1_center_hole, overlap2, 0, true);
  TestRelation(overlap1, overlap2_side_hole, 0, true);
  TestRelation(overlap1, overlap2_center_hole, 0, true);
  TestRelation(overlap1_side_hole, overlap2_side_hole, 0, true);
  TestRelation(overlap1_center_hole, overlap2_side_hole, 0, true);
  TestRelation(overlap1_side_hole, overlap2_center_hole, 0, true);
  TestRelation(overlap1_center_hole, overlap2_center_hole, 0, true);
#undef TestRelation
}

struct TestCase {
  char const* a;
  char const* b;
  char const* a_and_b;
  char const* a_or_b;
  char const* a_minus_b;
};

TestCase test_cases[] = {
  // Two triangles that share an edge.
  { "4:2, 3:1, 3:3;",

    "3:1, 2:2, 3:3;",

    "",

    "4:2, 3:1, 2:2, 3:3;",

    "4:2, 3:1, 3:3;"
  },

  // Two vertical bars and a horizontal bar connecting them.
  { "0:0, 0:2, 3:2, 3:0;   0:3, 0:5, 3:5, 3:3;",

    "1:1, 1:4, 2:4, 2:1;",

    "1:1, 1:2, 2:2, 2:1;   1:3, 1:4, 2:4, 2:3;",

    "0:0, 0:2, 1:2, 1:3, 0:3, 0:5, 3:5, 3:3, 2:3, 2:2, 3:2, 3:0;",

    "0:0, 0:2, 1:2, 1:1, 2:1, 2:2, 3:2, 3:0;   "
    "0:3, 0:5, 3:5, 3:3, 2:3, 2:4, 1:4, 1:3;"
  },

  // Two vertical bars and two horizontal bars centered around S2::Origin().
  { "1:88, 1:93, 2:93, 2:88;   -1:88, -1:93, 0:93, 0:88;",

    "-2:89, -2:90, 3:90, 3:89;   -2:91, -2:92, 3:92, 3:91;",

    "1:89, 1:90, 2:90, 2:89;   1:91, 1:92, 2:92, 2:91;   "
    "-1:89, -1:90, 0:90, 0:89;   -1:91, -1:92, 0:92, 0:91;",

    "-1:88, -1:89, -2:89, -2:90, -1:90, -1:91, -2:91, -2:92, -1:92, -1:93,"
    "0:93, 0:92, 1:92, 1:93, 2:93, 2:92, 3:92, 3:91, 2:91, 2:90, 3:90,"
    "3:89, 2:89, 2:88, 1:88, 1:89, 0:89, 0:88;   "
    "0:90, 0:91, 1:91, 1:90;",

    "1:88, 1:89, 2:89, 2:88;   1:90, 1:91, 2:91, 2:90;   "
    "1:92, 1:93, 2:93, 2:92;   -1:88, -1:89, 0:89, 0:88;   "
    "-1:90, -1:91, 0:91, 0:90;   -1:92, -1:93, 0:93, 0:92;"
  },

  // Two interlocking square doughnuts centered around -S2::Origin().
  { "-1:-93, -1:-89, 3:-89, 3:-93;   0:-92, 0:-90, 2:-90, 2:-92;",

    "-3:-91, -3:-87, 1:-87, 1:-91;   -2:-90, -2:-88, 0:-88, 0:-90;",

    "-1:-91, -1:-90, 0:-90, 0:-91;   0:-90, 0:-89, 1:-89, 1:-90;",

    "-1:-93, -1:-91, -3:-91, -3:-87, 1:-87, 1:-89, 3:-89, 3:-93;   "
    "0:-92, 0:-91, 1:-91, 1:-90, 2:-90, 2:-92;   "
    "-2:-90, -2:-88, 0:-88, 0:-89, -1:-89, -1:-90;",

    "-1:-93, -1:-91, 0:-91, 0:-92, 2:-92, 2:-90, 1:-90, 1:-89, 3:-89, 3:-93;   "
    "-1:-90, -1:-89, 0:-89, 0:-90;"
  },

  // An incredibly thin triangle intersecting a square, such that the two
  // intersection points of the triangle with the square are identical.
  // This results in a degenerate loop that needs to be handled correctly.
  { "10:44, 10:46, 12:46, 12:44;",

    "11:45, 89:45.00000000000001, 90:45;",

    "",  // Empty intersection!

    // Original square with extra vertex, and triangle disappears (due to
    // default vertex_merge_radius of S2EdgeUtil::kIntersectionTolerance).
    "10:44, 10:46, 12:46, 12:45, 12:44;",

    "10:44, 10:46, 12:46, 12:45, 12:44;"
  },
};

TEST_F(S2PolygonTestBase, Operations) {
  S2Polygon far_south;
  far_south.InitToIntersection(far_H, south_H);
  CheckEqual(&far_south, far_H_south_H);

  for (int i = 0; i < arraysize(test_cases); ++i) {
    SCOPED_TRACE(StringPrintf("Polygon operation test case %d", i));
    TestCase* test = test_cases + i;
    scoped_ptr<S2Polygon> a(MakePolygon(test->a));
    scoped_ptr<S2Polygon> b(MakePolygon(test->b));
    scoped_ptr<S2Polygon> expected_a_and_b(MakePolygon(test->a_and_b));
    scoped_ptr<S2Polygon> expected_a_or_b(MakePolygon(test->a_or_b));
    scoped_ptr<S2Polygon> expected_a_minus_b(MakePolygon(test->a_minus_b));

    // The intersections in the "expected" data were computed in lat-lng
    // space, while the actual intersections are computed using geodesics.
    // The error due to this depends on the length and direction of the line
    // segment being intersected, and how close the intersection is to the
    // endpoints of the segment.  The worst case is for a line segment between
    // two points at the same latitude, where the intersection point is in the
    // middle of the segment.  In this case the error is approximately
    // (p * t^2) / 8, where "p" is the absolute latitude in radians, "t" is
    // the longitude difference in radians, and both "p" and "t" are small.
    // The test cases all have small latitude and longitude differences.
    // If "p" and "t" are converted to degrees, the following error bound is
    // valid as long as (p * t^2 < 150).

    static double const kMaxError = 1e-4;

    S2Polygon a_and_b, a_or_b, a_minus_b;
    a_and_b.InitToIntersection(a.get(), b.get());
    CheckEqual(&a_and_b, expected_a_and_b.get(), kMaxError);
    a_or_b.InitToUnion(a.get(), b.get());
    TestUnion(a.get(), b.get());
    CheckEqual(&a_or_b, expected_a_or_b.get(), kMaxError);
    a_minus_b.InitToDifference(a.get(), b.get());
    CheckEqual(&a_minus_b, expected_a_minus_b.get(), kMaxError);
  }
}

void ClearPolylineVector(vector<S2Polyline*>* polylines) {
  for (vector<S2Polyline*>::const_iterator it = polylines->begin();
       it != polylines->end(); ++it) {
    delete *it;
  }
  polylines->clear();
}

static void PolylineIntersectionSharedEdgeTest(const S2Polygon *p,
                                               int start_vertex,
                                               int direction) {
  SCOPED_TRACE(StringPrintf("Polyline intersection shared edge test "
                            " start=%d direction=%d",
                            start_vertex, direction));
  vector<S2Point> points;
  points.push_back(p->loop(0)->vertex(start_vertex));
  points.push_back(p->loop(0)->vertex(start_vertex + direction));
  S2Polyline polyline(points);
  vector<S2Polyline*> polylines;
  if (direction < 0) {
    p->IntersectWithPolyline(&polyline, &polylines);
    EXPECT_EQ(0, polylines.size());
    ClearPolylineVector(&polylines);
    p->SubtractFromPolyline(&polyline, &polylines);
    ASSERT_EQ(1, polylines.size());
    ASSERT_EQ(2, polylines[0]->num_vertices());
    EXPECT_EQ(points[0], polylines[0]->vertex(0));
    EXPECT_EQ(points[1], polylines[0]->vertex(1));
  } else {
    p->IntersectWithPolyline(&polyline, &polylines);
    ASSERT_EQ(1, polylines.size());
    ASSERT_EQ(2, polylines[0]->num_vertices());
    EXPECT_EQ(points[0], polylines[0]->vertex(0));
    EXPECT_EQ(points[1], polylines[0]->vertex(1));
    ClearPolylineVector(&polylines);
    p->SubtractFromPolyline(&polyline, &polylines);
    EXPECT_EQ(0, polylines.size());
  }
  ClearPolylineVector(&polylines);
}

// This tests polyline-polyline intersections.
// It covers the same edge cases as TestOperations and also adds some
// extra tests for shared edges.
TEST_F(S2PolygonTestBase, PolylineIntersection) {
  for (int v = 0; v < 3; ++v) {
    PolylineIntersectionSharedEdgeTest(cross1, v, 1);
    PolylineIntersectionSharedEdgeTest(cross1, v + 1, -1);
    PolylineIntersectionSharedEdgeTest(cross1_side_hole, v, 1);
    PolylineIntersectionSharedEdgeTest(cross1_side_hole, v + 1, -1);
  }

  // See comments in TestOperations about the vlue of this constant.
  static double const kMaxError = 1e-4;

  // This duplicates some of the tests in TestOperations by
  // converting the outline of polygon A to a polyline then intersecting
  // it with the polygon B. It then converts B to a polyline and intersects
  // it with A. It then feeds all of the results into a polygon builder and
  // tests that the output is equal to doing an intersection between A and B.
  for (int i = 0; i < arraysize(test_cases); ++i) {
    SCOPED_TRACE(StringPrintf("Polyline intersection test case %d", i));
    TestCase* test = test_cases + i;
    scoped_ptr<S2Polygon> a(MakePolygon(test->a));
    scoped_ptr<S2Polygon> b(MakePolygon(test->b));
    scoped_ptr<S2Polygon> expected_a_and_b(MakePolygon(test->a_and_b));

    vector<S2Point> points;
    vector<S2Polyline *> polylines;
    for (int ab = 0; ab < 2; ab++) {
      S2Polygon *tmp = ab ? a.get() : b.get();
      S2Polygon *tmp2 = ab ? b.get() : a.get();
      for (int l = 0; l < tmp->num_loops(); l++) {
        points.clear();
        if (tmp->loop(l)->is_hole()) {
          for (int v = tmp->loop(l)->num_vertices(); v >=0 ; v--) {
            points.push_back(tmp->loop(l)->vertex(v));
          }
        } else {
          for (int v = 0; v <= tmp->loop(l)->num_vertices(); v++) {
            points.push_back(tmp->loop(l)->vertex(v));
          }
        }
        S2Polyline polyline(points);
        vector<S2Polyline *> tmp;
        tmp2->IntersectWithPolyline(&polyline, &tmp);
        polylines.insert(polylines.end(), tmp.begin(), tmp.end());
      }
    }

    S2PolygonBuilder builder(S2PolygonBuilderOptions::DIRECTED_XOR());
    for (int i = 0; i < polylines.size(); i++) {
      for (int j = 0; j < polylines[i]->num_vertices() - 1; j++) {
        builder.AddEdge(polylines[i]->vertex(j), polylines[i]->vertex(j + 1));
        VLOG(3) << " ... Adding edge: " << polylines[i]->vertex(j) << " - " <<
            polylines[i]->vertex(j + 1);
      }
    }
    ClearPolylineVector(&polylines);

    S2Polygon a_and_b;
    ASSERT_TRUE(builder.AssemblePolygon(&a_and_b, NULL));
    CheckEqual(&a_and_b, expected_a_and_b.get(), kMaxError);
  }
}

// Remove a random polygon from "pieces" and return it.
static S2Polygon* ChoosePiece(vector<S2Polygon*> *pieces) {
  int i = S2Testing::rnd.Uniform(pieces->size());
  S2Polygon* result = (*pieces)[i];
  pieces->erase(pieces->begin() + i);
  return result;
}

static void SplitAndAssemble(S2Polygon const* polygon) {
  S2PolygonBuilder builder(S2PolygonBuilderOptions::DIRECTED_XOR());
  S2Polygon expected;
  builder.AddPolygon(polygon);
  ASSERT_TRUE(builder.AssemblePolygon(&expected, NULL));

  for (int iter = 0; iter < 10; ++iter) {
    S2RegionCoverer coverer;
    // Compute the minimum level such that the polygon's bounding
    // cap is guaranteed to be cut.
    double diameter = 2 * polygon->GetCapBound().angle().radians();
    int min_level = S2::kMaxWidth.GetMinLevel(diameter);

    // TODO: Choose a level that will have up to 256 cells in the covering.
    int level = min_level + S2Testing::rnd.Uniform(4);
    coverer.set_min_level(min_level);
    coverer.set_max_level(level);
    coverer.set_max_cells(500);

    vector<S2CellId> cells;
    coverer.GetCovering(*polygon, &cells);
    S2CellUnion covering;
    covering.Init(cells);
    S2Testing::CheckCovering(*polygon, covering, false);
    VLOG(2) << cells.size() << " cells in covering";
    vector<S2Polygon*> pieces;
    for (int i = 0; i < cells.size(); ++i) {
      S2Cell cell(cells[i]);
      S2Polygon window(cell);
      S2Polygon* piece = new S2Polygon;
      piece->InitToIntersection(polygon, &window);
      pieces.push_back(piece);
      VLOG(4) << "\nPiece " << i << ":\n  Window: "
              << S2Testing::ToString(&window)
              << "\n  Piece: " << S2Testing::ToString(piece);
    }

    // Now we repeatedly remove two random pieces, compute their union, and
    // insert the result as a new piece until only one piece is left.
    //
    // We don't use S2Polygon::DestructiveUnion() because it joins the pieces
    // in a mostly deterministic order.  We don't just call random_shuffle()
    // on the pieces and repeatedly join the last two pieces in the vector
    // because this always joins a single original piece to the current union
    // rather than doing the unions according to a random tree structure.
    while (pieces.size() > 1) {
      scoped_ptr<S2Polygon> a(ChoosePiece(&pieces));
      scoped_ptr<S2Polygon> b(ChoosePiece(&pieces));
      S2Polygon* c = new S2Polygon;
      c->InitToUnion(a.get(), b.get());
      pieces.push_back(c);
      VLOG(4) << "\nJoining piece a: " << S2Testing::ToString(a.get())
              << "\n  With piece b: " << S2Testing::ToString(b.get())
              << "\n  To get piece c: " << S2Testing::ToString(c);
    }
    scoped_ptr<S2Polygon> result(pieces[0]);
    pieces.pop_back();
 
    // The moment of truth!
    EXPECT_TRUE(expected.BoundaryNear(result.get()))
        << "\nActual:\n" << S2Testing::ToString(result.get())
        << "\nExpected:\n" << S2Testing::ToString(&expected);
  }
}

TEST_F(S2PolygonTestBase, Splitting) {
  // It takes too long to test all the polygons in debug mode, so we just pick
  // out some of the more interesting ones.

  SplitAndAssemble(near_H3210);
  SplitAndAssemble(far_H3210);
  SplitAndAssemble(south_0ab);
  SplitAndAssemble(south_210b);
  SplitAndAssemble(south_H20abc);
  SplitAndAssemble(nf1_n10_f2_s10abc);
  SplitAndAssemble(nf2_n2_f210_s210ab);
  SplitAndAssemble(far_H);
  SplitAndAssemble(south_H);
  SplitAndAssemble(far_H_south_H);
}

TEST(S2Polygon, InitToCellUnionBorder) {
  // Test S2Polygon::InitToCellUnionBorder().
  // The main thing to check is that adjacent cells of different sizes get
  // merged correctly.  To do this we generate two random adjacent cells,
  // convert to polygon, and make sure the polygon only has a single loop.
  for (int iter = 0; iter < 500; ++iter) {
    SCOPED_TRACE(StringPrintf("Iteration %d", iter));

    // Choose a random non-leaf cell.
    S2CellId big_cell =
        S2Testing::GetRandomCellId(S2Testing::rnd.Uniform(S2CellId::kMaxLevel));
    // Get all neighbors at some smaller level.
    int small_level = big_cell.level() +
        S2Testing::rnd.Uniform(min(16, S2CellId::kMaxLevel - big_cell.level()));
    vector<S2CellId> neighbors;
    big_cell.AppendAllNeighbors(small_level, &neighbors);
    // Pick one at random.
    S2CellId small_cell = neighbors[S2Testing::rnd.Uniform(neighbors.size())];
    // If it's diagonally adjacent, bail out.
    S2CellId edge_neighbors[4];
    big_cell.GetEdgeNeighbors(edge_neighbors);
    bool diagonal = true;
    for (int i = 0; i < 4; ++i) {
      if (edge_neighbors[i].contains(small_cell)) {
        diagonal = false;
      }
    }
    VLOG(3) << iter << ": big_cell " << big_cell <<
        " small_cell " << small_cell;
    if (diagonal) {
      VLOG(3) << "  diagonal - bailing out!";
      continue;
    }

    vector<S2CellId> cells;
    cells.push_back(big_cell);
    cells.push_back(small_cell);
    S2CellUnion cell_union;
    cell_union.Init(cells);
    EXPECT_EQ(2, cell_union.num_cells());
    S2Polygon poly;
    poly.InitToCellUnionBorder(cell_union);
    EXPECT_EQ(1, poly.num_loops());
    // If the conversion were perfect we could test containment, but due to
    // rounding the polygon won't always exactly contain both cells.  We can
    // at least test intersection.
    EXPECT_TRUE(poly.MayIntersect(S2Cell(big_cell)));
    EXPECT_TRUE(poly.MayIntersect(S2Cell(small_cell)));
  }
}

TEST_F(S2PolygonTestBase, TestEncodeDecode) {
  Encoder encoder;
  cross1->Encode(&encoder);
  Decoder decoder(encoder.base(), encoder.length());
  S2Polygon decoded_polygon;
  ASSERT_TRUE(decoded_polygon.Decode(&decoder));
  EXPECT_TRUE(cross1->BoundaryEquals(&decoded_polygon));
  EXPECT_EQ(cross1->GetRectBound(), decoded_polygon.GetRectBound());
}

// This test checks that S2Polygons created directly from S2Cells behave
// identically to S2Polygons created from the vertices of those cells; this
// previously was not the case, because S2Cells calculate their bounding
// rectangles slightly differently, and S2Polygons created from them just
// copied the S2Cell bounds.
TEST(S2Polygon, TestS2CellConstructorAndContains) {
  S2LatLng latlng(S1Angle::E6(40565459), S1Angle::E6(-74645276));
  S2Cell cell(S2CellId::FromLatLng(latlng));
  S2Polygon cell_as_polygon(cell);
  S2Polygon empty;
  S2Polygon polygon_copy;
  polygon_copy.InitToUnion(&cell_as_polygon, &empty);
  EXPECT_TRUE(polygon_copy.Contains(&cell_as_polygon));
  EXPECT_TRUE(polygon_copy.Contains(cell));
}

TEST(S2PolygonTest, Project) {
  scoped_ptr<S2Polygon> polygon(MakePolygon(kNear0 + kNear2));
  S2Point point;
  S2Point projected;

  // The point inside the polygon should be projected into itself.
  point = S2Testing::MakePoint("1.1:0");
  projected = polygon->Project(point);
  EXPECT_TRUE(S2::ApproxEquals(point, projected));

  // The point is on the outside of the polygon.
  point = S2Testing::MakePoint("5.1:-2");
  projected = polygon->Project(point);
  EXPECT_TRUE(S2::ApproxEquals(S2Testing::MakePoint("5:-2"), projected));

  // The point is inside the hole in the polygon.
  point = S2Testing::MakePoint("-0.49:-0.49");
  projected = polygon->Project(point);
  EXPECT_TRUE(S2::ApproxEquals(S2Testing::MakePoint("-0.5:-0.5"),
                               projected, 1e-6));

  point = S2Testing::MakePoint("0:-3");
  projected = polygon->Project(point);
  EXPECT_TRUE(S2::ApproxEquals(S2Testing::MakePoint("0:-2"), projected));
}

// Returns the distance of a point to a polygon (distance is 0 if the
// point is in the polygon).
double DistanceToPolygonInDegrees(S2Point point, S2Polygon const& poly) {
  S1Angle distance = S1Angle(poly.Project(point), point);
  return distance.degrees();
}

// Returns the diameter of a loop (maximum distance between any two
// points in the loop).
S1Angle LoopDiameter(S2Loop const& loop) {
  S1Angle diameter = S1Angle();
  for (int i = 0; i < loop.num_vertices(); ++i) {
    S2Point test_point = loop.vertex(i);
    for (int j = i + 1; j < loop.num_vertices(); ++j) {
      diameter = max(diameter,
                     S2EdgeUtil::GetDistance(test_point, loop.vertex(j),
                                             loop.vertex(j+1)));
    }
  }
  return diameter;
}

// Returns the maximum distance from any vertex of poly_a to poly_b, that is,
// the directed Haussdorf distance of the set of vertices of poly_a to the
// boundary of poly_b.
//
// Doesn't consider loops from poly_a that have diameter less than min_diameter
// in degrees.
double MaximumDistanceInDegrees(S2Polygon const& poly_a,
                                S2Polygon const& poly_b,
                                double min_diameter_in_degrees) {
  double min_distance = 360;
  bool has_big_loops = false;
  for (int l = 0; l < poly_a.num_loops(); ++l) {
    S2Loop* a_loop = poly_a.loop(l);
    if (LoopDiameter(*a_loop).degrees() <= min_diameter_in_degrees) {
      continue;
    }
    has_big_loops = true;
    for (int v = 0; v < a_loop->num_vertices(); ++v) {
      double distance =
          DistanceToPolygonInDegrees(a_loop->vertex(v), poly_b);
      if (distance < min_distance) {
        min_distance = distance;
      }
    }
  }
  if (has_big_loops) {
    return min_distance;
  } else {
    return 0.;  // As if the first polygon were empty.
  }
}

class S2PolygonSimplifierTest : public ::testing::Test {
 protected:
  S2PolygonSimplifierTest() {
    simplified = NULL;
    original = NULL;
  }

  ~S2PolygonSimplifierTest() {
    delete simplified;
    delete original;
  }

  // Owns poly.
  void SetInput(S2Polygon* poly, double tolerance_in_degrees) {
    delete original;
    delete simplified;
    original = poly;

    simplified = new S2Polygon();
    return simplified->InitToSimplified(original, 
                                        S1Angle::Degrees(tolerance_in_degrees));
  }

  void SetInput(string const& poly, double tolerance_in_degrees) {
    SetInput(S2Testing::MakePolygon(poly), tolerance_in_degrees);
  }

  S2Polygon* simplified;
  S2Polygon* original;
};

TEST_F(S2PolygonSimplifierTest, NoSimplification) {
  SetInput("0:0, 0:20, 20:20, 20:0", 1.0);
  EXPECT_EQ(4, simplified->num_vertices());

  EXPECT_EQ(0, MaximumDistanceInDegrees(*simplified, *original, 0));
  EXPECT_EQ(0, MaximumDistanceInDegrees(*original, *simplified, 0));
}

// Here, 10:-2 will be removed and  0:0-20:0 will intersect two edges.
// (The resulting polygon will in fact probably have more edges.)
TEST_F(S2PolygonSimplifierTest, SimplifiedLoopSelfIntersects) {
  SetInput("0:0, 0:20, 10:-0.1, 20:20, 20:0, 10:-0.2", 0.22);

  // To make sure that this test does something, we check
  // that the vertex 10:-0.2 is not in the simplification anymore.
  S2Point test_point = S2Testing::MakePoint("10:-0.2");
  EXPECT_LT(0.05, DistanceToPolygonInDegrees(test_point, *simplified));

  EXPECT_GE(0.22, MaximumDistanceInDegrees(*simplified, *original, 0));
  EXPECT_GE(0.22, MaximumDistanceInDegrees(*original, *simplified, 0.22));
}

TEST_F(S2PolygonSimplifierTest, NoSimplificationManyLoops) {
  SetInput("0:0,    0:1,   1:0;   0:20, 0:21, 1:20; "
           "20:20, 20:21, 21:20; 20:0, 20:1, 21:0", 0.01);
  EXPECT_EQ(0, MaximumDistanceInDegrees(*simplified, *original, 0));
  EXPECT_EQ(0, MaximumDistanceInDegrees(*original, *simplified, 0));
}

TEST_F(S2PolygonSimplifierTest, TinyLoopDisappears) {
  SetInput("0:0, 0:1, 1:1, 1:0", 1.1);
  EXPECT_EQ(0, simplified->num_vertices());
}

TEST_F(S2PolygonSimplifierTest, StraightLinesAreSimplified) {
  SetInput("0:0, 1:0, 2:0, 3:0, 4:0, 5:0, 6:0,"
           "6:1, 5:1, 4:1, 3:1, 2:1, 1:1, 0:1", 0.01);
  EXPECT_EQ(4, simplified->num_vertices());
}

TEST_F(S2PolygonSimplifierTest, EdgeSplitInManyPieces) {
  // near_square's right four-point side will be simplified to a vertical
  // line at lng=7.9, that will cut the 9 teeth of the saw (the edge will
  // therefore be broken into 19 pieces).
  const string saw =
      "1:1, 1:8, 2:2, 2:8, 3:2, 3:8, 4:2, 4:8, 5:2, 5:8,"
      "6:2, 6:8, 7:2, 7:8, 8:2, 8:8, 9:2, 9:8, 10:1";
  const string near_square =
      "0:0, 0:7.9, 1:8.1, 10:8.1, 11:7.9, 11:0";
  SetInput(saw + ";" + near_square, 0.21);

  EXPECT_TRUE(simplified->IsValid());
  EXPECT_GE(0.11, MaximumDistanceInDegrees(*simplified, *original, 0));
  EXPECT_GE(0.11, MaximumDistanceInDegrees(*original, *simplified, 0));
  // The resulting polygon's 9 little teeth are very small and disappear
  // due to the vertex_merge_radius of the polygon builder.  There remains
  // nine loops.
  EXPECT_EQ(9, simplified->num_loops());
}

TEST_F(S2PolygonSimplifierTest, EdgesOverlap) {
  // Two loops, One edge of the second one ([0:1 - 0:2]) is part of an
  // edge of the first one..
  SetInput("0:0, 0:3, 1:0; 0:1, -1:1, 0:2", 0.01);
  scoped_ptr<S2Polygon> true_poly(
      S2Testing::MakePolygon("0:3, 1:0, 0:0, 0:1, -1:1, 0:2"));
  EXPECT_TRUE(simplified->BoundaryApproxEquals(true_poly.get()));
}

S2Polygon* MakeRegularPolygon(const string& center,
                              int num_points, double radius_in_degrees) {

  double radius_in_radians = S1Angle::Degrees(radius_in_degrees).radians();
  S2Loop* l = S2Testing::MakeRegularLoop(S2Testing::MakePoint(center),
                                         num_points,
                                         radius_in_radians);
  vector<S2Loop*> loops;
  loops.push_back(l);
  return new S2Polygon(&loops);
}

// Tests that a regular polygon with many points gets simplified
// enough.
TEST_F(S2PolygonSimplifierTest, LargeRegularPolygon) {
  const double kRadius = 2.;  // in degrees
  const int num_initial_points = 1000;
  const int num_desired_points = 250;
  double tolerance = 1.05 * kRadius * (1 - cos(M_PI / num_desired_points));

  S2Polygon* p = MakeRegularPolygon("0:0", num_initial_points, kRadius);
  SetInput(p, tolerance);

  EXPECT_GE(tolerance, MaximumDistanceInDegrees(*simplified, *original, 0));
  EXPECT_GE(tolerance, MaximumDistanceInDegrees(*original, *simplified, 0));
  EXPECT_GE(250, simplified->num_vertices());
  EXPECT_LE(200, simplified->num_vertices());
}

string GenerateInputForBenchmark(int num_vertices_per_loop_for_bm) {
  CHECK_LE(FLAGS_num_loops_per_polygon_for_bm, 90);
  vector<S2Loop*> loops;
  for (int li = 0; li < FLAGS_num_loops_per_polygon_for_bm; ++li) {
    vector<S2Point> vertices;
    double radius_degrees =
        1.0 + (50.0 * li) / FLAGS_num_loops_per_polygon_for_bm;
    for (int vi = 0; vi < num_vertices_per_loop_for_bm; ++vi) {
      double angle_radians = (2 * M_PI * vi) / num_vertices_per_loop_for_bm;
      double lat = radius_degrees * cos(angle_radians);
      double lng = radius_degrees * sin(angle_radians);
      vertices.push_back(S2LatLng::FromDegrees(lat, lng).ToPoint());
    }
    loops.push_back(new S2Loop(vertices));
  }
  S2Polygon polygon_to_encode(&loops);

  Encoder encoder;
  polygon_to_encode.Encode(&encoder);
  string encoded(encoder.base(), encoder.length());

  return encoded;
}

static void BM_S2Decoding(int iters, int num_vertices_per_loop_for_bm) {
  StopBenchmarkTiming();
  string encoded = GenerateInputForBenchmark(num_vertices_per_loop_for_bm);
  StartBenchmarkTiming();
  for (int i = 0; i < iters; ++i) {
    Decoder decoder(encoded.data(), encoded.size());
    S2Polygon decoded_polygon;
    decoded_polygon.Decode(&decoder);
  }
}
BENCHMARK_RANGE(BM_S2Decoding, 8, 131072);

static void BM_S2DecodingWithinScope(int iters,
                                     int num_vertices_per_loop_for_bm) {
  StopBenchmarkTiming();
  string encoded = GenerateInputForBenchmark(num_vertices_per_loop_for_bm);
  StartBenchmarkTiming();
  for (int i = 0; i < iters; ++i) {
    Decoder decoder(encoded.data(), encoded.size());
    S2Polygon decoded_polygon;
    decoded_polygon.DecodeWithinScope(&decoder);
  }
}
BENCHMARK_RANGE(BM_S2DecodingWithinScope, 8, 131072);


void ConcentricLoops(S2Point center,
                     int num_loops,
                     int num_vertices_per_loop,
                     S2Polygon* poly) {
  Matrix3x3_d m;
  S2::GetFrame(center, &m);
  vector<S2Loop*> loops;
  for (int li = 0; li < num_loops; ++li) {
    vector<S2Point> vertices;
    double radius = 0.005 * (li + 1) / num_loops;
    double radian_step = 2 * M_PI / num_vertices_per_loop;
    for (int vi = 0; vi < num_vertices_per_loop; ++vi) {
      double angle = vi * radian_step;
      S2Point p(radius * cos(angle), radius * sin(angle), 1);
      vertices.push_back(S2::FromFrame(m, p.Normalize()));
    }
    loops.push_back(new S2Loop(vertices));
  }
  poly->Init(&loops);
}

static void UnionOfPolygons(int iters,
                            int num_vertices_per_loop,
                            double second_polygon_offset) {
  for (int i = 0; i < iters; ++i) {
    StopBenchmarkTiming();
    S2Polygon p1, p2;
    S2Point center = S2Testing::RandomPoint();
    ConcentricLoops(center,
                    FLAGS_num_loops_per_polygon_for_bm,
                    num_vertices_per_loop,
                    &p1);
    ConcentricLoops(
        (center + S2Point(second_polygon_offset,
                          second_polygon_offset,
                          second_polygon_offset)).Normalize(),
        FLAGS_num_loops_per_polygon_for_bm,
        num_vertices_per_loop,
        &p2);
    StartBenchmarkTiming();
    S2Polygon p_result;
    p_result.InitToUnion(&p1, &p2);
  }
}

static void BM_DeepPolygonUnion(int iters, int num_vertices_per_loop) {
  UnionOfPolygons(iters, num_vertices_per_loop, 0.000001);
}
BENCHMARK(BM_DeepPolygonUnion)
    ->Arg(8)
    ->Arg(64)
    ->Arg(128)
    ->Arg(256)
    ->Arg(512)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(8192);

static void BM_ShallowPolygonUnion(int iters, int num_vertices_per_loop) {
  UnionOfPolygons(iters, num_vertices_per_loop, 0.004);
}
BENCHMARK(BM_ShallowPolygonUnion)
    ->Arg(8)
    ->Arg(64)
    ->Arg(128)
    ->Arg(256)
    ->Arg(512)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(8192);

static void BM_DisjointPolygonUnion(int iters, int num_vertices_per_loop) {
  UnionOfPolygons(iters, num_vertices_per_loop, 0.3);
}
BENCHMARK(BM_DisjointPolygonUnion)
    ->Arg(8)
    ->Arg(64)
    ->Arg(128)
    ->Arg(256)
    ->Arg(512)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(8192);
