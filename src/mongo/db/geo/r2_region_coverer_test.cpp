/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kGeo

#include <chrono>
#include <random>

#include "mongo/db/geo/r2_region_coverer.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace {

using std::auto_ptr;
using namespace mongo;
using mongo::Polygon;  // "windows.h" has another Polygon for Windows GDI.

std::default_random_engine generator;

MONGO_INITIALIZER(R2CellUnion_Test)(InitializerContext* context) {
    unsigned seed = stdx::chrono::system_clock::now().time_since_epoch().count();
    for (size_t i = 2; i < context->args().size(); ++i) {
        if (context->args()[i - 1] == "--seed") {
            seed = std::stoul(context->args()[i]);
            break;
        }
    }
    generator.seed(seed);
    log() << "R2CellUnion Test - Random Number Generator Seed: " << seed;
    return Status::OK();
}

// Returns an integral number in [lower, upper]
template <typename NumType>
NumType random(NumType lower, NumType upper) {
    std::uniform_int_distribution<NumType> distribution(lower, upper);
    return distribution(generator);
}

//
// GeoHash
//
TEST(R2RegionCoverer, GeoHashSubdivide) {
    GeoHash children[4];

    // Full plane -> 4 quadrants
    GeoHash fullPlane;
    ASSERT_TRUE(fullPlane.subdivide(children));
    ASSERT_EQUALS(children[0], GeoHash(0LL, 1u));        // (x, y) : (0, 0)
    ASSERT_EQUALS(children[1], GeoHash(1LL << 62, 1u));  // (x, y) : (0, 1)
    ASSERT_EQUALS(children[2], GeoHash(2LL << 62, 1u));  // (x, y) : (1, 0)
    ASSERT_EQUALS(children[3], GeoHash(3LL << 62, 1u));  // (x, y) : (1, 1)

    // Small cell: 0...11XX -> 0...11[0-3]
    const long long cellHash = 3LL << 2;
    GeoHash cell(cellHash, 31u);
    ASSERT_TRUE(cell.subdivide(children));
    ASSERT_EQUALS(children[0], GeoHash(cellHash, 32u));      // (x, y) : (0, 0)
    ASSERT_EQUALS(children[1], GeoHash(cellHash + 1, 32u));  // (x, y) : (0, 1)
    ASSERT_EQUALS(children[2], GeoHash(cellHash + 2, 32u));  // (x, y) : (1, 0)
    ASSERT_EQUALS(children[3], GeoHash(cellHash + 3, 32u));  // (x, y) : (1, 1)

    // Smallest cell at finest level cannot subdivide
    GeoHash leafCell(1LL, 32u);
    ASSERT_FALSE(leafCell.subdivide(children));
}

TEST(R2RegionCoverer, GeoHashUnusedBits) {
    GeoHash geoHash(5566154225580586776LL, 0u);
    GeoHash entirePlane;
    ASSERT_EQUALS(geoHash, entirePlane);
}

TEST(R2RegionCoverer, GeoHashContains) {
    GeoHash entirePlane;
    GeoHash geoHash(5566154225580586776LL, 32u);  // An arbitrary random cell
    // GeoHash contains itself
    ASSERT_TRUE(entirePlane.contains(entirePlane));
    ASSERT_TRUE(geoHash.contains(geoHash));
    // Entire plane contains everything
    ASSERT_TRUE(entirePlane.contains(geoHash));
    ASSERT_FALSE(geoHash.contains(entirePlane));

    // Positive cases
    GeoHash parent("0010");
    GeoHash child("00100101");
    ASSERT_TRUE(parent.contains(parent));
    ASSERT_TRUE(parent.contains(child));
    ASSERT_TRUE(entirePlane.contains(geoHash));

    // Negative cases
    GeoHash other("01");
    ASSERT_FALSE(parent.contains(other));
    ASSERT_FALSE(other.contains(parent));
}


//
// R2RegionCoverer
//

// Plane boundary, x: [0.0, 100.0], y: [0.0, 100.0]
const double MAXBOUND = 100.0;

GeoHashConverter::Parameters getConverterParams() {
    GeoHashConverter::Parameters params;
    params.bits = 32;
    params.min = 0.0;
    params.max = MAXBOUND;
    const double numBuckets = (1024 * 1024 * 1024 * 4.0);
    params.scaling = numBuckets / (params.max - params.min);
    return params;
}

