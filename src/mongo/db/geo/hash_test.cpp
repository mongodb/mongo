/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * This file contains tests for mongo/db/geo/hash.cpp.
 */

#include <algorithm>  // For max()
#include <bitset>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

#include "mongo/db/geo/hash.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace {

using namespace mongo;

using std::cout;
using std::endl;
using std::string;
using std::stringstream;

TEST(GeoHash, MakeZeroHash) {
    unsigned x = 0, y = 0;
    GeoHash hash(x, y);
}

static string makeRandomBitString(int length) {
    stringstream ss;
    mongo::PseudoRandom random(31337);
    for (int i = 0; i < length; ++i) {
        if (random.nextInt32() & 1) {
            ss << "1";
        } else {
            ss << "0";
        }
    }
    return ss.str();
}

// splitBinStr("0000111100001111") -> "0000 1111 0000 1111"
string splitBinStr(string bin) {
    string split = "";
    for (unsigned i = 0; i < bin.length(); i += 4) {
        split += bin.substr(i, 4) + ' ';
    }
    return split.substr(0, split.size() - 1);
}

TEST(GeoHash, MakeRandomValidHashes) {
    int maxStringLength = 64;
    for (int i = 0; i < maxStringLength; i += 2) {
        string a = makeRandomBitString(i);
        GeoHash hashA = GeoHash(a);
        (void)hashA.isBitSet(i, 0);
        (void)hashA.isBitSet(i, 1);
    }
}

// ASSERT_THROWS does not work if we try to put GeoHash(a) in the macro.
static GeoHash makeHash(const string& a) {
    return GeoHash(a);
}

TEST(GeoHash, MakeTooLongHash) {
    string a = makeRandomBitString(100);
    ASSERT_THROWS(makeHash(a), mongo::AssertionException);
}

TEST(GeoHash, MakeOddHash) {
    string a = makeRandomBitString(13);
    ASSERT_THROWS(makeHash(a), mongo::AssertionException);
}

TEST(GeoHash, HashAndUnhash) {
    PseudoRandom random(12345);
    for (int i = 0; i < 1'000; i++) {
        auto x = random.nextInt32();
        auto y = random.nextInt32();
        auto hash = GeoHash(x, y, 32);
        unsigned int unhashedX, unhashedY;
        hash.unhash(&unhashedX, &unhashedY);
        ASSERT_EQ(x, unhashedX);
        ASSERT_EQ(y, unhashedY);
    }
}

TEST(GeoHash, HashCropsBits) {
    // The following numbers were generated with this code snippet on Linux and hardcoded.
    // PseudoRandom random(12345);
    {
        auto x = -2067174821;
        auto y = 1127948890;
        auto bits = 1;
        auto hash = GeoHash(x, y, bits);
        ASSERT_EQ(hash.toString(), "10");
    }
    {
        auto x = -847616485;
        auto y = -2132331508;
        auto bits = 3;
        auto hash = GeoHash(x, y, bits);
        ASSERT_EQ(hash.toString(), "111000");
    }
    {
        auto x = -818733575;
        auto y = -721367113;
        auto bits = 6;
        auto hash = GeoHash(x, y, bits);
        ASSERT_EQ(hash.toString(), "111100011011");
    }
    {
        auto x = 1272197554;
        auto y = 1923758992;
        auto bits = 15;
        auto hash = GeoHash(x, y, bits);
        ASSERT_EQ(hash.toString(), "001101011000111011100110011001");
    }
    {
        auto x = -1516163863;
        auto y = -158391651;
        auto bits = 23;
        auto hash = GeoHash(x, y, bits);
        ASSERT_EQ(hash.toString(), "1101110100110110110010000101011100001100101001");
    }
    {
        auto x = -1665346465;
        auto y = 1063852771;
        auto bits = 30;
        auto hash = GeoHash(x, y, bits);
        ASSERT_EQ(hash.toString(), "100001111111010110011110111000011010001101100100011101101010");
    }
    {
        auto x = 327397251;
        auto y = 471329956;
        auto bits = 32;
        auto hash = GeoHash(x, y, bits);
        ASSERT_EQ(hash.toString(),
                  "0000001101011010100000010001111111011100111110101100010000011010");
    }
}

