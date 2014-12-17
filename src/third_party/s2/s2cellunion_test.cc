// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2cellunion.h"

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <vector>
using std::vector;


#include "base/integral_types.h"
#include "base/logging.h"
#include "testing/base/public/gunit.h"
#include "s2cellid.h"
#include "s2cell.h"
#include "s2cap.h"
#include "s2testing.h"
#include "s2regioncoverer.h"

TEST(S2CellUnion, Basic) {
  S2CellUnion empty;
  vector<S2CellId> ids;
  empty.Init(ids);
  EXPECT_EQ(0, empty.num_cells());

  S2CellId face1_id = S2CellId::FromFacePosLevel(1, 0, 0);
  S2CellUnion face1_union;
  ids.push_back(face1_id);
  face1_union.Init(ids);
  EXPECT_EQ(1, face1_union.num_cells());
  EXPECT_EQ(face1_id, face1_union.cell_id(0));

  S2CellId face2_id = S2CellId::FromFacePosLevel(2, 0, 0);
  S2CellUnion face2_union;
  vector<uint64> cell_ids;
  cell_ids.push_back(face2_id.id());
  face2_union.Init(cell_ids);
  EXPECT_EQ(1, face2_union.num_cells());
  EXPECT_EQ(face2_id, face2_union.cell_id(0));

  S2Cell face1_cell(face1_id);
  S2Cell face2_cell(face2_id);
  EXPECT_TRUE(face1_union.Contains(face1_cell));
  EXPECT_TRUE(!face1_union.Contains(face2_cell));
}

static S2Testing::Random& rnd = S2Testing::rnd;

static void AddCells(S2CellId const& id, bool selected,
                     vector<S2CellId> *input, vector<S2CellId> *expected) {
  // Decides whether to add "id" and/or some of its descendants to the
  // test case.  If "selected" is true, then the region covered by "id"
  // *must* be added to the test case (either by adding "id" itself, or
  // some combination of its descendants, or both).  If cell ids are to
  // the test case "input", then the corresponding expected result after
  // simplification is added to "expected".

  if (id == S2CellId::None()) {
    // Initial call: decide whether to add cell(s) from each face.
    for (int face = 0; face < 6; ++face) {
      AddCells(S2CellId::FromFacePosLevel(face, 0, 0), false, input, expected);
    }
    return;
  }
  if (id.is_leaf()) {
    // The rnd.OneIn() call below ensures that the parent of a leaf cell
    // will always be selected (if we make it that far down the hierarchy).
    DCHECK(selected);
    input->push_back(id);
    return;
  }
  // The following code ensures that the probability of selecting a cell
  // at each level is approximately the same, i.e. we test normalization
  // of cells at all levels.
  if (!selected && rnd.OneIn(S2CellId::kMaxLevel - id.level())) {
    // Once a cell has been selected, the expected output is predetermined.
    // We then make sure that cells are selected that will normalize to
    // the desired output.
    expected->push_back(id);
    selected = true;
  }

  // With the rnd.OneIn() constants below, this function adds an average
  // of 5/6 * (kMaxLevel - level) cells to "input" where "level" is the
  // level at which the cell was first selected (level 15 on average).
  // Therefore the average number of input cells in a test case is about
  // (5/6 * 15 * 6) = 75.  The average number of output cells is about 6.

  // If a cell is selected, we add it to "input" with probability 5/6.
  bool added = false;
  if (selected && !rnd.OneIn(6)) {
    input->push_back(id);
    added = true;
  }
  int num_children = 0;
  S2CellId child = id.child_begin();
  for (int pos = 0; pos < 4; ++pos, child = child.next()) {
    // If the cell is selected, on average we recurse on 4/12 = 1/3 child.
    // This intentionally may result in a cell and some of its children
    // being included in the test case.
    //
    // If the cell is not selected, on average we recurse on one child.
    // We also make sure that we do not recurse on all 4 children, since
    // then we might include all 4 children in the input case by accident
    // (in which case the expected output would not be correct).
    if (rnd.OneIn(selected ? 12 : 4) && num_children < 3) {
      AddCells(child, selected, input, expected);
      ++num_children;
    }
    // If this cell was selected but the cell itself was not added, we
    // must ensure that all 4 children (or some combination of their
    // descendants) are added.
    if (selected && !added) AddCells(child, selected, input, expected);
  }
}