/**
 * Test region which mimics the region of a geohash cell.
 * NOTE: Technically this is not 100% correct, since geohash cells are inclusive on lower and
 * exclusive on upper edges.  For now, this region is just exclusive on all edges.
 * TODO: Create an explicit HashCell which correctly encapsulates this behavior, push to the
 * R2Region interface.
 */
class HashBoxRegion : public R2Region {
public:
    HashBoxRegion(Box box) : _box(box) {}
    Box getR2Bounds() const {
        return _box;
    }

    bool fastContains(const Box& other) const {
        return _box.contains(other);
    }

    bool fastDisjoint(const Box& other) const {
        if (!_box.intersects(other))
            return true;

        // Make outer edges exclusive
        if (_box._max.x == other._min.x || _box._min.x == other._max.x ||
            _box._max.y == other._min.y || _box._min.y == other._max.y)
            return true;

        return false;
    }

private:
    Box _box;
};

TEST(R2RegionCoverer, RandomCells) {
    GeoHashConverter converter(getConverterParams());
    R2RegionCoverer coverer(&converter);
    coverer.setMaxCells(1);
    // Test random cell ids at all levels.
    for (int i = 0; i < 10000; ++i) {
        GeoHash id(
            random(std::numeric_limits<long long>::lowest(), std::numeric_limits<long long>::max()),
            random(0U, GeoHash::kMaxBits));
        vector<GeoHash> covering;
        Box box = converter.unhashToBoxCovering(id);
        // Since the unhashed box is expanded by the error 8Mu, we need to shrink it.
        box.fudge(-GeoHashConverter::kMachinePrecision * MAXBOUND * 20);
        HashBoxRegion region(box);
        coverer.getCovering(region, &covering);
        ASSERT_EQUALS(covering.size(), (size_t)1);
        ASSERT_EQUALS(covering[0], id);
    }
}

double randDouble(double lowerBound, double upperBound) {
    verify(lowerBound <= upperBound);
    const int NUMBITS = 53;
    // Random double in [0, 1)
    long long randLong =
        random(std::numeric_limits<long long>::lowest(), std::numeric_limits<long long>::max());
    double r = ldexp(static_cast<double>(randLong & ((1ULL << NUMBITS) - 1ULL)), -NUMBITS);
    return lowerBound + r * (upperBound - lowerBound);
}

// Check the given region is covered by the covering completely.
// cellId is used internally.
void checkCellIdCovering(const GeoHashConverter& converter,
                         const R2Region& region,
                         const R2CellUnion& covering,
                         const GeoHash cellId = GeoHash()) {
    Box cell = converter.unhashToBoxCovering(cellId);

    // The covering may or may not contain this disjoint cell, we don't care.
    if (region.fastDisjoint(cell))
        return;

    // If the covering contains this id, that's fine.
    if (covering.contains(cellId))
        return;

    // The covering doesn't contain this cell, so the region shouldn't contain this cell.
    if (region.fastContains(cell)) {
        log() << "covering " << covering.toString();
        log() << "cellId " << cellId;
    }
    ASSERT_FALSE(region.fastContains(cell));

    // The region intersects with this cell. So the covering should intersect with it too.
    // We need to go deeper until a leaf. When we reach a leaf, it must be caught above
    //   - disjoint with the region, we don't care.
    //   - intersected with the region, contained in the covering.
    // We can guarantee the disjoint/intersection test is exact, since it's a circle.
    GeoHash children[4];
    ASSERT_TRUE(cellId.subdivide(children));  // Not a leaf
    for (int i = 0; i < 4; i++) {
        checkCellIdCovering(converter, region, covering, children[i]);
    }
}

void checkCovering(const GeoHashConverter& converter,
                   const R2Region& region,
                   const R2RegionCoverer& coverer,
                   const vector<GeoHash> covering) {
    // Keep track of how many cells have the same coverer.minLevel() ancestor.
    map<GeoHash, int> minLevelCells;
    // Check covering's minLevel and maxLevel.
    for (size_t i = 0; i < covering.size(); ++i) {
        unsigned int level = covering[i].getBits();
        ASSERT_NOT_LESS_THAN(level, coverer.minLevel());
        ASSERT_NOT_GREATER_THAN(level, coverer.maxLevel());
        minLevelCells[covering[i].parent(coverer.minLevel())] += 1;
    }
    if (covering.size() > (unsigned int)coverer.maxCells()) {
        // If the covering has more than the requested number of cells, then check
        // that the cell count cannot be reduced by using the parent of some cell.
        for (map<GeoHash, int>::const_iterator i = minLevelCells.begin(); i != minLevelCells.end();
             ++i) {
            ASSERT_EQUALS(i->second, 1);
        }
    }

    R2CellUnion cellUnion;
    cellUnion.init(covering);
    checkCellIdCovering(converter, region, cellUnion);
}