TEST(GeoHashConvertor, EdgeLength) {
    const double kError = 10E-15;
    GeoHashConverter::Parameters params{};
    params.max = 200.0;
    params.min = 100.0;
    params.bits = 32;
    double numBuckets = (1024 * 1024 * 1024 * 4.0);
    params.scaling = numBuckets / (params.max - params.min);

    auto converter = GeoHashConverter::createFromParams(params);
    ASSERT_OK(converter.getStatus());

    ASSERT_APPROX_EQUAL(100.0, converter.getValue()->sizeEdge(0), kError);
    ASSERT_APPROX_EQUAL(50.0, converter.getValue()->sizeEdge(1), kError);
    ASSERT_APPROX_EQUAL(25.0, converter.getValue()->sizeEdge(2), kError);
}

/**
 * ==========================
 * Error Bound of UnhashToBox
 * ==========================
 *
 * Compute the absolute error when unhashing a GeoHash to a box, so that expanding
 * the box by this absolute error can guarantee a point is always contained by the box
 * of its GeoHash. Thus, the absolute error of box should consist of 3 components:
 *
 * 1) The error introduced by hashing x to GeoHash. The extreme example would be a point
 * close to the boundary of a cell is hashed to an adjacent box.
 *
 * For a hash/unhash functions h(x)/uh(x) and computed functions h'(x),uh'(x):
 *
 *          x  uh(h'(x))
 * |--------|----|--------------------> min-max scale
 * min       \
 *            \
 *             \
 *              \
 * |--------|--|-|--------------------> hash scale for cells c
 * 0      h(x) c h'(x)
 *
 * 2) The error introduced by unhashing an (int) GeoHash to its lower left corner in x-y
 * space.
 *
 *            uh(c)
 *          x  |   uh'(c)
 * |--------|--|----|-----------------> min-max scale
 * min       \     /
 *            \   /
 *             \ /
 *              X
 * |--------|--|-|--------------------> hash scale for cells c
 * 0      h(x) c h'(x)
 *
 * 3) The error introduced by adding the edge length to get the top-right corner of box.
 * Instead of directly computing uh'(c+1), we add the computed box edge length to the computed
 * value uh(c), giving us an extra error.
 *
 *               |edge(min,max)|
 *               |             |
 *               |         uh(c)+edge
 *              uh(c)          |
 * |-------------|------[uh(c)+edge']-----------> min-max scale
 * min
 *
 * |-------------|-------------|----------------> hash scale
 * 0             c            c+1
 * Hash and unhash definitions
 * -------------------------
 * h(x) = (x - min) * scaling = 2^32 * (x - min) / (max - min)
 * uh(h) = h / scaling + min,
 * where
 * scaling = 2^32 / (max - min)
 *
 * Again, h(x)/uh(x) are the exact hash functions and h'(x)/uh'(x) are the computational hash
 * functions which have small rounding errors.
 *
 * | h'(x) - h(x) | == | delta_h(x; max, min) |
 * where delta_fn = the absolute difference between the computed and actual value of a
 * function.
 *
 * Restating the problem, we're looking for:
 * |delta_box| = | delta_x_{h'(x)=H} + delta_uh(h) + delta_edge_length |
 *            <= | delta_x_{h'(x)=H} | + | delta_uh(h) | + | delta_edge_length |
 *
 * 1. Error bounds calculation
 * ---------------------------
 *
 * 1.1 Error: | delta_x_{h'(x)=H} |
 * --------------------------------
 * The first error | delta_x_{h'(x)=H} | means, given GeoHash H, we can find
 * the range of x and only the range of x that may be mapped to H.
 * In other words, given H, for any x that is far enough from uh(H) by at least d,
 * it is impossible for x to be mapped to H.
 * Mathematical, find d, such that for any x satisfying |x - uh(H)| > d,
 *    |h(x) - H| >= | delta_h(x) |
 * => |h(x) - H| - | delta_h(x) | >= 0
 * => |h(x) - H + delta_h(x) | >= 0         (|a + b| >= |a| - |b|)
 * => |h'(x) - H| >= 0                      (h'(x) = h(x) + delta_h(x))
 * which guarantees h'(x) != H.
 *
 *
 *          uh(H)-d
 *              |
 *          x   |  uh(H)
 * |--------|---[----|----]-----------> min-max scale
 * min     / \   \       /
 *        /   \   \     /
 *       /     \   \   /
 *      /       \   \ /
 * |---[----|--|-]---|----------------> hash scale for cells c
 * 0      h(x) |     H
 *          h'(x)
 *         =h(x)+delta_h(x)
 *
 *
 * Let's consider one case of the above inequality. We need to find the d,
 * such that, when
 *     x < uh(H) - d,                                 (1)
 * we have
 *     h(x) + |delta_h(x)| <= H.                      (2)
 *
 * Due to the monotonicity of h(x), apply h(x) to both side of inequality (1),
 * we have
 *     h(x) < h(uh(H) - d) <= H - |delta_h(x)|     (from (2))
 *
 * By solving it, we have
 *     d = |delta_h(x)| / scaling
 *      <= 2Mu * (1 + |x-min|/|max-min|)     (see calculation for |delta_h(x)| below)
 *      <= 4Mu
 *
 * | delta_x_{h'(x)=H} | <= d <= 4Mu
 * The similar calculation applies for the other side of the above inequality.
 *
 * 1.2 Error of h(x)
 * -----------------
 *
 * Rules of error propagation
 * --------------------------
 * Absolute error of x is |delta_x|
 * Relative error of x is epsilon_x = |delta_x| / |x|
 * For any double number x, the relative error of x is bounded by "u". We assume all inputs
 * have this error to make deduction clear.
 * epsilon_x <= u = 0.5 * unit of least precision(ULP) ~= 1.1 * 10E-16
 *
 * |delta_(x + y)| <= |delta_x| + |delta_y|
 * |delta_(x - y)| <= |delta_x| + |delta_y|
 * epsilon_(x * y) <= epsilon_x + epsilon_y
 * epsilon_(x / y) <= epsilon_x + epsilon_y
 *
 * For a given min, max scale, the maximum delta in a computation is bounded by the maximum
 * value in the scale - M * u = max(|max|, |min|) * u.
 *
 * For the hash function h(x)
 * --------------------------
 *
 * epsilon_h(x) = epsilon_(x-min) + epsilon_scaling
 *
 * epsilon_(x-min) = (|delta_x| + |delta_min|) / |x - min|
 *                <= 2Mu / |x - min|
 *
 * epsilon_scaling = epsilon_(2^32) + epsilon_(max - min)
 *                 = 0 + epsilon_(max - min)
 *                <= 2Mu / |max - min|
 *
 * Hence, epsilon_h(x) <= 2Mu * (1/|x - min| + 1/|max - min|)
 *
 * |delta_h(x)| = 2Mu * (1 + |x-min|/|max-min|) * 2^32 / |max - min|
 *             <= 4Mu * 2^32 / |max-min|
 *
 * 2. Error: unhashing GeoHash to point
 * ------------------------------------
 * Similarly, we can calculate the error for uh(h) function, assuming h is exactly
 * represented in form of GeoHash, since integer is represented exactly.
 *
 * |delta_uh(h)| = epsilon_(h/scaling) * |h/scaling| + delta_min
 *               = epsilon_(scaling) * |h/scaling| + delta_min
 *              <= 2Mu / |max-min| * |max-min| + |min| * u
 *              <= 3Mu
 *
 * Thus, the second error |delta_uh(h)| <= 3Mu
 * Totally, the absolute error we need to add to unhashing to a point <=  4Mu + 3Mu = 7Mu
 *
 * 3. Error: edge length
 * ---------------------
 * The third part is easy to compute, since ldexp() doesn't introduce extra
 * relative error.
 *
 * edge_length = ldexp(max - min, -level)
 *
 * epsilon_edge = epsilon_(max - min) <= 2 * M * u / |max - min|
 *
 * | delta_edge | = epsilon_edge * (max - min) * 2^(-level)
 * = 2Mu * 2^(-level) <= Mu    (level >= 1)
 *
 * This error is neglectable when level >> 0.
 *
 * In conclusion, | delta_box | <= 8Mu
 *
 *
 * Test
 * ====
 * This first two component errors can be simulated by uh'(h'(x)).
 * Let h = h'(x)
 * |delta_(uh'(h'(x)))|
 * = epsilon_(h/scaling) * |h/scaling| + delta_min
 * = (epsilon_(h) + epsilon_(scaling)) * |h/scaling| + delta_min
 * = epsilon_(h) * h/scaling + epsilon_(scaling) * |h/scaling| + delta_min
 * = |delta_h|/scaling + |delta_uh(h)|
 * ~= |delta_box| when level = 32
 *
 * Another way to think about it is the error of uh'(h'(x)) also consists of
 * the same two components that constitute the error of unhashing to a point,
 * by substituting c with h'(x).
 *
 * | delta_(uh'(h'(x))) | = | x - uh'(h(x)) |
 *
 *            uh(h'(x))
 *              |
 *          x   | uh'(h(x))
 * |--------|---|---|----------------> min-max scale
 * min       \     /
 *            \   /
 *             \ /
 * |--------|---|--------------------> hash scale for cells c
 * 0      h(x)  h'(x)
 *
 *
 * We can get the maximum of the error by making max very large and min = -min, x -> max
 */