TEST(S2CellUnion, Normalize) {
  // Try a bunch of random test cases, and keep track of average
  // statistics for normalization (to see if they agree with the
  // analysis above).
  S2CellUnion cellunion;
  double in_sum = 0, out_sum = 0;
  static int const kIters = 2000;
  for (int i = 0; i < kIters; ++i) {
    vector<S2CellId> input, expected;
    AddCells(S2CellId::None(), false, &input, &expected);
    in_sum += input.size();
    out_sum += expected.size();
    cellunion.Init(input);
    EXPECT_EQ(expected.size(), cellunion.num_cells());
    for (int i = 0; i < expected.size(); ++i) {
      EXPECT_EQ(expected[i], cellunion.cell_id(i));
    }

    // Test GetCapBound().
    S2Cap cap = cellunion.GetCapBound();
    for (int i = 0; i < cellunion.num_cells(); ++i) {
      EXPECT_TRUE(cap.Contains(S2Cell(cellunion.cell_id(i))));
    }

    // Test Contains(S2CellId) and Intersects(S2CellId).
    for (int j = 0; j < input.size(); ++j) {
      EXPECT_TRUE(cellunion.Contains(input[j]));
      EXPECT_TRUE(cellunion.Contains(input[j].ToPoint()));
      EXPECT_TRUE(cellunion.VirtualContainsPoint(input[j].ToPoint()));
      EXPECT_TRUE(cellunion.Intersects(input[j]));
      if (!input[j].is_face()) {
        EXPECT_TRUE(cellunion.Intersects(input[j].parent()));
        if (input[j].level() > 1) {
          EXPECT_TRUE(cellunion.Intersects(input[j].parent().parent()));
          EXPECT_TRUE(cellunion.Intersects(input[j].parent(0)));
        }
      }
      if (!input[j].is_leaf()) {
        EXPECT_TRUE(cellunion.Contains(input[j].child_begin()));
        EXPECT_TRUE(cellunion.Intersects(input[j].child_begin()));
        EXPECT_TRUE(cellunion.Contains(input[j].child_end().prev()));
        EXPECT_TRUE(cellunion.Intersects(input[j].child_end().prev()));
        EXPECT_TRUE(cellunion.Contains(
                        input[j].child_begin(S2CellId::kMaxLevel)));
        EXPECT_TRUE(cellunion.Intersects(
                        input[j].child_begin(S2CellId::kMaxLevel)));
      }
    }
    for (int j = 0; j < expected.size(); ++j) {
      if (!expected[j].is_face()) {
        EXPECT_TRUE(!cellunion.Contains(expected[j].parent()));
        EXPECT_TRUE(!cellunion.Contains(expected[j].parent(0)));
      }
    }

    // Test Contains(S2CellUnion*), Intersects(S2CellUnion*),
    // GetUnion(), GetIntersection(), and GetDifference().
    vector<S2CellId> x, y, x_or_y, x_and_y;
    for (int j = 0; j < input.size(); ++j) {
      bool in_x = rnd.OneIn(2);
      bool in_y = rnd.OneIn(2);
      if (in_x) x.push_back(input[j]);
      if (in_y) y.push_back(input[j]);
      if (in_x || in_y) x_or_y.push_back(input[j]);
    }
    S2CellUnion xcells, ycells, x_or_y_expected, x_and_y_expected;
    xcells.Init(x);
    ycells.Init(y);
    x_or_y_expected.Init(x_or_y);

    S2CellUnion x_or_y_cells;
    x_or_y_cells.GetUnion(&xcells, &ycells);
    EXPECT_TRUE(x_or_y_cells == x_or_y_expected);

    // Compute the intersection of "x" with each cell of "y",
    // check that this intersection is correct, and append the
    // results to x_and_y_expected.
    for (int j = 0; j < ycells.num_cells(); ++j) {
      S2CellId yid = ycells.cell_id(j);
      S2CellUnion u;
      u.GetIntersection(&xcells, yid);
      for (int k = 0; k < xcells.num_cells(); ++k) {
        S2CellId xid = xcells.cell_id(k);
        if (xid.contains(yid)) {
          EXPECT_TRUE(u.num_cells() == 1 && u.cell_id(0) == yid);
        } else if (yid.contains(xid)) {
          EXPECT_TRUE(u.Contains(xid));
        }
      }
      for (int k = 0; k < u.num_cells(); ++k) {
        EXPECT_TRUE(xcells.Contains(u.cell_id(k)));
        EXPECT_TRUE(yid.contains(u.cell_id(k)));
      }
      x_and_y.insert(x_and_y.end(), u.cell_ids().begin(), u.cell_ids().end());
    }
    x_and_y_expected.Init(x_and_y);

    S2CellUnion x_and_y_cells;
    x_and_y_cells.GetIntersection(&xcells, &ycells);
    EXPECT_TRUE(x_and_y_cells == x_and_y_expected);

    S2CellUnion x_minus_y_cells, y_minus_x_cells;
    x_minus_y_cells.GetDifference(&xcells, &ycells);
    y_minus_x_cells.GetDifference(&ycells, &xcells);
    EXPECT_TRUE(xcells.Contains(&x_minus_y_cells));
    EXPECT_TRUE(!x_minus_y_cells.Intersects(&ycells));
    EXPECT_TRUE(ycells.Contains(&y_minus_x_cells));
    EXPECT_TRUE(!y_minus_x_cells.Intersects(&xcells));
    EXPECT_TRUE(!x_minus_y_cells.Intersects(&y_minus_x_cells));
    S2CellUnion diff_union;
    diff_union.GetUnion(&x_minus_y_cells, &y_minus_x_cells);
    S2CellUnion diff_intersection_union;
    diff_intersection_union.GetUnion(&diff_union, &x_and_y_cells);
    EXPECT_TRUE(diff_intersection_union == x_or_y_cells);

    vector<S2CellId> test, dummy;
    AddCells(S2CellId::None(), false, &test, &dummy);
    for (int j = 0; j < test.size(); ++j) {
      bool contains = false, intersects = false;
      for (int k = 0; k < expected.size(); ++k) {
        if (expected[k].contains(test[j])) contains = true;
        if (expected[k].intersects(test[j])) intersects = true;
      }
      EXPECT_EQ(contains, cellunion.Contains(test[j]));
      EXPECT_EQ(intersects, cellunion.Intersects(test[j]));
    }

  }
  printf("avg in %.2f, avg out %.2f\n", in_sum / kIters, out_sum / kIters);
}

