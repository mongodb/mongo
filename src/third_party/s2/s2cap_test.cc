// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2cap.h"

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "testing/base/public/gunit.h"
#include "s2.h"
#include "s2cell.h"
#include "s2latlng.h"
#include "s2latlngrect.h"

static S2Point GetLatLngPoint(double lat_degrees, double lng_degrees) {
  return S2LatLng::FromDegrees(lat_degrees, lng_degrees).ToPoint();
}

// About 9 times the double-precision roundoff relative error.
static const double kEps = 1e-15;

TEST(S2Cap, Basic) {
  // Test basic properties of empty and full caps.
  S2Cap empty = S2Cap::Empty();
  S2Cap full = S2Cap::Full();
  EXPECT_TRUE(empty.is_valid());
  EXPECT_TRUE(empty.is_empty());
  EXPECT_TRUE(empty.Complement().is_full());
  EXPECT_TRUE(full.is_valid());
  EXPECT_TRUE(full.is_full());
  EXPECT_TRUE(full.Complement().is_empty());
  EXPECT_EQ(2, full.height());
  EXPECT_DOUBLE_EQ(180.0, full.angle().degrees());

  // Check that the default S2Cap is identical to Empty().
  S2Cap default_empty;
  EXPECT_TRUE(default_empty.is_valid());
  EXPECT_TRUE(default_empty.is_empty());
  EXPECT_EQ(empty.axis(), default_empty.axis());
  EXPECT_EQ(empty.height(), default_empty.height());

  // Containment and intersection of empty and full caps.
  EXPECT_TRUE(empty.Contains(empty));
  EXPECT_TRUE(full.Contains(empty));
  EXPECT_TRUE(full.Contains(full));
  EXPECT_FALSE(empty.InteriorIntersects(empty));
  EXPECT_TRUE(full.InteriorIntersects(full));
  EXPECT_FALSE(full.InteriorIntersects(empty));

  // Singleton cap containing the x-axis.
  S2Cap xaxis = S2Cap::FromAxisHeight(S2Point(1, 0, 0), 0);
  EXPECT_TRUE(xaxis.Contains(S2Point(1, 0, 0)));
  EXPECT_TRUE(xaxis.VirtualContainsPoint(S2Point(1, 0, 0)));
  EXPECT_FALSE(xaxis.Contains(S2Point(1, 1e-20, 0)));
  EXPECT_FALSE(xaxis.VirtualContainsPoint(S2Point(1, 1e-20, 0)));
  EXPECT_EQ(0, xaxis.angle().radians());

  // Singleton cap containing the y-axis.
  S2Cap yaxis = S2Cap::FromAxisAngle(S2Point(0, 1, 0), S1Angle::Radians(0));
  EXPECT_FALSE(yaxis.Contains(xaxis.axis()));
  EXPECT_EQ(0, xaxis.height());

  // Check that the complement of a singleton cap is the full cap.
  S2Cap xcomp = xaxis.Complement();
  EXPECT_TRUE(xcomp.is_valid());
  EXPECT_TRUE(xcomp.is_full());
  EXPECT_TRUE(xcomp.Contains(xaxis.axis()));

  // Check that the complement of the complement is *not* the original.
  EXPECT_TRUE(xcomp.Complement().is_valid());
  EXPECT_TRUE(xcomp.Complement().is_empty());
  EXPECT_FALSE(xcomp.Complement().Contains(xaxis.axis()));

  // Check that very small caps can be represented accurately.
  // Here "kTinyRad" is small enough that unit vectors perturbed by this
  // amount along a tangent do not need to be renormalized.
  static const double kTinyRad = 1e-10;
  S2Cap tiny = S2Cap::FromAxisAngle(S2Point(1, 2, 3).Normalize(),
                                    S1Angle::Radians(kTinyRad));
  S2Point tangent = tiny.axis().CrossProd(S2Point(3, 2, 1)).Normalize();
  EXPECT_TRUE(tiny.Contains(tiny.axis() + 0.99 * kTinyRad * tangent));
  EXPECT_FALSE(tiny.Contains(tiny.axis() + 1.01 * kTinyRad * tangent));

  // Basic tests on a hemispherical cap.
  S2Cap hemi = S2Cap::FromAxisHeight(S2Point(1, 0, 1).Normalize(), 1);
  EXPECT_EQ(-hemi.axis(), S2Point(hemi.Complement().axis()));
  EXPECT_EQ(1, hemi.Complement().height());
  EXPECT_TRUE(hemi.Contains(S2Point(1, 0, 0)));
  EXPECT_FALSE(hemi.Complement().Contains(S2Point(1, 0, 0)));
  EXPECT_TRUE(hemi.Contains(S2Point(1, 0, -(1-kEps)).Normalize()));
  EXPECT_FALSE(hemi.InteriorContains(S2Point(1, 0, -(1+kEps)).Normalize()));

  // A concave cap.
  S2Cap concave = S2Cap::FromAxisAngle(GetLatLngPoint(80, 10),
                                       S1Angle::Degrees(150));
  EXPECT_TRUE(concave.Contains(GetLatLngPoint(-70 * (1 - kEps), 10)));
  EXPECT_FALSE(concave.Contains(GetLatLngPoint(-70 * (1 + kEps), 10)));
  EXPECT_TRUE(concave.Contains(GetLatLngPoint(-50 * (1 - kEps), -170)));
  EXPECT_FALSE(concave.Contains(GetLatLngPoint(-50 * (1 + kEps), -170)));

  // Cap containment tests.
  EXPECT_FALSE(empty.Contains(xaxis));
  EXPECT_FALSE(empty.InteriorIntersects(xaxis));
  EXPECT_TRUE(full.Contains(xaxis));
  EXPECT_TRUE(full.InteriorIntersects(xaxis));
  EXPECT_FALSE(xaxis.Contains(full));
  EXPECT_FALSE(xaxis.InteriorIntersects(full));
  EXPECT_TRUE(xaxis.Contains(xaxis));
  EXPECT_FALSE(xaxis.InteriorIntersects(xaxis));
  EXPECT_TRUE(xaxis.Contains(empty));
  EXPECT_FALSE(xaxis.InteriorIntersects(empty));
  EXPECT_TRUE(hemi.Contains(tiny));
  EXPECT_TRUE(hemi.Contains(
                  S2Cap::FromAxisAngle(S2Point(1, 0, 0),
                                       S1Angle::Radians(M_PI_4 - kEps))));
  EXPECT_FALSE(hemi.Contains(
                   S2Cap::FromAxisAngle(S2Point(1, 0, 0),
                                        S1Angle::Radians(M_PI_4 + kEps))));
  EXPECT_TRUE(concave.Contains(hemi));
  EXPECT_TRUE(concave.InteriorIntersects(hemi.Complement()));
  EXPECT_FALSE(concave.Contains(S2Cap::FromAxisHeight(-concave.axis(), 0.1)));
}