TEST(GeoHashConverter, UnhashToBoxError) {
    GeoHashConverter::Parameters params{};
    // Test max from 2^-20 to 2^20
    for (int times = -20; times <= 20; times += 2) {
        // Construct parameters
        params.max = ldexp(1 + 0.01 * times, times);
        params.min = -params.max;
        params.bits = 32;
        double numBuckets = (1024 * 1024 * 1024 * 4.0);
        params.scaling = numBuckets / (params.max - params.min);

        auto converter = GeoHashConverter::createFromParams(params);
        ASSERT_OK(converter.getStatus());

        // Assume level == 32, so we ignore the error of  edge length here.
        double delta_box = 7.0 / 8.0 * GeoHashConverter::calcUnhashToBoxError(params);
        double cellEdge = 1 / params.scaling;
        double x;

        // We are not able to test all the FP numbers to verify the error bound by design,
        // so we consider the numbers in the cell near the point we are interested in.
        //
        // FP numbers starting at max, working downward in minimal increments
        x = params.max;
        while (x > params.max - cellEdge) {
            x = nextafter(x, params.min);
            double x_prime = converter.getValue()->convertDoubleFromHashScale(
                converter.getValue()->convertToDoubleHashScale(x));
            double delta = fabs(x - x_prime);
            ASSERT_LESS_THAN(delta, delta_box);
        }

        // FP numbers starting between first and second cell, working downward to min
        x = params.min + cellEdge;
        while (x > params.min) {
            x = nextafter(x, params.min);
            double x_prime = converter.getValue()->convertDoubleFromHashScale(
                converter.getValue()->convertToDoubleHashScale(x));
            double delta = fabs(x - x_prime);
            ASSERT_LESS_THAN(delta, delta_box);
        }
    }
}