static double GetMaxAngle(S2CellUnion const& covering, S2Point const& axis) {
  double max_angle = 0;
  for (int i = 0; i < covering.num_cells(); ++i) {
    S2Cell cell(covering.cell_id(i));
    S2Cap cell_cap = cell.GetCapBound();
    double angle = axis.Angle(cell_cap.axis()) + cell_cap.angle().radians();
    max_angle = max(max_angle, angle);
  }
  return max_angle;
}

TEST(S2CellUnion, Expand) {
  // This test generates coverings for caps of random sizes, and expands
  // the coverings by a random radius, and then make sure that the new
  // covering covers the expanded cap.  It also makes sure that the
  // new covering is not too much larger than expected.

  S2RegionCoverer coverer;
  for (int i = 0; i < 1000; ++i) {
    S2Cap cap = S2Testing::GetRandomCap(
        S2Cell::AverageArea(S2CellId::kMaxLevel), 4 * M_PI);

    // Expand the cap by a random factor whose log is uniformly distributed
    // between 0 and log(1e2).
    S2Cap expanded_cap = S2Cap::FromAxisHeight(
        cap.axis(), min(2.0, pow(1e2, rnd.RandDouble()) * cap.height()));

    double radius = expanded_cap.angle().radians() - cap.angle().radians();
    int max_level_diff = rnd.Uniform(8);

    S2CellUnion covering;
    coverer.set_max_cells(1 + rnd.Skewed(10));
    coverer.GetCellUnion(cap, &covering);
    S2Testing::CheckCovering(cap, covering, true);

    double max_angle = GetMaxAngle(covering, cap.axis());
    int min_level = S2CellId::kMaxLevel;
    for (int i = 0; i < covering.num_cells(); ++i) {
      min_level = min(min_level, covering.cell_id(i).level());
    }
    covering.Expand(S1Angle::Radians(radius), max_level_diff);
    S2Testing::CheckCovering(expanded_cap, covering, false);

    int expand_level = min(min_level + max_level_diff,
                           S2::kMinWidth.GetMaxLevel(radius));
    double expanded_max_angle = GetMaxAngle(covering, cap.axis());

    // If the covering includes a tiny cell along the boundary, in theory the
    // maximum angle of the covering from the cap axis can increase by up to
    // twice the maximum length of a cell diagonal.  We allow for an increase
    // of slightly more than this because cell bounding caps are not exact.
    EXPECT_LE(expanded_max_angle - max_angle,
              2.02 * S2::kMaxDiag.GetValue(expand_level));
    // TODO(user): This fails for some random seeds,
    // e.g. initialize the random seed to 3 in s2testing.cc.  This
    // means the assumption above is incorrect and needs to be
    // revisited.
  }
}

