// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2regioncoverer.h"

#include <stdio.h>

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <hash_map>
using __gnu_cxx::hash_map;

#include <queue>
using std::priority_queue;

#include <string>
using std::string;

#include <vector>
using std::vector;


#include "base/commandlineflags.h"
#include "base/logging.h"
#include "base/strtoint.h"
#include "strings/split.h"
#include "strings/stringprintf.h"
#include "testing/base/public/benchmark.h"
#include "testing/base/public/gunit.h"
#include "s2cap.h"
#include "s2cell.h"
#include "s2cellid.h"
#include "s2cellunion.h"
#include "s2latlng.h"
#include "s2loop.h"
#include "s2testing.h"

DEFINE_string(max_cells, "4,8",
              "Comma-separated list of values to use for 'max_cells'");

DEFINE_int32(iters, DEBUG_MODE ? 1000 : 100000,
             "Number of random caps to try for each max_cells value");

TEST(S2RegionCoverer, RandomCells) {
  S2RegionCoverer coverer;
  coverer.set_max_cells(1);

  // Test random cell ids at all levels.
  for (int i = 0; i < 10000; ++i) {
    S2CellId id = S2Testing::GetRandomCellId();
    SCOPED_TRACE(StringPrintf("Iteration %d, cell ID token %s",
                              i, id.ToToken().c_str()));
    vector<S2CellId> covering;
    coverer.GetCovering(S2Cell(id), &covering);
    EXPECT_EQ(covering.size(), 1);
    EXPECT_EQ(covering[0], id);
  }
}

static void CheckCovering(S2RegionCoverer const& coverer,
                          S2Region const& region,
                          vector<S2CellId> const& covering,
                          bool interior) {
  // Keep track of how many cells have the same coverer.min_level() ancestor.
  hash_map<S2CellId, int> min_level_cells;
  for (int i = 0; i < covering.size(); ++i) {
    int level = covering[i].level();
    EXPECT_GE(level, coverer.min_level());
    EXPECT_LE(level, coverer.max_level());
    EXPECT_EQ((level - coverer.min_level()) % coverer.level_mod(), 0);
    min_level_cells[covering[i].parent(coverer.min_level())] += 1;
  }
  if (covering.size() > coverer.max_cells()) {
    // If the covering has more than the requested number of cells, then check
    // that the cell count cannot be reduced by using the parent of some cell.
    for (hash_map<S2CellId, int>::const_iterator i = min_level_cells.begin();
         i != min_level_cells.end(); ++i) {
      EXPECT_EQ(i->second, 1);
    }
  }
  if (interior) {
    for (int i = 0; i < covering.size(); ++i) {
      EXPECT_TRUE(region.Contains(S2Cell(covering[i])));
    }
  } else {
    S2CellUnion cell_union;
    cell_union.Init(covering);
    S2Testing::CheckCovering(region, cell_union, true);
  }
}

TEST(S2RegionCoverer, RandomCaps) {
  static int const kMaxLevel = S2CellId::kMaxLevel;
  S2RegionCoverer coverer;
  for (int i = 0; i < 1000; ++i) {
    do {
      coverer.set_min_level(S2Testing::rnd.Uniform(kMaxLevel + 1));
      coverer.set_max_level(S2Testing::rnd.Uniform(kMaxLevel + 1));
    } while (coverer.min_level() > coverer.max_level());
    coverer.set_max_cells(S2Testing::rnd.Skewed(10));
    coverer.set_level_mod(1 + S2Testing::rnd.Uniform(3));
    double max_area =  min(4 * M_PI, (3 * coverer.max_cells() + 1) *
                           S2Cell::AverageArea(coverer.min_level()));
    S2Cap cap = S2Testing::GetRandomCap(0.1 * S2Cell::AverageArea(kMaxLevel),
                                        max_area);
    vector<S2CellId> covering, interior;
    coverer.GetCovering(cap, &covering);
    CheckCovering(coverer, cap, covering, false);
    coverer.GetInteriorCovering(cap, &interior);
    CheckCovering(coverer, cap, interior, true);

    // Check that GetCovering is deterministic.
    vector<S2CellId> covering2;
    coverer.GetCovering(cap, &covering2);
    EXPECT_EQ(covering, covering2);

    // Also check S2CellUnion::Denormalize().  The denormalized covering
    // may still be different and smaller than "covering" because
    // S2RegionCoverer does not guarantee that it will not output all four
    // children of the same parent.
    S2CellUnion cells;
    cells.Init(covering);
    vector<S2CellId> denormalized;
    cells.Denormalize(coverer.min_level(), coverer.level_mod(), &denormalized);
    CheckCovering(coverer, cap, denormalized, false);
  }
}

TEST(S2RegionCoverer, SimpleCoverings) {
  static int const kMaxLevel = S2CellId::kMaxLevel;
  S2RegionCoverer coverer;
  coverer.set_max_cells(kint32max);
  for (int i = 0; i < 1000; ++i) {
    int level = S2Testing::rnd.Uniform(kMaxLevel + 1);
    coverer.set_min_level(level);
    coverer.set_max_level(level);
    double max_area =  min(4 * M_PI, 1000 * S2Cell::AverageArea(level));
    S2Cap cap = S2Testing::GetRandomCap(0.1 * S2Cell::AverageArea(kMaxLevel),
                                        max_area);
    vector<S2CellId> covering;
    S2RegionCoverer::GetSimpleCovering(cap, cap.axis(), level, &covering);
    CheckCovering(coverer, cap, covering, false);
  }
}

// We keep a priority queue of the caps that had the worst approximation
// ratios so that we can print them at the end.
struct WorstCap {
  double ratio;
  S2Cap cap;
  int num_cells;
  bool operator<(WorstCap const& o) const { return ratio > o.ratio; }
  WorstCap(double r, S2Cap c, int n) : ratio(r), cap(c), num_cells(n) {}
};