// Generate a circle within [0, MAXBOUND]
GeometryContainer* getRandomCircle(double radius) {
    ASSERT_LESS_THAN(radius, MAXBOUND / 2);

    // Format: { $center : [ [-74, 40.74], 10 ] }
    GeometryContainer* container = new GeometryContainer();
    container->parseFromQuery(
        BSON("$center" << BSON_ARRAY(BSON_ARRAY(randDouble(radius, MAXBOUND - radius)
                                                << randDouble(radius, MAXBOUND - radius))
                                     << radius))
            .firstElement());
    return container;
}

// Test the covering for arbitrary random circle.
TEST(R2RegionCoverer, RandomCircles) {
    GeoHashConverter converter(getConverterParams());
    R2RegionCoverer coverer(&converter);
    coverer.setMaxCells(8);

    for (int i = 0; i < 1000; i++) {
        // Using R2BoxRegion, the disjoint with circle gives poor results around the corner,
        // so many small cells are considered as intersected in the priority queue, which is
        // very slow for larger minLevel (smaller cell). So we limit minLevels in [0, 6].
        coverer.setMinLevel(random(0, 6));
        coverer.setMaxLevel(coverer.minLevel() + 4);

        double radius = randDouble(0.0, MAXBOUND / 2);
        auto_ptr<GeometryContainer> geometry(getRandomCircle(radius));
        const R2Region& region = geometry->getR2Region();

        vector<GeoHash> covering;
        coverer.getCovering(region, &covering);
        checkCovering(converter, region, coverer, covering);
    }
}

// Test the covering for very small circles, since the above test doesn't cover finest cells.
TEST(R2RegionCoverer, RandomTinyCircles) {
    GeoHashConverter converter(getConverterParams());
    R2RegionCoverer coverer(&converter);
    coverer.setMaxCells(random(1, 20));  // [1, 20]

    for (int i = 0; i < 10000; i++) {
        do {
            coverer.setMinLevel(random(0U, GeoHash::kMaxBits));
            coverer.setMaxLevel(random(0U, GeoHash::kMaxBits));
        } while (coverer.minLevel() > coverer.maxLevel());

        // 100 * 2 ^ -32 ~= 2.3E-8 (cell edge length)
        double radius = randDouble(1E-15, ldexp(100.0, -32) * 10);
        auto_ptr<GeometryContainer> geometry(getRandomCircle(radius));
        const R2Region& region = geometry->getR2Region();

        vector<GeoHash> covering;
        coverer.getCovering(region, &covering);
        checkCovering(converter, region, coverer, covering);
    }
}

//
// Shape Intersection
//
TEST(ShapeIntersection, Lines) {
    /*
     *    E     |D
     *  A___B   |C   G
     *    F
     */
    Point a(0, 0), b(1, 0), c(2, 0), d(2, 1);
    Point e(0.5, 1), f(0.5, -0.5), g(3, 0);

    /*
     * Basic disjoint
     *   / |
     *  /  |
     */
    ASSERT_FALSE(linesIntersect(a, d, c, b));
    ASSERT_FALSE(linesIntersect(c, b, a, d));  // commutative

    /*
     * Basic disjoint (axis aligned)
     *     |
     * ___ |
     */
    ASSERT_FALSE(linesIntersect(a, b, c, d));
    ASSERT_FALSE(linesIntersect(c, d, a, b));  // commutative

    /*
     * Basic intersection
     * \/
     * /\
     */
    ASSERT_TRUE(linesIntersect(e, c, f, d));
    ASSERT_TRUE(linesIntersect(f, d, e, c));  // commutative

    /*
     * Basic intersection (axis aligned)
     *  _|_
     *   |
     */
    ASSERT_TRUE(linesIntersect(a, b, e, f));
    ASSERT_TRUE(linesIntersect(f, e, b, a));  // commutative

    /*
     * One vertex on the line
     *        \
     *  ____   \
     */
    ASSERT_FALSE(linesIntersect(a, b, e, c));
    ASSERT_FALSE(linesIntersect(e, c, a, b));  // commutative

    /*
     * One vertex on the segment
     *    \
     *  ___\___
     */
    ASSERT_TRUE(linesIntersect(a, c, b, e));
    ASSERT_TRUE(linesIntersect(e, b, a, c));  // commutative

    /*
     * Two segments share one vertex
     *    /
     *   /____
     */
    ASSERT_TRUE(linesIntersect(a, c, a, e));
    ASSERT_TRUE(linesIntersect(a, e, a, c));  // commutative

    /*
     * Intersected segments on the same line
     * A___B===C---G
     */
    ASSERT_TRUE(linesIntersect(a, c, b, g));
    ASSERT_TRUE(linesIntersect(b, g, c, a));  // commutative

    /*
     * Disjoint segments on the same line
     * A___B   C---G
     */
    ASSERT_FALSE(linesIntersect(a, b, c, g));
    ASSERT_FALSE(linesIntersect(c, g, a, b));  // commutative

    /*
     * Segments on the same line share one vertex.
     *        /D
     *       /B
     *     F/
     */
    ASSERT_TRUE(linesIntersect(d, b, b, f));
    ASSERT_TRUE(linesIntersect(f, b, d, b));  // commutative
    // axis aligned
    ASSERT_TRUE(linesIntersect(a, c, g, c));
    ASSERT_TRUE(linesIntersect(c, g, a, c));  // commutative
}