static void TestInitFromRange(S2CellId const& min_id,
                              S2CellId const& max_id) {
  S2CellUnion cell_union;
  cell_union.InitFromRange(min_id, max_id);
  vector<S2CellId> const& cell_ids = cell_union.cell_ids();

  EXPECT_GT(cell_ids.size(), 0);
  EXPECT_EQ(min_id, cell_ids.front().range_min());
  EXPECT_EQ(max_id, cell_ids.back().range_max());
  for (int i = 1; i < cell_ids.size(); ++i) {
    EXPECT_EQ(cell_ids[i].range_min(), cell_ids[i-1].range_max().next());
  }
  EXPECT_FALSE(cell_union.Normalize());
}

TEST(S2CellUnion, InitFromRange) {
  // Check the very first leaf cell and face cell.
  S2CellId face1_id = S2CellId::FromFacePosLevel(0, 0, 0);
  TestInitFromRange(face1_id.range_min(), face1_id.range_min());
  TestInitFromRange(face1_id.range_min(), face1_id.range_max());

  // Check the very last leaf cell and face cell.
  S2CellId face5_id = S2CellId::FromFacePosLevel(5, 0, 0);
  TestInitFromRange(face1_id.range_min(), face1_id.range_max());
  TestInitFromRange(face1_id.range_max(), face1_id.range_max());

  // Check random ranges of leaf cells.
  for (int i = 0; i < 100; ++i) {
    S2CellId x = S2Testing::GetRandomCellId(S2CellId::kMaxLevel);
    S2CellId y = S2Testing::GetRandomCellId(S2CellId::kMaxLevel);
    if (x > y) swap(x, y);
    TestInitFromRange(x, y);
  }
}

