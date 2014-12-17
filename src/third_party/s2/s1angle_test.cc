// Copyright 2005 Google Inc. All Rights Reserved.

#include "s1angle.h"

#include <sstream>

#include "base/commandlineflags.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "gtest/gtest.h"
#include "s2latlng.h"
#include "s2testing.h"

using namespace std;

DEFINE_int32(iters, (DEBUG_MODE ? 100 : 1000) * (1000 * 1000),
             "Run timing tests with this many iterations");

TEST(S1Angle, DefaultConstructor) {
  // Check that the default constructor returns an angle of 0.
  S1Angle a;
  EXPECT_EQ(0, a.radians());
}

TEST(S1Angle, PiRadiansExactly180Degrees) {
  // Check that the conversion between Pi radians and 180 degrees is exact.
  EXPECT_EQ(M_PI, S1Angle::Radians(M_PI).radians());
  EXPECT_EQ(180.0, S1Angle::Radians(M_PI).degrees());
  EXPECT_EQ(M_PI, S1Angle::Degrees(180).radians());
  EXPECT_EQ(180.0, S1Angle::Degrees(180).degrees());

  EXPECT_EQ(90.0, S1Angle::Radians(M_PI_2).degrees());

  // Check negative angles.
  EXPECT_EQ(-90.0, S1Angle::Radians(-M_PI_2).degrees());
  EXPECT_EQ(-M_PI_4, S1Angle::Degrees(-45).radians());
}

TEST(S1Angle, E5E6E7Representations) {
  // Check that E5/E6/E7 representations work as expected.
  EXPECT_DOUBLE_EQ(S1Angle::Degrees(-45).radians(),
                   S1Angle::E5(-4500000).radians());
  EXPECT_DOUBLE_EQ(S1Angle::Degrees(-60).radians(),
                   S1Angle::E6(-60000000).radians());
  EXPECT_DOUBLE_EQ(S1Angle::Degrees(75).radians(),
                   S1Angle::E7(750000000).radians());
  EXPECT_EQ(-17256123, S1Angle::Degrees(-172.56123).e5());
  EXPECT_EQ(12345678, S1Angle::Degrees(12.345678).e6());
  EXPECT_EQ(-123456789, S1Angle::Degrees(-12.3456789).e7());
}

TEST(S1Angle, E6E7RepresentationsUnsigned) {
  // Check that unsigned E6/E7 representations work as expected.
  EXPECT_DOUBLE_EQ(
      S1Angle::Degrees(60).radians(),
      S1Angle::UnsignedE6(static_cast<uint32>(60000000)).radians());
  EXPECT_DOUBLE_EQ(
      S1Angle::Degrees(-60).radians(),
      S1Angle::UnsignedE6(static_cast<uint32>(-60000000)).radians());
  EXPECT_DOUBLE_EQ(
      S1Angle::Degrees(75).radians(),
      S1Angle::UnsignedE7(static_cast<uint32>(750000000)).radians());
  EXPECT_DOUBLE_EQ(
      S1Angle::Degrees(-75).radians(),
      S1Angle::UnsignedE7(static_cast<uint32>(-750000000)).radians());
}

TEST(S1Angle, NormalizeCorrectlyCanonicalizesAngles) {
  EXPECT_DOUBLE_EQ(0.0, S1Angle::Degrees(360.0).Normalized().degrees());
  EXPECT_DOUBLE_EQ(180.0, S1Angle::Degrees(-180.0).Normalized().degrees());
  EXPECT_DOUBLE_EQ(180.0, S1Angle::Degrees(180.0).Normalized().degrees());
  EXPECT_DOUBLE_EQ(180.0, S1Angle::Degrees(540.0).Normalized().degrees());
  EXPECT_DOUBLE_EQ(90.0, S1Angle::Degrees(-270.0).Normalized().degrees());
}

TEST(S1Angle, ArithmeticOperationsOnAngles) {
  EXPECT_DOUBLE_EQ(0.3, S1Angle::Radians(-0.3).abs().radians());
  EXPECT_DOUBLE_EQ(-0.1, (-S1Angle::Radians(0.1)).radians());
  EXPECT_DOUBLE_EQ(0.4,
                   (S1Angle::Radians(0.1) + S1Angle::Radians(0.3)).radians());
  EXPECT_DOUBLE_EQ(-0.2,
                   (S1Angle::Radians(0.1) - S1Angle::Radians(0.3)).radians());
  EXPECT_DOUBLE_EQ(0.6, (2 * S1Angle::Radians(0.3)).radians());
  EXPECT_DOUBLE_EQ(0.6, (S1Angle::Radians(0.3) * 2).radians());
  EXPECT_DOUBLE_EQ(0.15, (S1Angle::Radians(0.3) / 2).radians());
  EXPECT_DOUBLE_EQ(0.5, (S1Angle::Radians(0.3) / S1Angle::Radians(0.6)));

  S1Angle tmp = S1Angle::Radians(1.0);
  tmp += S1Angle::Radians(0.5);
  EXPECT_DOUBLE_EQ(1.5, tmp.radians());
  tmp -= S1Angle::Radians(1.0);
  EXPECT_DOUBLE_EQ(0.5, tmp.radians());
  tmp *= 5;
  EXPECT_DOUBLE_EQ(2.5, tmp.radians());
  tmp /= 2;
  EXPECT_DOUBLE_EQ(1.25, tmp.radians());
}