TEST(ShapeIntersection, Polygons) {
    // Convex polygon (triangle)

    /*
     * Disjoint, bounds disjoint
     *        /|
     *       / |  []
     *      /__|
     */
    vector<Point> triangleVetices;
    triangleVetices.push_back(Point(0, 0));
    triangleVetices.push_back(Point(1, 0));
    triangleVetices.push_back(Point(1, 4));
    Polygon triangle(triangleVetices);
    Box box;

    box = Box(1.5, 1.5, 1);
    ASSERT_FALSE(edgesIntersectsWithBox(triangle.points(), box));
    ASSERT_FALSE(polygonIntersectsWithBox(triangle, box));
    ASSERT_FALSE(polygonContainsBox(triangle, box));

    /*
     * Disjoint, bounds intersect
     *     [] /|
     *       / |
     *      /__|
     */
    box = Box(-0.5, 3.5, 1);
    ASSERT_FALSE(edgesIntersectsWithBox(triangle.points(), box));
    ASSERT_FALSE(polygonIntersectsWithBox(triangle, box));
    ASSERT_FALSE(polygonContainsBox(triangle, box));

    /*
     * Intersect on one polygon vertex
     *      _____
     *     |     |
     *     |_ /|_|
     *       / |
     *      /__|
     */
    box = Box(0, 3, 2);
    ASSERT_TRUE(edgesIntersectsWithBox(triangle.points(), box));
    ASSERT_TRUE(polygonIntersectsWithBox(triangle, box));
    ASSERT_FALSE(polygonContainsBox(triangle, box));

    /*
     * Box contains polygon
     *   __________
     *  |          |
     *  |     /|   |
     *  |    / |   |
     *  |   /__|   |
     *  |__________|
     */
    box = Box(-1, -1, 6);
    ASSERT_FALSE(edgesIntersectsWithBox(triangle.points(), box));
    ASSERT_TRUE(polygonIntersectsWithBox(triangle, box));
    ASSERT_FALSE(polygonContainsBox(triangle, box));

    /*
     * Polygon contains box
     *        /|
     *       / |
     *      /  |
     *     / []|
     *    /____|
     */
    box = Box(0.1, 0.1, 0.2);
    ASSERT_FALSE(edgesIntersectsWithBox(triangle.points(), box));
    ASSERT_TRUE(polygonIntersectsWithBox(triangle, box));
    ASSERT_TRUE(polygonContainsBox(triangle, box));

    /*
     * Intersect, but no vertex is contained by the other shape.
     *    ___ /|_
     *   |   / | |
     *   |  /  | |
     *   |_/___|_|
     *    /____|
     */
    box = Box(0, 1, 2);
    ASSERT_TRUE(edgesIntersectsWithBox(triangle.points(), box));
    ASSERT_TRUE(polygonIntersectsWithBox(triangle, box));
    ASSERT_FALSE(polygonContainsBox(triangle, box));

    // Concave polygon

    /*
     * (0,4)
     * |\
     * | \(1,1)
     * |  `.
     * |____`. (4,0)
     * (0,0)
     */
    vector<Point> concaveVetices;
    concaveVetices.push_back(Point(0, 0));
    concaveVetices.push_back(Point(4, 0));
    concaveVetices.push_back(Point(1, 1));
    concaveVetices.push_back(Point(0, 4));
    Polygon concave(concaveVetices);

    /*
     * Disjoint
     * |\
     * | \
     * |  `.
     * |____`.
     *   []
     */
    box = Box(1, -1, 0.9);
    ASSERT_FALSE(edgesIntersectsWithBox(concave.points(), box));
    ASSERT_FALSE(polygonIntersectsWithBox(concave, box));
    ASSERT_FALSE(polygonContainsBox(concave, box));

    /*
     * Disjoint, bounds intersect
     * |\
     * | \[]
     * |  `.
     * |____`.
     */
    box = Box(1.1, 1.1, 0.2);
    ASSERT_FALSE(edgesIntersectsWithBox(concave.points(), box));
    ASSERT_FALSE(polygonIntersectsWithBox(concave, box));
    ASSERT_FALSE(polygonContainsBox(concave, box));

    /*
     * Intersect, one box vertex is contained by the polygon.
     *  |\
     *  |+\+ (1.5, 1.5)
     *  |+-`.
     *  |____`.
     */
    box = Box(0.5, 0.5, 1);
    ASSERT_TRUE(edgesIntersectsWithBox(concave.points(), box));
    ASSERT_TRUE(polygonIntersectsWithBox(concave, box));
    ASSERT_FALSE(polygonContainsBox(concave, box));

    /*
     * Intersect, no vertex is contained by the other shape.
     *  |\
     * +| \--+
     * ||  `.|
     * ||____`.
     * +-----+
     */
    box = Box(-0.5, -0.5, 3);
    ASSERT_TRUE(edgesIntersectsWithBox(concave.points(), box));
    ASSERT_TRUE(polygonIntersectsWithBox(concave, box));
    ASSERT_FALSE(polygonContainsBox(concave, box));
}

