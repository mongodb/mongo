// Copyright 2005 Google Inc. All Rights Reserved.

#include "r1interval.h"

#include "gtest/gtest.h"

static void TestIntervalOps(R1Interval const& x, R1Interval const& y,
                            const char* expected) {
  // Test all of the interval operations on the given pair of intervals.
  // "expected" is a sequence of "T" and "F" characters corresponding to
  // the expected results of Contains(), InteriorContains(), Intersects(),
  // and InteriorIntersects() respectively.

  EXPECT_EQ(expected[0] == 'T', x.Contains(y));
  EXPECT_EQ(expected[1] == 'T', x.InteriorContains(y));
  EXPECT_EQ(expected[2] == 'T', x.Intersects(y));
  EXPECT_EQ(expected[3] == 'T', x.InteriorIntersects(y));

  EXPECT_EQ(x.Contains(y), x.Union(y) == x);
  EXPECT_EQ(x.Intersects(y), !x.Intersection(y).is_empty());
}

TEST(R1Interval, TestBasic) {
  // Constructors and accessors.
  R1Interval unit(0, 1);
  R1Interval negunit(-1, 0);
  EXPECT_EQ(0, unit.lo());
  EXPECT_EQ(1, unit.hi());
  EXPECT_EQ(-1, negunit.bound(0));
  EXPECT_EQ(0, negunit.bound(1));
  R1Interval ten(0, 0);
  ten.set_hi(10);
  EXPECT_EQ(10, ten.hi());

  // is_empty()
  R1Interval half(0.5, 0.5);
  EXPECT_FALSE(unit.is_empty());
  EXPECT_FALSE(half.is_empty());
  R1Interval empty = R1Interval::Empty();
  EXPECT_TRUE(empty.is_empty());

  // Check that the default R1Interval is identical to Empty().
  R1Interval default_empty;
  EXPECT_TRUE(default_empty.is_empty());
  EXPECT_EQ(empty.lo(), default_empty.lo());
  EXPECT_EQ(empty.hi(), default_empty.hi());

  // GetCenter(), GetLength()
  EXPECT_EQ(unit.GetCenter(), 0.5);
  EXPECT_EQ(half.GetCenter(), 0.5);
  EXPECT_EQ(negunit.GetLength(), 1.0);
  EXPECT_EQ(half.GetLength(), 0);
  EXPECT_LT(empty.GetLength(), 0);

  // Contains(double), InteriorContains(double)
  EXPECT_TRUE(unit.Contains(0.5));
  EXPECT_TRUE(unit.InteriorContains(0.5));
  EXPECT_TRUE(unit.Contains(0));
  EXPECT_FALSE(unit.InteriorContains(0));
  EXPECT_TRUE(unit.Contains(1));
  EXPECT_FALSE(unit.InteriorContains(1));

  // Contains(R1Interval), InteriorContains(R1Interval)
  // Intersects(R1Interval), InteriorIntersects(R1Interval)
  { SCOPED_TRACE(""); TestIntervalOps(empty, empty, "TTFF"); }
  { SCOPED_TRACE(""); TestIntervalOps(empty, unit, "FFFF"); }
  { SCOPED_TRACE(""); TestIntervalOps(unit, half, "TTTT"); }
  { SCOPED_TRACE(""); TestIntervalOps(unit, unit, "TFTT"); }
  { SCOPED_TRACE(""); TestIntervalOps(unit, empty, "TTFF"); }
  { SCOPED_TRACE(""); TestIntervalOps(unit, negunit, "FFTF"); }
  { SCOPED_TRACE(""); TestIntervalOps(unit, R1Interval(0, 0.5), "TFTT"); }
  { SCOPED_TRACE(""); TestIntervalOps(half, R1Interval(0, 0.5), "FFTF"); }

  // AddPoint()
  R1Interval r = empty;
  r.AddPoint(5);
  EXPECT_EQ(5, r.lo());
  EXPECT_EQ(5, r.hi());
  r.AddPoint(-1);
  EXPECT_EQ(-1, r.lo());
  EXPECT_EQ(5, r.hi());
  r.AddPoint(0);
  EXPECT_EQ(-1, r.lo());
  EXPECT_EQ(5, r.hi());

  // FromPointPair()
  EXPECT_EQ(R1Interval(4, 4), R1Interval::FromPointPair(4, 4));
  EXPECT_EQ(R1Interval(-2, -1), R1Interval::FromPointPair(-1, -2));
  EXPECT_EQ(R1Interval(-5, 3), R1Interval::FromPointPair(-5, 3));

  // Expanded()
  EXPECT_EQ(empty, empty.Expanded(0.45));
  EXPECT_EQ(R1Interval(-0.5, 1.5), unit.Expanded(0.5));

  // Union(), Intersection()
  EXPECT_EQ(R1Interval(99, 100), R1Interval(99,100).Union(empty));
  EXPECT_EQ(R1Interval(99, 100), empty.Union(R1Interval(99,100)));
  EXPECT_TRUE(R1Interval(5,3).Union(R1Interval(0,-2)).is_empty());
  EXPECT_TRUE(R1Interval(0,-2).Union(R1Interval(5,3)).is_empty());
  EXPECT_EQ(unit, unit.Union(unit));
  EXPECT_EQ(R1Interval(-1, 1), unit.Union(negunit));
  EXPECT_EQ(R1Interval(-1, 1), negunit.Union(unit));
  EXPECT_EQ(unit, half.Union(unit));
  EXPECT_EQ(half, unit.Intersection(half));
  EXPECT_EQ(R1Interval(0, 0), unit.Intersection(negunit));
  EXPECT_TRUE(negunit.Intersection(half).is_empty());
  EXPECT_TRUE(unit.Intersection(empty).is_empty());
  EXPECT_TRUE(empty.Intersection(unit).is_empty());
}