static void TestAccuracy(int max_cells) {
  SCOPED_TRACE(StringPrintf("%d cells", max_cells));

  static int const kNumMethods = 1;
  // This code is designed to evaluate several approximation algorithms and
  // figure out which one works better.  The way to do this is to hack the
  // S2RegionCoverer interface to add a global variable to control which
  // algorithm (or variant of an algorithm) is selected, and then assign to
  // this variable in the "method" loop below.  The code below will then
  // collect statistics on all methods, including how often each one wins in
  // terms of cell count and approximation area.

  S2RegionCoverer coverer;
  coverer.set_max_cells(max_cells);

  double ratio_total[kNumMethods] = {0};
  double min_ratio[kNumMethods];  // initialized in loop below
  double max_ratio[kNumMethods] = {0};
  vector<double> ratios[kNumMethods];
  int cell_total[kNumMethods] = {0};
  int area_winner_tally[3] = {0};
  int cell_winner_tally[3] = {0};
  for (int method = 0; method < kNumMethods; ++method) {
    min_ratio[method] = 1e20;
  }

  priority_queue<WorstCap> worst_caps;
  static int const kMaxWorstCaps = 10;

  for (int i = 0; i < FLAGS_iters; ++i) {
    // Choose the log of the cap area to be uniformly distributed over
    // the allowable range.  Don't try to approximate regions that are so
    // small they can't use the given maximum number of cells efficiently.
    double const min_cap_area = S2Cell::AverageArea(S2CellId::kMaxLevel)
                                * max_cells * max_cells;
    S2Cap cap = S2Testing::GetRandomCap(min_cap_area, 4 * M_PI);
    double cap_area = cap.area();

    double min_area = 1e30;
    int min_cells = 1 << 30;
    double area[kNumMethods];
    int cells[kNumMethods];
    for (int method = 0; method < kNumMethods; ++method) {
      // If you want to play with different methods, do this:
      // S2RegionCoverer::method_number = method;

      vector<S2CellId> covering;
      coverer.GetCovering(cap, &covering);

      double union_area = 0;
      for (int j = 0; j < covering.size(); ++j) {
        union_area += S2Cell(covering[j]).ExactArea();
      }
      cells[method] = covering.size();
      min_cells = min(cells[method], min_cells);
      area[method] = union_area;
      min_area = min(area[method], min_area);
      cell_total[method] += cells[method];
      double ratio = area[method] / cap_area;
      ratio_total[method] += ratio;
      min_ratio[method] = min(ratio, min_ratio[method]);
      max_ratio[method] = max(ratio, max_ratio[method]);
      ratios[method].push_back(ratio);
      if (worst_caps.size() < kMaxWorstCaps) {
        worst_caps.push(WorstCap(ratio, cap, cells[method]));
      } else if (ratio > worst_caps.top().ratio) {
        worst_caps.pop();
        worst_caps.push(WorstCap(ratio, cap, cells[method]));
      }
    }
    for (int method = 0; method < kNumMethods; ++method) {
      if (area[method] == min_area) ++area_winner_tally[method];
      if (cells[method] == min_cells) ++cell_winner_tally[method];
    }
  }
  for (int method = 0; method < kNumMethods; ++method) {
    printf("\nMax cells %d, method %d:\n", max_cells, method);
    printf("  Average cells: %.4f\n", cell_total[method] /
           static_cast<double>(FLAGS_iters));
    printf("  Average area ratio: %.4f\n", ratio_total[method] / FLAGS_iters);
    vector<double>& mratios = ratios[method];
    sort(mratios.begin(), mratios.end());
    printf("  Median ratio: %.4f\n", mratios[mratios.size() / 2]);
    printf("  Max ratio: %.4f\n", max_ratio[method]);
    printf("  Min ratio: %.4f\n", min_ratio[method]);
    if (kNumMethods > 1) {
      printf("  Cell winner probability: %.4f\n",
             cell_winner_tally[method] / static_cast<double>(FLAGS_iters));
      printf("  Area winner probability: %.4f\n",
             area_winner_tally[method] / static_cast<double>(FLAGS_iters));
    }
    printf("  Caps with worst approximation ratios:\n");
    for (; !worst_caps.empty(); worst_caps.pop()) {
      WorstCap const& w = worst_caps.top();
      S2LatLng ll(w.cap.axis());
      printf("    Ratio %.4f, Cells %d, "
             "Center (%.8f, %.8f), Km %.6f\n",
             w.ratio, w.num_cells,
             ll.lat().degrees(), ll.lng().degrees(),
             w.cap.angle().radians() * 6367.0);
    }
  }
}

TEST(S2RegionCoverer, Accuracy) {
  vector<string> max_cells;
  SplitStringUsing(FLAGS_max_cells, ",", &max_cells);
  for (int i = 0; i < max_cells.size(); ++i) {
    TestAccuracy(atoi32(max_cells[i].c_str()));
  }
}


// Two concentric loops don't cross so there is no 'fast exit'
static void BM_Covering(int iters, int max_cells, int num_vertices) {
  StopBenchmarkTiming();
  S2RegionCoverer coverer;
  coverer.set_max_cells(max_cells);

  for (int i = 0; i < iters; ++i) {
    S2Point center = S2Testing::RandomPoint();
    S2Loop* loop = S2Testing::MakeRegularLoop(center, num_vertices, 0.005);

    StartBenchmarkTiming();
    vector<S2CellId> covering;
    coverer.GetCovering(*loop, &covering);
    StopBenchmarkTiming();

    delete loop;
  }
}
BENCHMARK(BM_Covering)->RangePair(8, 1024, 8, 1<<17);