TEST(ShapeIntersection, Annulus) {
    R2Annulus annulus(Point(0.0, 0.0), 1, 5);
    Box box;

    // Disjoint, out of outer circle
    box = Box(4, 4, 1);
    ASSERT_TRUE(annulus.fastDisjoint(box));
    ASSERT_FALSE(annulus.fastContains(box));

    // Box contains outer circle
    box = Box(-6, -5.5, 12);
    ASSERT_FALSE(annulus.fastDisjoint(box));
    ASSERT_FALSE(annulus.fastContains(box));

    // Box intersects with the outer circle, but not the inner circle
    box = Box(3, 3, 4);
    ASSERT_FALSE(annulus.fastDisjoint(box));
    ASSERT_FALSE(annulus.fastContains(box));

    // Box is contained by the annulus
    box = Box(2, 2, 1);
    ASSERT_FALSE(annulus.fastDisjoint(box));
    ASSERT_TRUE(annulus.fastContains(box));

    // Box is contained by the outer circle and intersects with the inner circle
    box = Box(0.4, 0.5, 3);
    ASSERT_FALSE(annulus.fastDisjoint(box));
    ASSERT_FALSE(annulus.fastContains(box));

    // Box intersects with both outer and inner circle
    box = Box(-4, -4, 4.5);
    ASSERT_FALSE(annulus.fastDisjoint(box));
    ASSERT_FALSE(annulus.fastContains(box));

    // Box is inside the inner circle
    box = Box(-0.1, -0.2, 0.5);
    ASSERT_TRUE(annulus.fastDisjoint(box));
    ASSERT_FALSE(annulus.fastContains(box));

    // Box contains the inner circle, but intersects with the outer circle
    box = Box(-2, -2, 7);
    ASSERT_FALSE(annulus.fastDisjoint(box));
    ASSERT_FALSE(annulus.fastContains(box));

    //
    // Annulus contains both inner and outer circles as boundaries.
    //

    // Box only touches the outer boundary
    box = Box(3, 4, 1);  // Lower left touches boundary
    ASSERT_FALSE(annulus.fastDisjoint(box));
    ASSERT_FALSE(annulus.fastContains(box));
    box = Box(-4, -5, 1);  // Upper right touches boundary
    ASSERT_FALSE(annulus.fastDisjoint(box));
    ASSERT_FALSE(annulus.fastContains(box));

    // Box is contained by the annulus touching the outer boundary
    box = Box(-4, -3, 0.1);
    ASSERT_FALSE(annulus.fastDisjoint(box));
    ASSERT_TRUE(annulus.fastContains(box));

    // Box is contained by the annulus touching the inner boundary
    box = Box(0, 1, 1);
    ASSERT_FALSE(annulus.fastDisjoint(box));
    ASSERT_TRUE(annulus.fastContains(box));

    // Box only touches the inner boundary at (-0.6, 0.8)
    box = Box(-0.6, 0.3, 0.5);
    ASSERT_FALSE(annulus.fastDisjoint(box));
    ASSERT_FALSE(annulus.fastContains(box));
}