TEST(S2Cap, GetRectBound) {
  // Empty and full caps.
  EXPECT_TRUE(S2Cap::Empty().GetRectBound().is_empty());
  EXPECT_TRUE(S2Cap::Full().GetRectBound().is_full());

  static const double kDegreeEps = 1e-13;
  // Maximum allowable error for latitudes and longitudes measured in
  // degrees.  (EXPECT_DOUBLE_EQ isn't sufficient.)

  // Cap that includes the south pole.
  S2LatLngRect rect = S2Cap::FromAxisAngle(GetLatLngPoint(-45, 57),
                                           S1Angle::Degrees(50)).GetRectBound();
  EXPECT_NEAR(rect.lat_lo().degrees(), -90, kDegreeEps);
  EXPECT_NEAR(rect.lat_hi().degrees(), 5, kDegreeEps);
  EXPECT_TRUE(rect.lng().is_full());

  // Cap that is tangent to the north pole.
  rect = S2Cap::FromAxisAngle(S2Point(1, 0, 1).Normalize(),
                              S1Angle::Radians(M_PI_4 + 1e-16)).GetRectBound();
  EXPECT_NEAR(rect.lat().lo(), 0, kEps);
  EXPECT_NEAR(rect.lat().hi(), M_PI_2, kEps);
  EXPECT_TRUE(rect.lng().is_full());

  rect = S2Cap::FromAxisAngle(S2Point(1, 0, 1).Normalize(),
                              S1Angle::Degrees(45 + 5e-15)).GetRectBound();
  EXPECT_NEAR(rect.lat_lo().degrees(), 0, kDegreeEps);
  EXPECT_NEAR(rect.lat_hi().degrees(), 90, kDegreeEps);
  EXPECT_TRUE(rect.lng().is_full());

  // The eastern hemisphere.
  rect = S2Cap::FromAxisAngle(S2Point(0, 1, 0),
                              S1Angle::Radians(M_PI_2 + 2e-16)).GetRectBound();
  EXPECT_NEAR(rect.lat_lo().degrees(), -90, kDegreeEps);
  EXPECT_NEAR(rect.lat_hi().degrees(), 90, kDegreeEps);
  EXPECT_TRUE(rect.lng().is_full());

  // A cap centered on the equator.
  rect = S2Cap::FromAxisAngle(GetLatLngPoint(0, 50),
                              S1Angle::Degrees(20)).GetRectBound();
  EXPECT_NEAR(rect.lat_lo().degrees(), -20, kDegreeEps);
  EXPECT_NEAR(rect.lat_hi().degrees(), 20, kDegreeEps);
  EXPECT_NEAR(rect.lng_lo().degrees(), 30, kDegreeEps);
  EXPECT_NEAR(rect.lng_hi().degrees(), 70, kDegreeEps);

  // A cap centered on the north pole.
  rect = S2Cap::FromAxisAngle(GetLatLngPoint(90, 123),
                              S1Angle::Degrees(10)).GetRectBound();
  EXPECT_NEAR(rect.lat_lo().degrees(), 80, kDegreeEps);
  EXPECT_NEAR(rect.lat_hi().degrees(), 90, kDegreeEps);
  EXPECT_TRUE(rect.lng().is_full());
}

