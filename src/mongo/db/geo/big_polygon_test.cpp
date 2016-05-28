/**
 *    Copyright (C) 2014 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/geo/big_polygon.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using std::unique_ptr;
using std::string;
using std::vector;

// Helper to build a vector of S2Point
struct PointBuilder {
    vector<S2Point> points;

    PointBuilder& operator<<(const S2LatLng& LatLng) {
        points.push_back(LatLng.ToPoint());
        return *this;
    }
};

vector<S2Point> pointVec(const PointBuilder& builder) {
    vector<S2Point> points(builder.points.begin(), builder.points.end());
    return points;
}

S2Loop* loop(const PointBuilder& builder) {
    return new S2Loop(builder.points);
}

vector<S2Loop*>* loopVec(const PointBuilder& builder) {
    static vector<S2Loop*> loops;
    loops.clear();
    loops.push_back(loop(builder));
    return &loops;
}

S2LatLng LatLng(double lat, double lng) {
    return S2LatLng::FromDegrees(lat, lng);
}

// Syntax sugar for PointBuilder, which can be used to construct
// - vector<S2Point> pointVec()
// - S2Loop* loop()
// - vector<S2Loop*>* loopVec()
//
// e.g. points() << LatLng(10.0, 10.0) << LatLng(10.0, -10.0) << LatLng(0.0, 0.0))
typedef PointBuilder points;

TEST(BigSimplePolygon, Basic) {
    // A 20x20 square centered at [0,0]
    BigSimplePolygon bigPoly20(loop(points() << LatLng(10.0, 10.0) << LatLng(10.0, -10.0)
                                             << LatLng(-10.0, -10.0)
                                             << LatLng(-10.0, 10.0)));

    // A 10x10 square centered at [0,0]
    S2Polygon poly10(loopVec(points() << LatLng(5.0, 5.0) << LatLng(5.0, -5.0) << LatLng(-5.0, -5.0)
                                      << LatLng(-5.0, 5.0)));

    ASSERT_LESS_THAN(bigPoly20.GetArea(), 2 * M_PI);
    ASSERT_LESS_THAN(poly10.GetArea(), bigPoly20.GetArea());
    ASSERT(bigPoly20.Contains(poly10));
    ASSERT(bigPoly20.Intersects(poly10));

    // A 20x20 square centered at [0,20]
    BigSimplePolygon bigPoly20Offset(loop(points() << LatLng(10.0, 30.0) << LatLng(10.0, 10.0)
                                                   << LatLng(-10.0, 10.0)
                                                   << LatLng(-10.0, 30.0)));

    ASSERT_LESS_THAN(bigPoly20Offset.GetArea(), 2 * M_PI);
    ASSERT_LESS_THAN(poly10.GetArea(), bigPoly20Offset.GetArea());
    ASSERT_FALSE(bigPoly20Offset.Contains(poly10));
    ASSERT_FALSE(bigPoly20Offset.Intersects(poly10));
}

TEST(BigSimplePolygon, BasicWithHole) {
    // A 30x30 square centered at [0,0] with a 20X20 hole
    vector<S2Loop*> loops;
    loops.push_back(loop(points() << LatLng(15.0, 15.0) << LatLng(15.0, -15.0)
                                  << LatLng(-15.0, -15.0)
                                  << LatLng(-15.0, 15.0)));
    loops.push_back(loop(points() << LatLng(10.0, 10.0) << LatLng(10.0, -10.0)
                                  << LatLng(-10.0, -10.0)
                                  << LatLng(-10.0, 10.0)));

    S2Polygon holePoly(&loops);

    // A 16X16 square centered at [0,0]
    BigSimplePolygon bigPoly16(loop(points() << LatLng(8.0, 8.0) << LatLng(8.0, -8.0)
                                             << LatLng(-8.0, -8.0)
                                             << LatLng(-8.0, 8.0)));

    ASSERT_LESS_THAN(bigPoly16.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly16.Contains(holePoly));
    ASSERT_FALSE(bigPoly16.Intersects(holePoly));

    // A big polygon bigger than the hole.
    BigSimplePolygon bigPoly24(loop(points() << LatLng(12.0, 12.0) << LatLng(12.0, -12.0)
                                             << LatLng(-12.0, -12.0)
                                             << LatLng(-12.0, 12.0)));
    ASSERT_LESS_THAN(bigPoly24.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly24.Contains(holePoly));
    ASSERT_TRUE(bigPoly24.Intersects(holePoly));
}

TEST(BigSimplePolygon, BasicWithHoleAndShell) {
    // A 30x30 square centered at [0,0] with a 20X20 hole and 10X10 shell
    vector<S2Loop*> loops;
    // Border
    loops.push_back(loop(points() << LatLng(15.0, 15.0) << LatLng(15.0, -15.0)
                                  << LatLng(-15.0, -15.0)
                                  << LatLng(-15.0, 15.0)));
    // Hole
    loops.push_back(loop(points() << LatLng(10.0, 10.0) << LatLng(10.0, -10.0)
                                  << LatLng(-10.0, -10.0)
                                  << LatLng(-10.0, 10.0)));
    // Shell
    loops.push_back(loop(points() << LatLng(5.0, 5.0) << LatLng(5.0, -5.0) << LatLng(-5.0, -5.0)
                                  << LatLng(-5.0, 5.0)));
    S2Polygon shellPoly(&loops);

    // A 16X16 square centered at [0,0] containing the shell
    BigSimplePolygon bigPoly16(loop(points() << LatLng(8.0, 8.0) << LatLng(8.0, -8.0)
                                             << LatLng(-8.0, -8.0)
                                             << LatLng(-8.0, 8.0)));
    ASSERT_LESS_THAN(bigPoly16.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly16.Contains(shellPoly));
    ASSERT_TRUE(bigPoly16.Intersects(shellPoly));

    // Try a big polygon bigger than the hole.
    BigSimplePolygon bigPoly24(loop(points() << LatLng(12.0, 12.0) << LatLng(12.0, -12.0)
                                             << LatLng(-12.0, -12.0)
                                             << LatLng(-12.0, 12.0)));
    ASSERT_LESS_THAN(bigPoly24.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly24.Contains(shellPoly));
    ASSERT_TRUE(bigPoly24.Intersects(shellPoly));

    // Try a big polygon smaller than the shell.
    BigSimplePolygon bigPoly8(loop(points() << LatLng(4.0, 4.0) << LatLng(4.0, -4.0)
                                            << LatLng(-4.0, -4.0)
                                            << LatLng(-4.0, 4.0)));
    ASSERT_LESS_THAN(bigPoly8.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly8.Contains(shellPoly));
    ASSERT_TRUE(bigPoly8.Intersects(shellPoly));
}

TEST(BigSimplePolygon, BasicComplement) {
    // Everything *not* in a 20x20 square centered at [0,0]
    BigSimplePolygon bigPoly20Comp(loop(points() << LatLng(10.0, 10.0) << LatLng(10.0, -10.0)
                                                 << LatLng(-10.0, -10.0)
                                                 << LatLng(-10.0, 10.0)));
    bigPoly20Comp.Invert();

    // A 10x10 square centered at [0,0]
    S2Polygon poly10(loopVec(points() << LatLng(5.0, 5.0) << LatLng(5.0, -5.0) << LatLng(-5.0, -5.0)
                                      << LatLng(-5.0, 5.0)));

    ASSERT_GREATER_THAN(bigPoly20Comp.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly20Comp.Contains(poly10));
    ASSERT_FALSE(bigPoly20Comp.Intersects(poly10));

    // A 10x10 square centered at [0,20], contained by bigPoly20Comp
    S2Polygon poly10Contained(loopVec(points() << LatLng(25.0, 25.0) << LatLng(25.0, 15.0)
                                               << LatLng(15.0, 15.0)
                                               << LatLng(15.0, 25.0)));

    ASSERT_LESS_THAN(poly10Contained.GetArea(), bigPoly20Comp.GetArea());
    ASSERT(bigPoly20Comp.Contains(poly10Contained));
    ASSERT(bigPoly20Comp.Intersects(poly10Contained));

    // A 30x30 square centered at [0,0], so that bigPoly20Comp contains its complement entirely,
    // which is not allowed by S2.
    S2Polygon poly30(loopVec(points() << LatLng(15.0, 15.0) << LatLng(15.0, -15.0)
                                      << LatLng(-15.0, -15.0)
                                      << LatLng(-15.0, 15.0)));
    ASSERT_LESS_THAN(poly30.GetArea(), bigPoly20Comp.GetArea());
    ASSERT_FALSE(bigPoly20Comp.Contains(poly30));
    ASSERT_TRUE(bigPoly20Comp.Intersects(poly30));
}

TEST(BigSimplePolygon, BasicIntersects) {
    // Everything *not* in a 20x20 square centered at [0,0]
    BigSimplePolygon bigPoly20(loop(points() << LatLng(10.0, 10.0) << LatLng(10.0, -10.0)
                                             << LatLng(-10.0, -10.0)
                                             << LatLng(-10.0, 10.0)));
    bigPoly20.Invert();

    // A 10x10 square centered at [10,10] (partial overlap)
    S2Polygon poly10(loopVec(points() << LatLng(15.0, 15.0) << LatLng(15.0, 5.0) << LatLng(5.0, 5.0)
                                      << LatLng(5.0, 15.0)));

    ASSERT_FALSE(bigPoly20.Contains(poly10));
    ASSERT(bigPoly20.Intersects(poly10));
}

TEST(BigSimplePolygon, BasicComplementWithHole) {
    // A 30x30 square centered at [0,0] with a 20X20 hole
    vector<S2Loop*> loops;
    loops.push_back(loop(points() << LatLng(15.0, 15.0) << LatLng(15.0, -15.0)
                                  << LatLng(-15.0, -15.0)
                                  << LatLng(-15.0, 15.0)));
    loops.push_back(loop(points() << LatLng(10.0, 10.0) << LatLng(10.0, -10.0)
                                  << LatLng(-10.0, -10.0)
                                  << LatLng(-10.0, 10.0)));

    S2Polygon holePoly(&loops);

    // 1. BigPolygon doesn't touch holePoly
    // Everything *not* in a 40x40 square centered at [0,0]
    BigSimplePolygon bigPoly40Comp(loop(points() << LatLng(20.0, 20.0) << LatLng(20.0, -20.0)
                                                 << LatLng(-20.0, -20.0)
                                                 << LatLng(-20.0, 20.0)));
    bigPoly40Comp.Invert();
    ASSERT_GREATER_THAN(bigPoly40Comp.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly40Comp.Contains(holePoly));
    ASSERT_FALSE(bigPoly40Comp.Intersects(holePoly));

    // 2. BigPolygon intersects holePoly
    // Everything *not* in a 24X24 square centered at [0,0]
    BigSimplePolygon bigPoly24Comp(loop(points() << LatLng(12.0, 12.0) << LatLng(12.0, -12.0)
                                                 << LatLng(-12.0, -12.0)
                                                 << LatLng(-12.0, 12.0)));
    bigPoly24Comp.Invert();
    ASSERT_GREATER_THAN(bigPoly24Comp.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly24Comp.Contains(holePoly));
    ASSERT_TRUE(bigPoly24Comp.Intersects(holePoly));

    // 3. BigPolygon contains holePoly
    // Everything *not* in a 16X16 square centered at [0,0]
    BigSimplePolygon bigPoly16Comp(loop(points() << LatLng(8.0, 8.0) << LatLng(8.0, -8.0)
                                                 << LatLng(-8.0, -8.0)
                                                 << LatLng(-8.0, 8.0)));
    bigPoly16Comp.Invert();
    ASSERT_GREATER_THAN(bigPoly16Comp.GetArea(), 2 * M_PI);
    ASSERT_TRUE(bigPoly16Comp.Contains(holePoly));
    ASSERT_TRUE(bigPoly16Comp.Intersects(holePoly));

    // 4. BigPolygon contains the right half of holePoly
    // Everything *not* in a 40x40 square centered at [0,20]
    BigSimplePolygon bigPoly40CompOffset(loop(points() << LatLng(20.0, 40.0) << LatLng(20.0, 0.0)
                                                       << LatLng(-20.0, 0.0)
                                                       << LatLng(-20.0, 40.0)));
    bigPoly40CompOffset.Invert();
    ASSERT_GREATER_THAN(bigPoly40CompOffset.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly40CompOffset.Contains(holePoly));
    ASSERT_TRUE(bigPoly40CompOffset.Intersects(holePoly));
}

TEST(BigSimplePolygon, BasicComplementWithHoleAndShell) {
    // A 30x30 square centered at [0,0] with a 20X20 hole and 10X10 shell
    vector<S2Loop*> loops;
    // Border
    loops.push_back(loop(points() << LatLng(15.0, 15.0) << LatLng(15.0, -15.0)
                                  << LatLng(-15.0, -15.0)
                                  << LatLng(-15.0, 15.0)));
    // Hole
    loops.push_back(loop(points() << LatLng(10.0, 10.0) << LatLng(10.0, -10.0)
                                  << LatLng(-10.0, -10.0)
                                  << LatLng(-10.0, 10.0)));
    // Shell
    loops.push_back(loop(points() << LatLng(5.0, 5.0) << LatLng(5.0, -5.0) << LatLng(-5.0, -5.0)
                                  << LatLng(-5.0, 5.0)));
    S2Polygon shellPoly(&loops);

    // 1. BigPolygon doesn't touch shellPoly
    // Everything *not* in a 40x40 square centered at [0,0]
    BigSimplePolygon bigPoly40Comp(loop(points() << LatLng(20.0, 20.0) << LatLng(20.0, -20.0)
                                                 << LatLng(-20.0, -20.0)
                                                 << LatLng(-20.0, 20.0)));
    bigPoly40Comp.Invert();
    ASSERT_GREATER_THAN(bigPoly40Comp.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly40Comp.Contains(shellPoly));
    ASSERT_FALSE(bigPoly40Comp.Intersects(shellPoly));

    // 2. BigPolygon intersects shellPoly
    // Everything *not* in a 24X24 square centered at [0,0]
    BigSimplePolygon bigPoly24Comp(loop(points() << LatLng(12.0, 12.0) << LatLng(12.0, -12.0)
                                                 << LatLng(-12.0, -12.0)
                                                 << LatLng(-12.0, 12.0)));
    bigPoly24Comp.Invert();
    ASSERT_GREATER_THAN(bigPoly24Comp.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly24Comp.Contains(shellPoly));
    ASSERT_TRUE(bigPoly24Comp.Intersects(shellPoly));

    // 3. BigPolygon contains shellPoly's outer ring
    // Everything *not* in a 16X16 square centered at [0,0]
    BigSimplePolygon bigPoly16Comp(loop(points() << LatLng(8.0, 8.0) << LatLng(8.0, -8.0)
                                                 << LatLng(-8.0, -8.0)
                                                 << LatLng(-8.0, 8.0)));
    bigPoly16Comp.Invert();
    ASSERT_GREATER_THAN(bigPoly16Comp.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly16Comp.Contains(shellPoly));
    ASSERT_TRUE(bigPoly16Comp.Intersects(shellPoly));

    // 4. BigPolygon contains the right half of shellPoly
    // Everything *not* in a 40x40 square centered at [0,20]
    BigSimplePolygon bigPoly40CompOffset(loop(points() << LatLng(20.0, 40.0) << LatLng(20.0, 0.0)
                                                       << LatLng(-20.0, 0.0)
                                                       << LatLng(-20.0, 40.0)));
    bigPoly40CompOffset.Invert();
    ASSERT_GREATER_THAN(bigPoly40CompOffset.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly40CompOffset.Contains(shellPoly));
    ASSERT_TRUE(bigPoly40CompOffset.Intersects(shellPoly));

    // 5. BigPolygon contain shellPoly (CW)
    BigSimplePolygon bigPolyCompOffset(loop(points() << LatLng(6.0, 6.0) << LatLng(6.0, 8.0)
                                                     << LatLng(-6.0, 8.0)
                                                     << LatLng(-6.0, 6.0)));
    ASSERT_GREATER_THAN(bigPolyCompOffset.GetArea(), 2 * M_PI);
    ASSERT_TRUE(bigPolyCompOffset.Contains(shellPoly));
    ASSERT_TRUE(bigPolyCompOffset.Intersects(shellPoly));
}

TEST(BigSimplePolygon, BasicWinding) {
    // A 20x20 square centered at [0,0] (CCW)
    BigSimplePolygon bigPoly20(loop(points() << LatLng(10.0, 10.0) << LatLng(10.0, -10.0)
                                             << LatLng(-10.0, -10.0)
                                             << LatLng(-10.0, 10.0)));

    // Everything *not* in a 20x20 square centered at [0,0] (CW)
    BigSimplePolygon bigPoly20Comp(loop(points() << LatLng(10.0, 10.0) << LatLng(-10.0, 10.0)
                                                 << LatLng(-10.0, -10.0)
                                                 << LatLng(10.0, -10.0)));

    ASSERT_LESS_THAN(bigPoly20.GetArea(), 2 * M_PI);
    ASSERT_GREATER_THAN(bigPoly20Comp.GetArea(), 2 * M_PI);
}

TEST(BigSimplePolygon, LineRelations) {
    // A 20x20 square centered at [0,0]
    BigSimplePolygon bigPoly20(loop(points() << LatLng(10.0, 10.0) << LatLng(10.0, -10.0)
                                             << LatLng(-10.0, -10.0)
                                             << LatLng(-10.0, 10.0)));

    // A 10x10 line circling [0,0]
    S2Polyline line10(pointVec(points() << LatLng(5.0, 5.0) << LatLng(5.0, -5.0)
                                        << LatLng(-5.0, -5.0)
                                        << LatLng(-5.0, 5.0)));

    ASSERT_LESS_THAN(bigPoly20.GetArea(), 2 * M_PI);
    ASSERT(bigPoly20.Contains(line10));
    ASSERT(bigPoly20.Intersects(line10));

    // Line segment disjoint from big polygon
    S2Polyline lineDisjoint(pointVec(points() << LatLng(15.0, 5.0) << LatLng(15.0, -5.0)));
    ASSERT_FALSE(bigPoly20.Contains(lineDisjoint));
    ASSERT_FALSE(bigPoly20.Intersects(lineDisjoint));

    // Line segment intersects big polygon
    S2Polyline lineIntersect(pointVec(points() << LatLng(0.0, 0.0) << LatLng(15.0, 0.0)));
    ASSERT_FALSE(bigPoly20.Contains(lineIntersect));
    ASSERT_TRUE(bigPoly20.Intersects(lineIntersect));
}

TEST(BigSimplePolygon, LineRelationsComplement) {
    // A 20x20 square centered at [0,0]
    BigSimplePolygon bigPoly20Comp(loop(points() << LatLng(10.0, 10.0) << LatLng(10.0, -10.0)
                                                 << LatLng(-10.0, -10.0)
                                                 << LatLng(-10.0, 10.0)));
    bigPoly20Comp.Invert();

    // A 10x10 line circling [0,0]
    S2Polyline line10(pointVec(points() << LatLng(5.0, 5.0) << LatLng(5.0, -5.0)
                                        << LatLng(-5.0, -5.0)
                                        << LatLng(-5.0, 5.0)));

    ASSERT_GREATER_THAN(bigPoly20Comp.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly20Comp.Contains(line10));
    ASSERT_FALSE(bigPoly20Comp.Intersects(line10));

    // Line segment (0, 0) -> (0, 15)
    S2Polyline lineIntersect(pointVec(points() << LatLng(0.0, 0.0) << LatLng(0.0, 15.0)));
    ASSERT_FALSE(bigPoly20Comp.Contains(lineIntersect));
    ASSERT_TRUE(bigPoly20Comp.Intersects(lineIntersect));

    // A 10x10 line circling [0,0]
    S2Polyline line30(pointVec(points() << LatLng(15.0, 15.0) << LatLng(15.0, -15.0)
                                        << LatLng(-15.0, -15.0)
                                        << LatLng(-15.0, 15.0)));
    ASSERT_TRUE(bigPoly20Comp.Contains(line30));
    ASSERT_TRUE(bigPoly20Comp.Intersects(line30));
}

TEST(BigSimplePolygon, LineRelationsWinding) {
    // Everything *not* in a 20x20 square centered at [0,0] (CW winding)
    BigSimplePolygon bigPoly20Comp(loop(points() << LatLng(10.0, 10.0) << LatLng(-10.0, 10.0)
                                                 << LatLng(-10.0, -10.0)
                                                 << LatLng(10.0, -10.0)));

    // A 10x10 line circling [0,0]
    S2Polyline line10(pointVec(points() << LatLng(5.0, 5.0) << LatLng(5.0, -5.0)
                                        << LatLng(-5.0, -5.0)
                                        << LatLng(-5.0, 5.0)));

    ASSERT_GREATER_THAN(bigPoly20Comp.GetArea(), 2 * M_PI);
    ASSERT_FALSE(bigPoly20Comp.Contains(line10));
    ASSERT_FALSE(bigPoly20Comp.Intersects(line10));
}

TEST(BigSimplePolygon, PolarContains) {
    // Square 10 degrees from the north pole [90,0]
    BigSimplePolygon bigNorthPoly(loop(points() << LatLng(80.0, 0.0) << LatLng(80.0, 90.0)
                                                << LatLng(80.0, 180.0)
                                                << LatLng(80.0, -90.0)));

    // Square 5 degrees from the north pole [90, 0]
    S2Polygon northPoly(loopVec(points() << LatLng(85.0, 0.0) << LatLng(85.0, 90.0)
                                         << LatLng(85.0, 180.0)
                                         << LatLng(85.0, -90.0)));

    ASSERT_LESS_THAN(bigNorthPoly.GetArea(), 2 * M_PI);
    ASSERT_LESS_THAN(northPoly.GetArea(), bigNorthPoly.GetArea());
    ASSERT(bigNorthPoly.Contains(northPoly));
    ASSERT(bigNorthPoly.Intersects(northPoly));
}

TEST(BigSimplePolygon, PolarContainsWithHoles) {
    // Square 10 degrees from the north pole [90,0]
    BigSimplePolygon bigNorthPoly(loop(points() << LatLng(80.0, 0.0) << LatLng(80.0, 90.0)
                                                << LatLng(80.0, 180.0)
                                                << LatLng(80.0, -90.0)));

    // Square 5 degrees from the north pole [90, 0] with a concentric hole 1 degree from the
    // north pole
    vector<S2Loop*> loops;
    loops.push_back(loop(points() << LatLng(85.0, 0.0) << LatLng(85.0, 90.0) << LatLng(85.0, 180.0)
                                  << LatLng(85.0, -90.0)));
    loops.push_back(loop(points() << LatLng(89.0, 0.0) << LatLng(89.0, 90.0) << LatLng(89.0, 180.0)
                                  << LatLng(89.0, -90.0)));
    S2Polygon northPolyHole(&loops);

    ASSERT_LESS_THAN(northPolyHole.GetArea(), bigNorthPoly.GetArea());
    ASSERT(bigNorthPoly.Contains(northPolyHole));
    ASSERT(bigNorthPoly.Intersects(northPolyHole));
}

TEST(BigSimplePolygon, PolarIntersectsWithHoles) {
    // Square 10 degrees from the north pole [90,0]
    BigSimplePolygon bigNorthPoly(loop(points() << LatLng(80.0, 0.0) << LatLng(80.0, 90.0)
                                                << LatLng(80.0, 180.0)
                                                << LatLng(80.0, -90.0)));

    // 5-degree square with 1-degree-wide concentric hole, centered on [80.0, 0.0]
    vector<S2Loop*> loops;
    loops.push_back(loop(points() << LatLng(85.0, 5.0) << LatLng(85.0, -5.0) << LatLng(75.0, -5.0)
                                  << LatLng(75.0, 5.0)));
    loops.push_back(loop(points() << LatLng(81.0, 1.0) << LatLng(81.0, -1.0) << LatLng(79.0, -1.0)
                                  << LatLng(79.0, 1.0)));
    S2Polygon northPolyHole(&loops);

    ASSERT_LESS_THAN(northPolyHole.GetArea(), bigNorthPoly.GetArea());
    ASSERT_FALSE(bigNorthPoly.Contains(northPolyHole));
    ASSERT(bigNorthPoly.Intersects(northPolyHole));
}

// Edge cases
//
// No promise in terms of points on border - they may be inside or outside the big polygon.
// But we need to ensure the result is consistent:
// 1. If a polygon/line is contained by a big polygon, they must intersect with each other.
// 2. Relation doesn't change as long as the touch point doesn't change, no matter the big
//    polygon is larger or less then a hemisphere.
// 3. Relations for big polygons less than a hemisphere are consistent with ordinary (simple)
//    polygon results.

template <typename TShape>
void checkConsistency(const BigSimplePolygon& bigPoly,
                      const BigSimplePolygon& expandedBigPoly,
                      const TShape& shape) {
    // Contain() => Intersects()
    if (bigPoly.Contains(shape))
        ASSERT(bigPoly.Intersects(shape));
    if (expandedBigPoly.Contains(shape))
        ASSERT(expandedBigPoly.Intersects(shape));
    // Relation doesn't change
    ASSERT_EQUALS(bigPoly.Contains(shape), expandedBigPoly.Contains(shape));
    ASSERT_EQUALS(bigPoly.Intersects(shape), expandedBigPoly.Intersects(shape));
}

// Polygon shares big polygon's edge (disjoint)
TEST(BigSimplePolygon, ShareEdgeDisjoint) {
    // Big polygon smaller than a hemisphere.
    BigSimplePolygon bigPoly(loop(points() << LatLng(80.0, 0.0) << LatLng(-80.0, 0.0)
                                           << LatLng(-80.0, 90.0)
                                           << LatLng(80.0, 90.0)));
    ASSERT_LESS_THAN(bigPoly.GetArea(), 2 * M_PI);

    // Vertex point and collinear point
    S2Point point = LatLng(80.0, 0.0).ToPoint();
    S2Point collinearPoint = LatLng(0.0, 0.0).ToPoint();

    // Polygon shares one edge
    S2Polygon poly(loopVec(points() << LatLng(80.0, 0.0) << LatLng(-80.0, 0.0)
                                    << LatLng(-80.0, -10.0)
                                    << LatLng(80.0, -10.0)));
    // Polygon shares a segment of one edge
    S2Polygon collinearPoly(loopVec(points() << LatLng(50.0, 0.0) << LatLng(-50.0, 0.0)
                                             << LatLng(-50.0, -10.0)
                                             << LatLng(50.0, -10.0)));

    // Line
    S2Polyline line(
        pointVec(points() << LatLng(80.0, 0.0) << LatLng(-80.0, 0.0) << LatLng(-80.0, -10.0)));
    // Line share a segment of one edge
    S2Polyline collinearLine(
        pointVec(points() << LatLng(50.0, 0.0) << LatLng(-50.0, 0.0) << LatLng(-50.0, -10.0)));

    // Big polygon larger than a hemisphere.
    BigSimplePolygon expandedBigPoly(loop(points() << LatLng(80.0, 0.0) << LatLng(-80.0, 0.0)
                                                   << LatLng(-80.0, 90.0)
                                                   << LatLng(-80.0, 180.0)
                                                   << LatLng(-80.0, -90.0)
                                                   << LatLng(80.0, -90.0)
                                                   << LatLng(80.0, 180.0)
                                                   << LatLng(80.0, 90.0)));
    ASSERT_GREATER_THAN(expandedBigPoly.GetArea(), 2 * M_PI);

    checkConsistency(bigPoly, expandedBigPoly, point);
    checkConsistency(bigPoly, expandedBigPoly, collinearPoint);
    checkConsistency(bigPoly, expandedBigPoly, poly);
    checkConsistency(bigPoly, expandedBigPoly, collinearPoly);
    checkConsistency(bigPoly, expandedBigPoly, line);
    checkConsistency(bigPoly, expandedBigPoly, collinearLine);

    // Check the complement of big polygon
    bigPoly.Invert();
    ASSERT_GREATER_THAN(bigPoly.GetArea(), 2 * M_PI);
    expandedBigPoly.Invert();
    ASSERT_LESS_THAN(expandedBigPoly.GetArea(), 2 * M_PI);

    checkConsistency(bigPoly, expandedBigPoly, point);
    checkConsistency(bigPoly, expandedBigPoly, collinearPoint);
    checkConsistency(bigPoly, expandedBigPoly, poly);
    checkConsistency(bigPoly, expandedBigPoly, collinearPoly);
    checkConsistency(bigPoly, expandedBigPoly, line);
    checkConsistency(bigPoly, expandedBigPoly, collinearLine);
}

// Polygon/line shares big polygon's edge (contained by big polygon)
TEST(BigSimplePolygon, ShareEdgeContained) {
    // Big polygon smaller than a hemisphere.
    BigSimplePolygon bigPoly(loop(points() << LatLng(80.0, 0.0) << LatLng(-80.0, 0.0)
                                           << LatLng(-80.0, 90.0)
                                           << LatLng(80.0, 90.0)));
    ASSERT_LESS_THAN(bigPoly.GetArea(), 2 * M_PI);

    // Polygon
    S2Polygon poly(loopVec(points() << LatLng(80.0, 0.0) << LatLng(-80.0, 0.0)
                                    << LatLng(-80.0, 10.0)
                                    << LatLng(80.0, 10.0)));
    // Polygon shares a segment of one edge
    S2Polygon collinearPoly(loopVec(points() << LatLng(50.0, 0.0) << LatLng(-50.0, 0.0)
                                             << LatLng(-50.0, 10.0)
                                             << LatLng(50.0, 10.0)));
    // Line
    S2Polyline line(
        pointVec(points() << LatLng(80.0, 0.0) << LatLng(-80.0, 0.0) << LatLng(0.0, 10.0)));
    // Line shares a segment of one edge
    S2Polyline collinearLine(
        pointVec(points() << LatLng(50.0, 0.0) << LatLng(-50.0, 0.0) << LatLng(-50.0, 10.0)));

    // Big polygon larger than a hemisphere.
    BigSimplePolygon expandedBigPoly(loop(points() << LatLng(80.0, 0.0) << LatLng(-80.0, 0.0)
                                                   << LatLng(-80.0, 90.0)
                                                   << LatLng(-80.0, 180.0)
                                                   << LatLng(-80.0, -90.0)
                                                   << LatLng(80.0, -90.0)
                                                   << LatLng(80.0, 180.0)
                                                   << LatLng(80.0, 90.0)));
    ASSERT_GREATER_THAN(expandedBigPoly.GetArea(), 2 * M_PI);

    checkConsistency(bigPoly, expandedBigPoly, poly);
    checkConsistency(bigPoly, expandedBigPoly, collinearPoly);
    checkConsistency(bigPoly, expandedBigPoly, line);
    checkConsistency(bigPoly, expandedBigPoly, collinearLine);

    // Check the complement of big polygon
    bigPoly.Invert();
    ASSERT_GREATER_THAN(bigPoly.GetArea(), 2 * M_PI);
    expandedBigPoly.Invert();
    ASSERT_LESS_THAN(expandedBigPoly.GetArea(), 2 * M_PI);

    checkConsistency(bigPoly, expandedBigPoly, poly);
    checkConsistency(bigPoly, expandedBigPoly, collinearPoly);
    checkConsistency(bigPoly, expandedBigPoly, line);
    checkConsistency(bigPoly, expandedBigPoly, collinearLine);
}
}