bool oneIn(unsigned num) {
    std::uniform_int_distribution<unsigned> distribution(1, num);
    return distribution(generator) == 1;
}

void generateRandomCells(GeoHash const& id,
                         bool selected,
                         std::vector<GeoHash>* unnormalized,
                         std::vector<GeoHash>* normalized) {
    // This function generates an unnormalized and a normalized GeoHash vector to create
    // a random R2CellUnion.
    // If selected is true, the region covered by GeoHash id will be covered by the cells
    // in unnormalized and normalized.

    // This is a leaf cell and cannot be subdivided further, so it must be added.
    if (id.getBits() == 32) {
        unnormalized->push_back(id);
        return;
    }

    // If the parent cell was not selected, this cell will be selected with a probability
    // proportional to its level, so smaller cells are more likely to be selected.
    if (!selected && oneIn(32 - id.getBits())) {
        normalized->push_back(id);
        selected = true;
    }

    // If this cell is selected, we can either add it or another set of cells that
    // cover the same region.
    bool added = false;
    if (selected && !oneIn(6)) {
        unnormalized->push_back(id);
        added = true;
    }

    // Add all the children of this cell if it was selected, but not added.
    // Randomly add other children cells.
    // Make sure not to include all 4 children if not selected to ensure that the
    // normalized union is correct.
    int numChildren = 0;
    GeoHash children[4];
    id.subdivide(children);
    for (int pos = 0; pos < 4; ++pos) {
        // If selected, recurse on 4/12 = 1/3 child to add overlapping cells to the
        // normalized vector.
        // If not selected, recurse on 4 * 2/7 = 8/7 child.
        if ((selected ? oneIn(12) : (random(0, 6) < 2)) && numChildren < 3) {
            generateRandomCells(children[pos], selected, unnormalized, normalized);
            ++numChildren;
        }
        if (selected && !added) {
            generateRandomCells(children[pos], selected, unnormalized, normalized);
        }
    }
}

TEST(R2CellUnion, Normalize) {
    int unnormalizedSum = 0, normalizedSum = 0;
    int kIters = 2000;
    for (int i = 0; i < kIters; ++i) {
        std::vector<GeoHash> input, expected;
        generateRandomCells(GeoHash(), false, &input, &expected);
        unnormalizedSum += input.size();
        normalizedSum += expected.size();
        // Initialize with unnormalized input
        R2CellUnion cellUnion;
        cellUnion.init(input);

        // Check to make sure the cells in cellUnion equal the expected cells
        ASSERT_EQUALS(expected.size(), cellUnion.cellIds().size());
        for (size_t i = 0; i < expected.size(); ++i) {
            ASSERT_EQUALS(expected[i], cellUnion.cellIds()[i]);
        }
    }
    log() << "Average Unnormalized Size: " << unnormalizedSum * 1.0 / kIters;
    log() << "Average Normalized Size: " << normalizedSum * 1.0 / kIters;
}

void testContains(const R2CellUnion& cellUnion, GeoHash id, int num) {
    // Breadth first check of the child cells to make sure that each one is contained
    // in cellUnion
    std::queue<GeoHash> ids;
    ids.push(id);
    int cellsChecked = 0;
    while (!ids.empty() && cellsChecked < num) {
        ++cellsChecked;
        GeoHash currentId = ids.front();
        ids.pop();
        ASSERT_TRUE(cellUnion.contains(currentId));
        ASSERT_TRUE(cellUnion.intersects(currentId));
        if (currentId.getBits() < 32) {
            GeoHash children[4];
            currentId.subdivide(children);
            for (int i = 0; i < 4; ++i) {
                ids.push(children[i]);
            }
        }
    }
}