TEST(S2CellUnion, Empty) {
  S2CellUnion empty_cell_union;
  S2CellId face1_id = S2CellId::FromFacePosLevel(1, 0, 0);

  // Normalize()
  empty_cell_union.Normalize();
  EXPECT_EQ(0, empty_cell_union.num_cells());

  // Denormalize(...)
  vector<S2CellId> output;
  empty_cell_union.Denormalize(0, 2, &output);
  EXPECT_EQ(0, empty_cell_union.num_cells());

  // Pack(...)
  empty_cell_union.Pack();

  // Contains(...)
  EXPECT_FALSE(empty_cell_union.Contains(face1_id));
  EXPECT_TRUE(empty_cell_union.Contains(&empty_cell_union));

  // Intersects(...)
  EXPECT_FALSE(empty_cell_union.Intersects(face1_id));
  EXPECT_FALSE(empty_cell_union.Intersects(&empty_cell_union));

  // GetUnion(...)
  S2CellUnion cell_union;
  cell_union.GetUnion(&empty_cell_union, &empty_cell_union);
  EXPECT_EQ(0, cell_union.num_cells());

  // GetIntersection(...)
  S2CellUnion intersection;
  intersection.GetIntersection(&empty_cell_union, face1_id);
  EXPECT_EQ(0, intersection.num_cells());
  intersection.GetIntersection(&empty_cell_union, &empty_cell_union);
  EXPECT_EQ(0, intersection.num_cells());

  // GetDifference(...)
  S2CellUnion difference;
  difference.GetDifference(&empty_cell_union, &empty_cell_union);
  EXPECT_EQ(0, difference.num_cells());

  // Expand(...)
  empty_cell_union.Expand(S1Angle::Radians(1), 20);
  EXPECT_EQ(0, empty_cell_union.num_cells());
  empty_cell_union.Expand(10);
  EXPECT_EQ(0, empty_cell_union.num_cells());
}

TEST(S2CellUnion, Detach) {
  S2CellId face1_id = S2CellId::FromFacePosLevel(1, 0, 0);
  S2CellUnion face1_union;
  vector<S2CellId> ids;
  ids.push_back(face1_id);
  face1_union.Init(ids);
  ASSERT_EQ(1, face1_union.num_cells());
  EXPECT_EQ(face1_id, face1_union.cell_id(0));

  vector<S2CellId> retrieve;
  retrieve.push_back(S2CellId::Sentinel());
  face1_union.Detach(&retrieve);
  ASSERT_EQ(1, retrieve.size());
  EXPECT_EQ(face1_id, retrieve[0]);
  EXPECT_EQ(0, face1_union.num_cells());
}

TEST(S2CellUnion, LeafCellsCovered) {
  S2CellUnion cell_union;

  // empty union
  EXPECT_EQ(0, cell_union.LeafCellsCovered());


  vector<S2CellId> ids;
  ids.push_back(S2CellId::FromFacePosLevel(
      0, (1ULL << ((S2CellId::kMaxLevel << 1) - 1)), S2CellId::kMaxLevel));
  // One leaf on face 0.
  cell_union.Init(ids);
  EXPECT_EQ(1ULL, cell_union.LeafCellsCovered());

  // Face 0.
  ids.push_back(S2CellId::FromFacePosLevel(0, 0, 0));
  cell_union.Init(ids);
  EXPECT_EQ(1ULL << 60, cell_union.LeafCellsCovered());
  // Five faces.
  cell_union.Expand(0);
  EXPECT_EQ(5ULL << 60, cell_union.LeafCellsCovered());
  // Whole world.
  cell_union.Expand(0);
  EXPECT_EQ(6ULL << 60, cell_union.LeafCellsCovered());

  // Add some disjoint cells.
  ids.push_back(S2CellId::FromFacePosLevel(1, 0, 1));
  ids.push_back(S2CellId::FromFacePosLevel(2, 0, 2));
  ids.push_back(S2CellId::FromFacePosLevel(2, (1ULL << 60), 2));
  ids.push_back(S2CellId::FromFacePosLevel(3, 0, 14));
  ids.push_back(S2CellId::FromFacePosLevel(4, (1ULL << 60), 15));
  ids.push_back(S2CellId::FromFacePosLevel(4, 0, 27));
  ids.push_back(S2CellId::FromFacePosLevel(5, 0, 30));
  cell_union.Init(ids);
  uint64 expected = 1ULL + (1ULL << 6) + (1ULL << 30) + (1ULL << 32) +
      (2ULL << 56) + (1ULL << 58) + (1ULL << 60);
  EXPECT_EQ(expected, cell_union.LeafCellsCovered());
}