TEST(S2Cap, S2CellMethods) {
  // For each cube face, we construct some cells on
  // that face and some caps whose positions are relative to that face,
  // and then check for the expected intersection/containment results.

  // The distance from the center of a face to one of its vertices.
  static const double kFaceRadius = atan(sqrt(2));

  for (int face = 0; face < 6; ++face) {
    // The cell consisting of the entire face.
    S2Cell root_cell = S2Cell::FromFacePosLevel(face, 0, 0);

    // A leaf cell at the midpoint of the v=1 edge.
    S2Cell edge_cell(S2::FaceUVtoXYZ(face, 0, 1 - kEps));

    // A leaf cell at the u=1, v=1 corner.
    S2Cell corner_cell(S2::FaceUVtoXYZ(face, 1 - kEps, 1 - kEps));

    // Quick check for full and empty caps.
    EXPECT_TRUE(S2Cap::Full().Contains(root_cell));
    EXPECT_FALSE(S2Cap::Empty().MayIntersect(root_cell));

    // Check intersections with the bounding caps of the leaf cells that are
    // adjacent to 'corner_cell' along the Hilbert curve.  Because this corner
    // is at (u=1,v=1), the curve stays locally within the same cube face.
    S2CellId first = corner_cell.id().advance(-3);
    S2CellId last = corner_cell.id().advance(4);
    for (S2CellId id = first; id < last; id = id.next()) {
      S2Cell cell(id);
      EXPECT_EQ(id == corner_cell.id(),
                cell.GetCapBound().Contains(corner_cell));
      EXPECT_EQ(id.parent().contains(corner_cell.id()),
                cell.GetCapBound().MayIntersect(corner_cell));
    }

    int anti_face = (face + 3) % 6;  // Opposite face.
    for (int cap_face = 0; cap_face < 6; ++cap_face) {
      // A cap that barely contains all of 'cap_face'.
      S2Point center = S2::GetNorm(cap_face);
      S2Cap covering = S2Cap::FromAxisAngle(
          center, S1Angle::Radians(kFaceRadius + kEps));
      EXPECT_EQ(cap_face == face, covering.Contains(root_cell));
      EXPECT_EQ(cap_face != anti_face, covering.MayIntersect(root_cell));
      EXPECT_EQ(center.DotProd(edge_cell.GetCenter()) > 0.1,
                covering.Contains(edge_cell));
      EXPECT_EQ(covering.MayIntersect(edge_cell), covering.Contains(edge_cell));
      EXPECT_EQ(cap_face == face, covering.Contains(corner_cell));
      EXPECT_EQ(center.DotProd(corner_cell.GetCenter()) > 0,
                covering.MayIntersect(corner_cell));

      // A cap that barely intersects the edges of 'cap_face'.
      S2Cap bulging = S2Cap::FromAxisAngle(
          center, S1Angle::Radians(M_PI_4 + kEps));
      EXPECT_FALSE(bulging.Contains(root_cell));
      EXPECT_EQ(cap_face != anti_face, bulging.MayIntersect(root_cell));
      EXPECT_EQ(cap_face == face, bulging.Contains(edge_cell));
      EXPECT_EQ(center.DotProd(edge_cell.GetCenter()) > 0.1,
                bulging.MayIntersect(edge_cell));
      EXPECT_FALSE(bulging.Contains(corner_cell));
      EXPECT_FALSE(bulging.MayIntersect(corner_cell));

      // A singleton cap.
      S2Cap singleton = S2Cap::FromAxisAngle(center, S1Angle::Radians(0));
      EXPECT_EQ(cap_face == face, singleton.MayIntersect(root_cell));
      EXPECT_FALSE(singleton.MayIntersect(edge_cell));
      EXPECT_FALSE(singleton.MayIntersect(corner_cell));
    }
  }
}

TEST(S2Cap, Expanded) {
  EXPECT_TRUE(S2Cap::Empty().Expanded(S1Angle::Radians(2)).is_empty());
  EXPECT_TRUE(S2Cap::Full().Expanded(S1Angle::Radians(2)).is_full());
  S2Cap cap50 = S2Cap::FromAxisAngle(S2Point(1, 0, 0), S1Angle::Degrees(50));
  S2Cap cap51 = S2Cap::FromAxisAngle(S2Point(1, 0, 0), S1Angle::Degrees(51));
  EXPECT_TRUE(cap50.Expanded(S1Angle::Radians(0)).ApproxEquals(cap50));
  EXPECT_TRUE(cap50.Expanded(S1Angle::Degrees(1)).ApproxEquals(cap51));
  EXPECT_FALSE(cap50.Expanded(S1Angle::Degrees(129.99)).is_full());
  EXPECT_TRUE(cap50.Expanded(S1Angle::Degrees(130.01)).is_full());
}