TEST(R2CellUnion, Contains) {
    // An R2CellUnion should contain all of its children.
    std::vector<GeoHash> entirePlaneVector;
    GeoHash entirePlane;
    entirePlaneVector.push_back(entirePlane);
    R2CellUnion entirePlaneUnion;
    entirePlaneUnion.init(entirePlaneVector);
    ASSERT_TRUE(entirePlaneUnion.contains(entirePlane));
    GeoHash childCell1("00");
    ASSERT_TRUE(entirePlaneUnion.contains(childCell1));
    GeoHash childCell2("01");
    ASSERT_TRUE(entirePlaneUnion.contains(childCell2));
    GeoHash childCell3("10");
    ASSERT_TRUE(entirePlaneUnion.contains(childCell3));
    GeoHash childCell4("11");
    ASSERT_TRUE(entirePlaneUnion.contains(childCell4));

    // An R2CellUnion should contain every cell that is contained by one of its member cells
    for (int i = 0; i < 2000; ++i) {
        std::vector<GeoHash> unnormalized, normalized;
        generateRandomCells(GeoHash(), false, &unnormalized, &normalized);
        R2CellUnion cellUnion;
        cellUnion.init(normalized);
        for (auto cellId : normalized) {
            testContains(cellUnion, cellId, 100);
        }
    }
}

// Naive implementation of intersects to test correctness
bool intersects(const R2CellUnion& cellUnion, GeoHash cellId) {
    for (auto unionCellId : cellUnion.cellIds()) {
        // Two cells will only intersect if one contains the other
        if (unionCellId.contains(cellId) || cellId.contains(unionCellId)) {
            return true;
        }
    }
    return false;
}

TEST(R2CellUnion, Intersects) {
    // An R2CellUnion should intersect with every cell it contains.
    std::vector<GeoHash> entirePlaneVector;
    GeoHash entirePlane;
    entirePlaneVector.push_back(entirePlane);
    R2CellUnion entirePlaneUnion;
    entirePlaneUnion.init(entirePlaneVector);
    ASSERT_TRUE(entirePlaneUnion.intersects(entirePlane));
    GeoHash childCell1("00");
    ASSERT_TRUE(entirePlaneUnion.intersects(childCell1));
    GeoHash childCell2("01");
    ASSERT_TRUE(entirePlaneUnion.intersects(childCell2));
    GeoHash childCell3("10");
    ASSERT_TRUE(entirePlaneUnion.intersects(childCell3));
    GeoHash childCell4("11");
    ASSERT_TRUE(entirePlaneUnion.intersects(childCell4));

    for (int k = 0; k < 2000; ++k) {
        R2CellUnion randomUnion;
        std::vector<GeoHash> unnormalized, normalized;
        generateRandomCells(GeoHash(), false, &unnormalized, &normalized);
        randomUnion.init(normalized);

        // An R2CellUnion should intersect with every cell that contains a member of the union.
        // It should also intersect with cells it contains
        for (auto cellId : randomUnion.cellIds()) {
            for (unsigned level = 0; level <= 32; ++level) {
                ASSERT_TRUE(randomUnion.intersects(GeoHash(cellId.getHash(), level)));
            }
        }

        // Check that the output of intersects matches that of the naive implementation
        std::vector<GeoHash> otherUnnormalized, otherNormalized;
        generateRandomCells(GeoHash(), false, &otherUnnormalized, &otherNormalized);
        for (const GeoHash& cellId : otherUnnormalized) {
            ASSERT_EQUALS(randomUnion.intersects(cellId), intersects(randomUnion, cellId));
        }
    }
}