TEST(S1Angle, ConstructorsThatMeasureAngles) {
  EXPECT_DOUBLE_EQ(M_PI_2,
                   S1Angle(S2Point(1, 0, 0), S2Point(0, 0, 2)).radians());
  EXPECT_DOUBLE_EQ(0.0, S1Angle(S2Point(1, 0, 0), S2Point(1, 0, 0)).radians());
  EXPECT_NEAR(50.0,
              S1Angle(S2LatLng::FromDegrees(20, 20),
                      S2LatLng::FromDegrees(70, 20)).degrees(),
              1e-13);
}

TEST(S1Angle, TestFormatting) {
  ostringstream ss;
  ss << S1Angle::Degrees(180.0);
  EXPECT_EQ("180.0000000", ss.str());
}

TEST(S1Angle, TestPerformance) {
  // Verify that the conversion to E5/E6/E7 is not much slower than the
  // conversion from E5/E6/E7.  (Float-to-integer conversions can be quite
  // slow on some platforms.)  We only check the times for E6; the times for
  // E5/E7 should be similar.

  // To reduce the impact of loop overhead, we do kOpsPerLoop ops per loop.
  static const int kOpsPerLoop = 8;

  // Time conversion from E6 to radians.
  double rad_sum = 0;
  const double from_e6_start = S2Testing::GetCpuTime();
  for (int i = FLAGS_iters; i > 0; i -= kOpsPerLoop) {
    // We structure both loops so that all the conversions can be done in
    // parallel.  Otherwise on some platforms the optimizer happens to do a
    // much better job of parallelizing one loop than the other.
    double r0 = S1Angle::E6(i-0).radians();
    double r1 = S1Angle::E6(i-1).radians();
    double r2 = S1Angle::E6(i-2).radians();
    double r3 = S1Angle::E6(i-3).radians();
    double r4 = S1Angle::E6(i-4).radians();
    double r5 = S1Angle::E6(i-5).radians();
    double r6 = S1Angle::E6(i-6).radians();
    double r7 = S1Angle::E6(i-7).radians();
    rad_sum += ((r0 + r1) + (r2 + r3)) + ((r4 + r5) + (r6 + r7));
  }
  const double from_e6_time = S2Testing::GetCpuTime() - from_e6_start;
  EXPECT_NE(rad_sum, 0);  // Don't let the sum get optimized away.
  LOG(INFO) << "From E6: "
            << (FLAGS_iters / from_e6_time)
            << " values per second";

  // Time conversion from radians to E6.
  const double delta = (2 * M_PI) / (FLAGS_iters - 1);
  double angle = -M_PI;
  long e6_sum = 0;
  const double to_e6_start = S2Testing::GetCpuTime();
  for (int i = FLAGS_iters; i > 0; i -= kOpsPerLoop) {
    long r0 = S1Angle::Radians(angle).e6(); angle += delta;
    long r1 = S1Angle::Radians(angle).e6(); angle += delta;
    long r2 = S1Angle::Radians(angle).e6(); angle += delta;
    long r3 = S1Angle::Radians(angle).e6(); angle += delta;
    long r4 = S1Angle::Radians(angle).e6(); angle += delta;
    long r5 = S1Angle::Radians(angle).e6(); angle += delta;
    long r6 = S1Angle::Radians(angle).e6(); angle += delta;
    long r7 = S1Angle::Radians(angle).e6(); angle += delta;
    e6_sum += ((r0 + r1) + (r2 + r3)) + ((r4 + r5) + (r6 + r7));
  }
  const double to_e6_time = S2Testing::GetCpuTime() - to_e6_start;
  EXPECT_NE(e6_sum + angle, 0);  // Don't let them get optimized away.
  LOG(INFO) << "  To E6: "
            << (FLAGS_iters / to_e6_time)
            << " values per second";

  // Make sure that the To/From E6 times are not much different.
  // The difference factor slightly less than 2 on an x86_64.
  EXPECT_LE(from_e6_time / to_e6_time, 3);
  EXPECT_LE(to_e6_time / from_e6_time, 3);
}