// SERVER-15576 Verify a point is contained by its GeoHash box.
TEST(GeoHashConverter, GeoHashBox) {
    GeoHashConverter::Parameters params{};
    params.max = 100000000.3;
    params.min = -params.max;
    params.bits = 32;
    double numBuckets = (1024 * 1024 * 1024 * 4.0);
    params.scaling = numBuckets / (params.max - params.min);

    auto converter = GeoHashConverter::createFromParams(params);
    ASSERT_OK(converter.getStatus());

    // Without expanding the box, the following point is not contained by its GeoHash box.
    mongo::Point p(-7201198.6497758823, -0.1);
    mongo::GeoHash hash = converter.getValue()->hash(p);
    mongo::Box box = converter.getValue()->unhashToBoxCovering(hash);
    ASSERT(box.inside(p));
}

TEST(GeoHash, NeighborsBasic) {
    vector<GeoHash> neighbors;

    // Top level
    GeoHash hashAtLevel3("100001");
    hashAtLevel3.appendVertexNeighbors(0u, &neighbors);
    ASSERT_EQUALS(neighbors.size(), (size_t)1);
    ASSERT_EQUALS(neighbors.front(), GeoHash(""));

    // Level 1
    neighbors.clear();
    hashAtLevel3.appendVertexNeighbors(1u, &neighbors);
    ASSERT_EQUALS(neighbors.size(), (size_t)2);
    std::sort(neighbors.begin(), neighbors.end());
    ASSERT_EQUALS(neighbors[0], GeoHash("00"));
    ASSERT_EQUALS(neighbors[1], GeoHash("10"));

    // Level 2
    neighbors.clear();
    hashAtLevel3.appendVertexNeighbors(2u, &neighbors);
    ASSERT_EQUALS(neighbors.size(), (size_t)4);
    std::sort(neighbors.begin(), neighbors.end());
    ASSERT_EQUALS(neighbors[0], GeoHash("0010"));
    ASSERT_EQUALS(neighbors[1], GeoHash("0011"));
    ASSERT_EQUALS(neighbors[2], GeoHash("1000"));
    ASSERT_EQUALS(neighbors[3], GeoHash("1001"));
}