void testDifference(std::vector<GeoHash>& xCellIds, std::vector<GeoHash>& yCellIds) {
    // Initialize the two cell unions
    R2CellUnion x, y;
    x.init(xCellIds);
    y.init(yCellIds);

    // Compute the differences x - y and y - x
    R2CellUnion xMinusY, yMinusX;
    xMinusY.init(xCellIds);
    xMinusY.getDifference(y);
    yMinusX.init(yCellIds);
    yMinusX.getDifference(x);

    // Check that x contains x - y and y contains y - x
    // Check that y doesn't intersect x - y and x doesn't intersect y - x
    // Check that y - x doesn't intersect with x - y
    for (size_t i = 0; i < xMinusY.cellIds().size(); ++i) {
        const GeoHash& cellId = xMinusY.cellIds()[i];
        ASSERT_TRUE(x.contains(cellId));
        ASSERT_TRUE(x.intersects(cellId));
        ASSERT_FALSE(y.contains(cellId));
        ASSERT_FALSE(y.intersects(cellId));
        ASSERT_FALSE(yMinusX.contains(cellId));
        ASSERT_FALSE(yMinusX.intersects(cellId));
    }
    for (size_t i = 0; i < yMinusX.cellIds().size(); ++i) {
        const GeoHash& cellId = yMinusX.cellIds()[i];
        ASSERT_TRUE(y.contains(cellId));
        ASSERT_TRUE(y.intersects(cellId));
        ASSERT_FALSE(x.contains(cellId));
        ASSERT_FALSE(x.intersects(cellId));
        ASSERT_FALSE(xMinusY.contains(cellId));
        ASSERT_FALSE(xMinusY.intersects(cellId));
    }

    // Check that x - y + y contains x U y and y - x + x contains x U y
    // Check that x U y contains both x - y + y and y - x + x
    R2CellUnion xMinusYPlusY, yMinusXPlusX, xUnionY;
    xMinusYPlusY.init(xMinusY.cellIds());
    xMinusYPlusY.add(y.cellIds());
    yMinusXPlusX.init(yMinusX.cellIds());
    yMinusXPlusX.add(x.cellIds());
    xUnionY.init(x.cellIds());
    xUnionY.add(y.cellIds());

    for (auto cellId : xUnionY.cellIds()) {
        ASSERT_TRUE(xMinusYPlusY.contains(cellId));
        ASSERT_TRUE(yMinusXPlusX.contains(cellId));
    }
    for (auto cellId : xMinusYPlusY.cellIds()) {
        ASSERT_TRUE(xUnionY.contains(cellId));
    }
    for (auto cellId : yMinusXPlusX.cellIds()) {
        ASSERT_TRUE(xUnionY.contains(cellId));
    }
}

TEST(R2CellUnion, Difference) {
    for (int i = 0; i < 2000; ++i) {
        std::vector<GeoHash> xUnnormalized, xNormalized;
        generateRandomCells(GeoHash(), false, &xUnnormalized, &xNormalized);
        std::vector<GeoHash> yUnnormalized, yNormalized;
        generateRandomCells(GeoHash(), false, &yUnnormalized, &yNormalized);
        // Test with two unions that contain each other
        testDifference(xUnnormalized, xNormalized);
        // Test with random unions
        testDifference(xUnnormalized, yUnnormalized);
    }
}

TEST(R2CellUnion, Empty) {
    R2CellUnion emptyUnion;
    R2CellUnion randomUnion;
    std::vector<GeoHash> unnormalized, normalized;
    generateRandomCells(GeoHash(), false, &unnormalized, &normalized);
    randomUnion.init(normalized);

    // normalize()
    emptyUnion.init(std::vector<GeoHash>());
    ASSERT_TRUE(emptyUnion.cellIds().empty());

    // contains() and intersects()
    for (const GeoHash& cellId : unnormalized) {
        ASSERT_FALSE(emptyUnion.contains(cellId));
        ASSERT_FALSE(emptyUnion.intersects(cellId));
    }

    // getDifference()
    std::vector<GeoHash> originalCellIds;
    std::copy(randomUnion.cellIds().begin(),
              randomUnion.cellIds().end(),
              std::back_inserter(originalCellIds));
    randomUnion.getDifference(emptyUnion);
    ASSERT_TRUE(originalCellIds == randomUnion.cellIds());
    emptyUnion.getDifference(randomUnion);
    ASSERT_TRUE(emptyUnion.cellIds().empty());
}

TEST(R2CellUnion, Detach) {
    GeoHash entirePlaneCell;
    std::vector<GeoHash> cellIds;
    cellIds.push_back(entirePlaneCell);
    R2CellUnion entirePlaneUnion;
    entirePlaneUnion.init(cellIds);
    ASSERT_EQUALS(1UL, entirePlaneUnion.cellIds().size());
    ASSERT_EQUALS(entirePlaneCell, entirePlaneUnion.cellIds()[0]);

    std::vector<GeoHash> otherCellIds;
    otherCellIds.push_back(GeoHash("01"));
    entirePlaneUnion.detach(&otherCellIds);
    ASSERT_EQUALS(1UL, otherCellIds.size());
    ASSERT_EQUALS(entirePlaneCell, otherCellIds[0]);
    ASSERT_TRUE(entirePlaneUnion.cellIds().empty());
}
}  // namespace
