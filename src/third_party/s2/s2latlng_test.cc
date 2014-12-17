// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2latlng.h"
#include "base/macros.h"
#include "base/stringprintf.h"
#include "strings/split.h"
#include "testing/base/public/gunit.h"
#include "testing/base/public/benchmark.h"
#include "s2testing.h"

TEST(S2LatLng, TestBasic) {
  S2LatLng ll_rad = S2LatLng::FromRadians(M_PI_4, M_PI_2);
  EXPECT_EQ(M_PI_4, ll_rad.lat().radians());
  EXPECT_EQ(M_PI_2, ll_rad.lng().radians());
  EXPECT_TRUE(ll_rad.is_valid());
  S2LatLng ll_deg = S2LatLng::FromDegrees(45, 90);
  EXPECT_EQ(ll_rad, ll_deg);
  EXPECT_TRUE(ll_deg.is_valid());
  EXPECT_FALSE(S2LatLng::FromDegrees(-91, 0).is_valid());
  EXPECT_FALSE(S2LatLng::FromDegrees(0, 181).is_valid());

  S2LatLng bad = S2LatLng::FromDegrees(120, 200);
  EXPECT_FALSE(bad.is_valid());
  S2LatLng better = bad.Normalized();
  EXPECT_TRUE(better.is_valid());
  EXPECT_EQ(S1Angle::Degrees(90), better.lat());
  EXPECT_DOUBLE_EQ(S1Angle::Degrees(-160).radians(), better.lng().radians());

  bad = S2LatLng::FromDegrees(-100, -360);
  EXPECT_FALSE(bad.is_valid());
  better = bad.Normalized();
  EXPECT_TRUE(better.is_valid());
  EXPECT_EQ(S1Angle::Degrees(-90), better.lat());
  EXPECT_DOUBLE_EQ(0.0, better.lng().radians());

  EXPECT_TRUE((S2LatLng::FromDegrees(10, 20) + S2LatLng::FromDegrees(20, 30)).
              ApproxEquals(S2LatLng::FromDegrees(30, 50)));
  EXPECT_TRUE((S2LatLng::FromDegrees(10, 20) - S2LatLng::FromDegrees(20, 30)).
              ApproxEquals(S2LatLng::FromDegrees(-10, -10)));
  EXPECT_TRUE((0.5 * S2LatLng::FromDegrees(10, 20)).
              ApproxEquals(S2LatLng::FromDegrees(5, 10)));

  // Check that Invalid() returns an invalid point.
  S2LatLng invalid = S2LatLng::Invalid();
  EXPECT_FALSE(invalid.is_valid());

  // Check that the default constructor sets latitude and longitude to 0.
  S2LatLng default_ll;
  EXPECT_TRUE(default_ll.is_valid());
  EXPECT_EQ(0, default_ll.lat().radians());
  EXPECT_EQ(0, default_ll.lng().radians());
}

TEST(S2LatLng, TestConversion) {
  // Test special cases: poles, "date line"
  EXPECT_DOUBLE_EQ(90.0,
                   S2LatLng(S2LatLng::FromDegrees(90.0, 65.0).ToPoint())
                   .lat().degrees());
  EXPECT_EQ(-M_PI_2,
            S2LatLng(S2LatLng::FromRadians(-M_PI_2, 1).ToPoint())
            .lat().radians());
  EXPECT_DOUBLE_EQ(180.0,
                   fabs(S2LatLng(S2LatLng::FromDegrees(12.2, 180.0).ToPoint())
                        .lng().degrees()));
  EXPECT_EQ(M_PI,
            fabs(S2LatLng(S2LatLng::FromRadians(0.1, -M_PI).ToPoint())
                 .lng().radians()));

  // Test a bunch of random points.
  for (int i = 0; i < 100000; ++i) {
    S2Point p = S2Testing::RandomPoint();
    EXPECT_TRUE(S2::ApproxEquals(p, S2LatLng(p).ToPoint())) << p;
  }
}

TEST(S2LatLng, TestDistance) {
  EXPECT_EQ(0.0,
            S2LatLng::FromDegrees(90, 0).GetDistance(
                S2LatLng::FromDegrees(90, 0)).radians());
  EXPECT_NEAR(77.0,
              S2LatLng::FromDegrees(-37, 25).GetDistance(
                  S2LatLng::FromDegrees(-66, -155)).degrees(),
              1e-13);
  EXPECT_NEAR(115.0,
              S2LatLng::FromDegrees(0, 165).GetDistance(
                  S2LatLng::FromDegrees(0, -80)).degrees(),
              1e-13);
  EXPECT_NEAR(180.0,
              S2LatLng::FromDegrees(47, -127).GetDistance(
                  S2LatLng::FromDegrees(-47, 53)).degrees(),
              2e-6);
}

TEST(S2LatLng, TestToString) {
  struct {
    double lat, lng;
    double expected_lat, expected_lng;
  } values[] = {
    {0, 0, 0, 0},
    {1.5, 91.7, 1.5, 91.7},
    {9.9, -0.31, 9.9, -0.31},
    {sqrt(2), -sqrt(5), 1.414214, -2.236068},
    {91.3, 190.4, 90, -169.6},
    {-100, -710, -90, 10},
  };
  for (int i = 0; i < ARRAYSIZE(values); ++i) {
    SCOPED_TRACE(StringPrintf("Iteration %d", i));
    S2LatLng p = S2LatLng::FromDegrees(values[i].lat, values[i].lng);
    string output;
    p.ToStringInDegrees(&output);

    const char *ptr = output.c_str();
    double lat, lng;
    EXPECT_TRUE(SplitOneDoubleToken(&ptr, ",", &lat));
    ASSERT_TRUE(ptr != NULL);
    EXPECT_TRUE(SplitOneDoubleToken(&ptr, ",", &lng));
    EXPECT_NEAR(values[i].expected_lat, lat, 1e-8);
    EXPECT_NEAR(values[i].expected_lng, lng, 1e-8);
  }
}

// Test the variant that returns a string.
TEST(S2LatLng, TestToStringReturnsString) {
  string s;
  S2LatLng::FromDegrees(0, 1).ToStringInDegrees(&s);
  EXPECT_EQ(S2LatLng::FromDegrees(0, 1).ToStringInDegrees(), s);
}


static void BM_ToPoint(int iters) {
    S2LatLng ll(S1Angle::E7(0x150bc888), S1Angle::E7(0x5099d63f));
    for (int i = 0; i < iters; i++) {
      ll.ToPoint();
    }
}
BENCHMARK(BM_ToPoint);