TEST(GeoHash, NeighborsAtFinestLevel) {
    std::vector<GeoHash> neighbors;

    std::string zeroBase = "00000000000000000000000000000000000000000000000000000000";
    // At finest level
    GeoHash cellHash(zeroBase + "00011110");
    neighbors.clear();
    cellHash.appendVertexNeighbors(31u, &neighbors);
    ASSERT_EQUALS(neighbors.size(), (size_t)4);
    std::sort(neighbors.begin(), neighbors.end());
    ASSERT_EQUALS(neighbors[0], GeoHash(zeroBase + "000110"));
    ASSERT_EQUALS(neighbors[1], GeoHash(zeroBase + "000111"));
    ASSERT_EQUALS(neighbors[2], GeoHash(zeroBase + "001100"));
    ASSERT_EQUALS(neighbors[3], GeoHash(zeroBase + "001101"));

    // Level 30
    neighbors.clear();
    cellHash.appendVertexNeighbors(30u, &neighbors);
    ASSERT_EQUALS(neighbors.size(), (size_t)4);
    std::sort(neighbors.begin(), neighbors.end());
    ASSERT_EQUALS(neighbors[0], GeoHash(zeroBase + "0001"));
    ASSERT_EQUALS(neighbors[1], GeoHash(zeroBase + "0011"));
    ASSERT_EQUALS(neighbors[2], GeoHash(zeroBase + "0100"));
    ASSERT_EQUALS(neighbors[3], GeoHash(zeroBase + "0110"));

    // Level 29, only two neighbors including the parent.
    // ^
    // |
    // +-+
    // +-+
    // +-+-------> x
    neighbors.clear();
    cellHash.appendVertexNeighbors(29u, &neighbors);
    ASSERT_EQUALS(neighbors.size(), (size_t)2);
    std::sort(neighbors.begin(), neighbors.end());
    ASSERT_EQUALS(neighbors[0], GeoHash(zeroBase + "00"));
    ASSERT_EQUALS(neighbors[1], GeoHash(zeroBase + "01"));

    // Level 28, only one neighbor (the parent) at the left bottom corner.
    // ^
    // |
    // +---+
    // |   |
    // +---+-----> x
    neighbors.clear();
    cellHash.appendVertexNeighbors(28u, &neighbors);
    ASSERT_EQUALS(neighbors.size(), (size_t)1);
    ASSERT_EQUALS(neighbors[0], GeoHash(zeroBase));

    // Level 1
    neighbors.clear();
    cellHash.appendVertexNeighbors(1u, &neighbors);
    ASSERT_EQUALS(neighbors.size(), (size_t)1);
    ASSERT_EQUALS(neighbors[0], GeoHash("00"));
}

TEST(GeoHash, ClearUnusedBitsClearsSomeBits) {
    GeoHash geoHash("10110010");
    // 'parent' should have the four higher order bits from the original hash (1011, or the
    // hexidecimal digit 'b').
    GeoHash parent = geoHash.parent(2);
    ASSERT_EQUALS(parent, GeoHash("1011"));
    const long long expectedHash = 0xb000000000000000LL;
    ASSERT_EQUALS(expectedHash, parent.getHash());
}

TEST(GeoHash, ClearUnusedBitsOnLengthZeroHashClearsAllBits) {
    GeoHash geoHash("11");
    const long long expectedHash = 0xc000000000000000LL;
    ASSERT_EQUALS(expectedHash, geoHash.getHash());
    GeoHash parent = geoHash.parent();
    ASSERT_EQUALS(GeoHash(), parent);
    ASSERT_EQUALS(0LL, parent.getHash());
}

TEST(GeoHash, ClearUnusedBitsIsNoopIfNoBitsAreUnused) {
    // 64 pairs of "10" repeated.
    str::stream ss;
    for (int i = 0; i < 32; ++i) {
        ss << "10";
    }
    GeoHash geoHash(ss);
    GeoHash other = geoHash.parent(32);
    ASSERT_EQUALS(geoHash, other);
}
}  // namespace
