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


#include "mongo/db/geo/geometry_container.h"

#include <cstddef>

#include <s1angle.h>
#include <s2.h>
#include <s2cap.h>
#include <s2cell.h>
#include <s2cellid.h>
#include <s2latlng.h>
#include <s2latlngrect.h>
#include <s2polygon.h>
#include <s2polyline.h>
#include <s2region.h>
#include <s2regionunion.h>

#include <util/math/vector3-inl.h>
#include <util/math/vector3.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/geo/big_polygon.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/query/bson/multikey_dotted_path_support.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"

#include <set>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

bool GeometryContainer::isSimpleContainer() const {
    return nullptr != _point || nullptr != _line || nullptr != _polygon;
}

bool GeometryContainer::isPoint() const {
    return nullptr != _point;
}

PointWithCRS GeometryContainer::getPoint() const {
    tassert(9911939, "", isPoint());
    return *_point;
}

bool GeometryContainer::supportsContains() const {
    return nullptr != _polygon || nullptr != _box || nullptr != _cap || nullptr != _multiPolygon ||
        (nullptr != _geometryCollection &&
         (!_geometryCollection->polygons.empty() || !_geometryCollection->multiPolygons.empty()));
}

bool GeometryContainer::hasS2Region() const {
    return (nullptr != _point && _point->crs == SPHERE) || nullptr != _line ||
        (nullptr != _polygon && (_polygon->crs == SPHERE || _polygon->crs == STRICT_SPHERE)) ||
        (nullptr != _cap && _cap->crs == SPHERE) || nullptr != _multiPoint ||
        nullptr != _multiLine || nullptr != _multiPolygon || nullptr != _geometryCollection;
}

const S2Region& GeometryContainer::getS2Region() const {
    if (nullptr != _point && SPHERE == _point->crs) {
        return _point->cell;
    } else if (nullptr != _line) {
        return _line->line;
    } else if (nullptr != _polygon && nullptr != _polygon->s2Polygon) {
        return *_polygon->s2Polygon;
    } else if (nullptr != _polygon && nullptr != _polygon->bigPolygon) {
        return *_polygon->bigPolygon;
    } else if (nullptr != _cap && SPHERE == _cap->crs) {
        return _cap->cap;
    } else if (nullptr != _multiPoint) {
        return *_s2Region;
    } else if (nullptr != _multiLine) {
        return *_s2Region;
    } else if (nullptr != _multiPolygon) {
        return *_s2Region;
    } else {
        tassert(9911928, "", nullptr != _geometryCollection);
        return *_s2Region;
    }
}

bool GeometryContainer::hasR2Region() const {
    return _cap || _box || _point || (_polygon && _polygon->crs == FLAT) ||
        (_multiPoint && FLAT == _multiPoint->crs);
}

class GeometryContainer::R2BoxRegion : public R2Region {
public:
    R2BoxRegion(const GeometryContainer* geometry);
    ~R2BoxRegion() override;

    Box getR2Bounds() const override;

    bool fastContains(const Box& other) const override;

    bool fastDisjoint(const Box& other) const override;

private:
    static Box buildBounds(const GeometryContainer& geometry);

    // Not owned here
    const GeometryContainer* _geometry;

    // TODO: For big complex shapes, may be better to use actual shape from above
    const Box _bounds;
};

GeometryContainer::R2BoxRegion::R2BoxRegion(const GeometryContainer* geometry)
    : _geometry(geometry), _bounds(buildBounds(*geometry)) {}

GeometryContainer::R2BoxRegion::~R2BoxRegion() {}

Box GeometryContainer::R2BoxRegion::getR2Bounds() const {
    return _bounds;
}

bool GeometryContainer::R2BoxRegion::fastContains(const Box& other) const {
    // TODO: Add more cases here to make coverings better
    if (_geometry->_box && FLAT == _geometry->_box->crs) {
        const Box& box = _geometry->_box->box;
        if (box.contains(other))
            return true;
    } else if (_geometry->_cap && FLAT == _geometry->_cap->crs) {
        const Circle& circle = _geometry->_cap->circle;
        // Exact test
        return circleContainsBox(circle, other);
    }

    if (_geometry->_polygon && FLAT == _geometry->_polygon->crs) {
        const Polygon& polygon = _geometry->_polygon->oldPolygon;
        // Exact test
        return polygonContainsBox(polygon, other);
    }

    // Not sure
    return false;
}

bool GeometryContainer::R2BoxRegion::fastDisjoint(const Box& other) const {
    if (!_bounds.intersects(other))
        return true;

    // Not sure
    return false;
}

static Point toLngLatPoint(const S2Point& s2Point) {
    Point point;
    S2LatLng latLng(s2Point);
    point.x = latLng.lng().degrees();
    point.y = latLng.lat().degrees();
    return point;
}

static void lineR2Bounds(const S2Polyline& flatLine, Box* flatBounds) {
    int numVertices = flatLine.num_vertices();
    MONGO_verify(flatLine.num_vertices() > 0);

    flatBounds->init(toLngLatPoint(flatLine.vertex(0)), toLngLatPoint(flatLine.vertex(0)));

    for (int i = 1; i < numVertices; ++i) {
        flatBounds->expandToInclude(toLngLatPoint(flatLine.vertex(i)));
    }
}

static void circleR2Bounds(const Circle& circle, Box* flatBounds) {
    flatBounds->init(Point(circle.center.x - circle.radius, circle.center.y - circle.radius),
                     Point(circle.center.x + circle.radius, circle.center.y + circle.radius));
}

static void multiPointR2Bounds(const vector<S2Point>& points, Box* flatBounds) {
    MONGO_verify(!points.empty());

    flatBounds->init(toLngLatPoint(points.front()), toLngLatPoint(points.front()));

    vector<S2Point>::const_iterator it = points.begin();
    for (++it; it != points.end(); ++it) {
        const S2Point& s2Point = *it;
        flatBounds->expandToInclude(toLngLatPoint(s2Point));
    }
}

static void polygonR2Bounds(const Polygon& polygon, Box* flatBounds) {
    *flatBounds = polygon.bounds();
}

static void s2RegionR2Bounds(const S2Region& region, Box* flatBounds) {
    S2LatLngRect s2Bounds = region.GetRectBound();
    flatBounds->init(Point(s2Bounds.lng_lo().degrees(), s2Bounds.lat_lo().degrees()),
                     Point(s2Bounds.lng_hi().degrees(), s2Bounds.lat_hi().degrees()));
}

Box GeometryContainer::R2BoxRegion::buildBounds(const GeometryContainer& geometry) {
    Box bounds;

    if (geometry._point && FLAT == geometry._point->crs) {
        bounds.init(geometry._point->oldPoint, geometry._point->oldPoint);
    } else if (geometry._line && FLAT == geometry._line->crs) {
        lineR2Bounds(geometry._line->line, &bounds);
    } else if (geometry._cap && FLAT == geometry._cap->crs) {
        circleR2Bounds(geometry._cap->circle, &bounds);
    } else if (geometry._box && FLAT == geometry._box->crs) {
        bounds = geometry._box->box;
    } else if (geometry._polygon && FLAT == geometry._polygon->crs) {
        polygonR2Bounds(geometry._polygon->oldPolygon, &bounds);
    } else if (geometry._multiPoint && FLAT == geometry._multiPoint->crs) {
        multiPointR2Bounds(geometry._multiPoint->points, &bounds);
    } else if (geometry._multiLine && FLAT == geometry._multiLine->crs) {
        MONGO_verify(false);
    } else if (geometry._multiPolygon && FLAT == geometry._multiPolygon->crs) {
        MONGO_verify(false);
    } else if (geometry._geometryCollection) {
        MONGO_verify(false);
    } else if (geometry.hasS2Region()) {
        // For now, just support spherical cap for $centerSphere and GeoJSON points
        MONGO_verify((geometry._cap && FLAT != geometry._cap->crs) ||
                     (geometry._point && FLAT != geometry._point->crs));
        s2RegionR2Bounds(geometry.getS2Region(), &bounds);
    }

    return bounds;
}

GeometryContainer::GeometryContainer(const GeometryContainer& other)
    : _point{other._point},
      _line{other._line},
      _box{other._box},
      _polygon{other._polygon},
      _cap{other._cap},
      _multiPoint{other._multiPoint},
      _multiLine{other._multiLine},
      _multiPolygon{other._multiPolygon},
      _geometryCollection{other._geometryCollection} {
    if (other._s2Region) {
        _s2Region.reset(other._s2Region->Clone());
    }
    if (hasR2Region()) {
        _r2Region.reset(new R2BoxRegion(this));
    }
}

GeometryContainer& GeometryContainer::operator=(const GeometryContainer& other) {
    if (&other != this) {
        _point = other._point;
        _line = other._line;
        _box = other._box;
        _polygon = other._polygon;
        _cap = other._cap;
        _multiPoint = other._multiPoint;
        _multiLine = other._multiLine;
        _multiPolygon = other._multiPolygon;
        _geometryCollection = other._geometryCollection;

        if (other._s2Region) {
            _s2Region.reset(other._s2Region->Clone());
        }
        if (hasR2Region()) {
            _r2Region.reset(new R2BoxRegion(this));
        }
    }
    return *this;
}

const R2Region& GeometryContainer::getR2Region() const {
    return *_r2Region;
}

bool GeometryContainer::contains(const GeometryContainer& otherContainer) const {
    // First let's deal with the FLAT cases

    if (_point && FLAT == _point->crs) {
        return false;
    }

    if (nullptr != _polygon && (FLAT == _polygon->crs)) {
        if (nullptr == otherContainer._point) {
            return false;
        }
        return _polygon->oldPolygon.contains(otherContainer._point->oldPoint);
    }

    if (nullptr != _box) {
        MONGO_verify(FLAT == _box->crs);
        if (nullptr == otherContainer._point) {
            return false;
        }
        return _box->box.inside(otherContainer._point->oldPoint);
    }

    if (nullptr != _cap && (FLAT == _cap->crs)) {
        if (nullptr == otherContainer._point) {
            return false;
        }
        // Let's be as consistent epsilon-wise as we can with the '2d' indextype.
        return distanceWithin(
            _cap->circle.center, otherContainer._point->oldPoint, _cap->circle.radius);
    }

    // Now we deal with all the SPHERE stuff.

    // Iterate over the other thing and see if we contain it all.
    if (nullptr != otherContainer._point) {
        return contains(otherContainer._point->cell, otherContainer._point->point);
    }

    if (nullptr != otherContainer._line) {
        return contains(otherContainer._line->line);
    }

    if (nullptr != otherContainer._polygon) {
        tassert(7323500,
                "Checking if geometry contains big polygon is not supported",
                nullptr != otherContainer._polygon->s2Polygon);
        return contains(*otherContainer._polygon->s2Polygon);
    }

    if (nullptr != otherContainer._multiPoint) {
        for (size_t i = 0; i < otherContainer._multiPoint->points.size(); ++i) {
            if (!contains(otherContainer._multiPoint->cells[i],
                          otherContainer._multiPoint->points[i])) {
                return false;
            }
        }
        return true;
    }

    if (nullptr != otherContainer._multiLine) {
        for (const auto& line : otherContainer._multiLine->lines) {
            if (!contains(*line)) {
                return false;
            }
        }
        return true;
    }

    if (nullptr != otherContainer._multiPolygon) {
        for (const auto& polygon : otherContainer._multiPolygon->polygons) {
            if (!contains(*polygon)) {
                return false;
            }
        }
        return true;
    }

    if (nullptr != otherContainer._geometryCollection) {
        GeometryCollection& c = *otherContainer._geometryCollection;

        for (size_t i = 0; i < c.points.size(); ++i) {
            if (!contains(c.points[i].cell, c.points[i].point)) {
                return false;
            }
        }

        for (const auto& line : c.lines) {
            if (!contains(line->line)) {
                return false;
            }
        }

        for (const auto& polygon : c.polygons) {
            if (!contains(*polygon->s2Polygon)) {
                return false;
            }
        }

        for (const auto& mp : c.multiPoints) {
            for (size_t j = 0; j < mp->points.size(); ++j) {
                if (!contains(mp->cells[j], mp->points[j])) {
                    return false;
                }
            }
        }

        for (const auto& multiLine : c.multiLines) {
            for (const auto& line : multiLine->lines) {
                if (!contains(*line)) {
                    return false;
                }
            }
        }

        for (const auto& multiPolygon : c.multiPolygons) {
            for (const auto& polygon : multiPolygon->polygons) {
                if (!contains(*polygon)) {
                    return false;
                }
            }
        }

        return true;
    }

    return false;
}

bool containsPoint(const S2Polygon& poly, const S2Cell& otherCell, const S2Point& otherPoint) {
    // This is much faster for actual containment checking.
    if (poly.Contains(otherPoint)) {
        return true;
    }
    // This is slower but contains edges/vertices.
    return poly.MayIntersect(otherCell);
}

bool GeometryContainer::contains(const S2Cell& otherCell, const S2Point& otherPoint) const {
    if (nullptr != _polygon && (nullptr != _polygon->s2Polygon)) {
        return containsPoint(*_polygon->s2Polygon, otherCell, otherPoint);
    }

    if (nullptr != _polygon && (nullptr != _polygon->bigPolygon)) {
        if (_polygon->bigPolygon->Contains(otherPoint))
            return true;
        return _polygon->bigPolygon->MayIntersect(otherCell);
    }

    if (nullptr != _cap && (_cap->crs == SPHERE)) {
        return _cap->cap.MayIntersect(otherCell);
    }

    if (nullptr != _multiPolygon) {
        for (const auto& polys : _multiPolygon->polygons) {
            if (containsPoint(*polys, otherCell, otherPoint)) {
                return true;
            }
        }
    }

    if (nullptr != _geometryCollection) {
        for (const auto& polygon : _geometryCollection->polygons) {
            if (containsPoint(*polygon->s2Polygon, otherCell, otherPoint)) {
                return true;
            }
        }

        for (const auto& multiPolygon : _geometryCollection->multiPolygons) {
            for (const auto& innerPolygon : multiPolygon->polygons) {
                if (containsPoint(*innerPolygon, otherCell, otherPoint)) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool containsLine(const S2Polygon& poly, const S2Polyline& otherLine) {
    // Kind of a mess.  We get a function for clipping the line to the
    // polygon.  We do this and make sure the line is the same as the
    // line we're clipping against.
    std::vector<S2Polyline*> clipped;

    poly.IntersectWithPolyline(&otherLine, &clipped);
    const std::vector<std::unique_ptr<S2Polyline>> clippedOwned =
        transitional_tools_do_not_use::spool_vector(clipped);
    if (1 != clipped.size()) {
        return false;
    }

    // If the line is entirely contained within the polygon, we should be
    // getting it back verbatim, so really there should be no error.
    bool ret = clipped[0]->NearlyCoversPolyline(otherLine, S1Angle::Degrees(1e-10));

    return ret;
}

bool GeometryContainer::contains(const S2Polyline& otherLine) const {
    if (nullptr != _polygon && nullptr != _polygon->s2Polygon) {
        return containsLine(*_polygon->s2Polygon, otherLine);
    }

    if (nullptr != _polygon && nullptr != _polygon->bigPolygon) {
        return _polygon->bigPolygon->Contains(otherLine);
    }

    if (nullptr != _cap && (_cap->crs == SPHERE)) {
        // If the radian distance of a line to the centroid of the complement spherical cap is less
        // than the arc radian of the complement cap, then the line is not within the spherical cap.
        S2Cap complementSphere = _cap->cap.Complement();
        if (S2Distance::minDistanceRad(complementSphere.axis(), otherLine) <
            complementSphere.angle().radians()) {
            return false;
        }
        return true;
    }

    if (nullptr != _multiPolygon) {
        for (const auto& polygon : _multiPolygon->polygons) {
            if (containsLine(*polygon, otherLine)) {
                return true;
            }
        }
    }

    if (nullptr != _geometryCollection) {
        for (const auto& polygon : _geometryCollection->polygons) {
            if (containsLine(*polygon->s2Polygon, otherLine)) {
                return true;
            }
        }

        for (const auto& multiPolygon : _geometryCollection->multiPolygons) {
            for (const auto& innerPolygon : multiPolygon->polygons) {
                if (containsLine(*innerPolygon, otherLine)) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool containsPolygon(const S2Polygon& poly, const S2Polygon& otherPoly) {
    return poly.Contains(&otherPoly);
}

bool GeometryContainer::contains(const S2Polygon& otherPolygon) const {
    if (nullptr != _polygon && nullptr != _polygon->s2Polygon) {
        return containsPolygon(*_polygon->s2Polygon, otherPolygon);
    }

    if (nullptr != _polygon && nullptr != _polygon->bigPolygon) {
        return _polygon->bigPolygon->Contains(otherPolygon);
    }

    if (nullptr != _cap && (_cap->crs == SPHERE)) {
        // If the radian distance of a polygon to the centroid of the complement spherical cap is
        // less than the arc radian of the complement cap, then the polygon is not within the
        // spherical cap.
        S2Cap complementSphere = _cap->cap.Complement();
        if (S2Distance::minDistanceRad(complementSphere.axis(), otherPolygon) <
            complementSphere.angle().radians()) {
            return false;
        }
        return true;
    }

    if (nullptr != _multiPolygon) {
        for (const auto& polygon : _multiPolygon->polygons) {
            if (containsPolygon(*polygon, otherPolygon)) {
                return true;
            }
        }
    }

    if (nullptr != _geometryCollection) {
        for (const auto& polygon : _geometryCollection->polygons) {
            if (containsPolygon(*polygon->s2Polygon, otherPolygon)) {
                return true;
            }
        }

        for (const auto& multiPolygon : _geometryCollection->multiPolygons) {
            for (const auto& innerPolygon : multiPolygon->polygons) {
                if (containsPolygon(*innerPolygon, otherPolygon)) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool GeometryContainer::intersects(const GeometryContainer& otherContainer) const {
    if (nullptr != otherContainer._point) {
        return intersects(otherContainer._point->cell);
    } else if (nullptr != otherContainer._line) {
        return intersects(otherContainer._line->line);
    } else if (nullptr != otherContainer._polygon) {
        if (nullptr == otherContainer._polygon->s2Polygon) {
            return false;
        }
        return intersects(*otherContainer._polygon->s2Polygon);
    } else if (nullptr != otherContainer._multiPoint) {
        return intersects(*otherContainer._multiPoint);
    } else if (nullptr != otherContainer._multiLine) {
        return intersects(*otherContainer._multiLine);
    } else if (nullptr != otherContainer._multiPolygon) {
        return intersects(*otherContainer._multiPolygon);
    } else if (nullptr != otherContainer._geometryCollection) {
        const GeometryCollection& c = *otherContainer._geometryCollection;

        for (size_t i = 0; i < c.points.size(); ++i) {
            if (intersects(c.points[i].cell)) {
                return true;
            }
        }

        for (size_t i = 0; i < c.polygons.size(); ++i) {
            if (intersects(*c.polygons[i]->s2Polygon)) {
                return true;
            }
        }

        for (size_t i = 0; i < c.lines.size(); ++i) {
            if (intersects(c.lines[i]->line)) {
                return true;
            }
        }

        for (size_t i = 0; i < c.multiPolygons.size(); ++i) {
            if (intersects(*c.multiPolygons[i])) {
                return true;
            }
        }

        for (size_t i = 0; i < c.multiLines.size(); ++i) {
            if (intersects(*c.multiLines[i])) {
                return true;
            }
        }

        for (size_t i = 0; i < c.multiPoints.size(); ++i) {
            if (intersects(*c.multiPoints[i])) {
                return true;
            }
        }
    }

    return false;
}

bool GeometryContainer::intersects(const MultiPointWithCRS& otherMultiPoint) const {
    for (size_t i = 0; i < otherMultiPoint.cells.size(); ++i) {
        if (intersects(otherMultiPoint.cells[i])) {
            return true;
        }
    }
    return false;
}

bool GeometryContainer::intersects(const MultiLineWithCRS& otherMultiLine) const {
    for (size_t i = 0; i < otherMultiLine.lines.size(); ++i) {
        if (intersects(*otherMultiLine.lines[i])) {
            return true;
        }
    }
    return false;
}

bool GeometryContainer::intersects(const MultiPolygonWithCRS& otherMultiPolygon) const {
    for (size_t i = 0; i < otherMultiPolygon.polygons.size(); ++i) {
        if (intersects(*otherMultiPolygon.polygons[i])) {
            return true;
        }
    }
    return false;
}

// Does this (GeometryContainer) intersect the provided data?
bool GeometryContainer::intersects(const S2Cell& otherPoint) const {
    if (nullptr != _point) {
        return _point->cell.MayIntersect(otherPoint);
    } else if (nullptr != _line) {
        return _line->line.MayIntersect(otherPoint);
    } else if (nullptr != _polygon && nullptr != _polygon->s2Polygon) {
        return _polygon->s2Polygon->MayIntersect(otherPoint);
    } else if (nullptr != _polygon && nullptr != _polygon->bigPolygon) {
        return _polygon->bigPolygon->MayIntersect(otherPoint);
    } else if (nullptr != _multiPoint) {
        const vector<S2Cell>& cells = _multiPoint->cells;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (cells[i].MayIntersect(otherPoint)) {
                return true;
            }
        }
    } else if (nullptr != _multiLine) {
        for (const auto& line : _multiLine->lines) {
            if (line->MayIntersect(otherPoint)) {
                return true;
            }
        }
    } else if (nullptr != _multiPolygon) {
        for (const auto& polygon : _multiPolygon->polygons) {
            if (polygon->MayIntersect(otherPoint)) {
                return true;
            }
        }
    } else if (nullptr != _geometryCollection) {
        const GeometryCollection& c = *_geometryCollection;

        for (size_t i = 0; i < c.points.size(); ++i) {
            if (c.points[i].cell.MayIntersect(otherPoint)) {
                return true;
            }
        }

        for (size_t i = 0; i < c.polygons.size(); ++i) {
            if (c.polygons[i]->s2Polygon->MayIntersect(otherPoint)) {
                return true;
            }
        }

        for (size_t i = 0; i < c.lines.size(); ++i) {
            if (c.lines[i]->line.MayIntersect(otherPoint)) {
                return true;
            }
        }

        for (size_t i = 0; i < c.multiPolygons.size(); ++i) {
            const auto& innerPolys = c.multiPolygons[i]->polygons;
            for (size_t j = 0; j < innerPolys.size(); ++j) {
                if (innerPolys[j]->MayIntersect(otherPoint)) {
                    return true;
                }
            }
        }

        for (size_t i = 0; i < c.multiLines.size(); ++i) {
            const auto& innerLines = c.multiLines[i]->lines;
            for (size_t j = 0; j < innerLines.size(); ++j) {
                if (innerLines[j]->MayIntersect(otherPoint)) {
                    return true;
                }
            }
        }

        for (size_t i = 0; i < c.multiPoints.size(); ++i) {
            const vector<S2Cell>& innerCells = c.multiPoints[i]->cells;
            for (size_t j = 0; j < innerCells.size(); ++j) {
                if (innerCells[j].MayIntersect(otherPoint)) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool polygonLineIntersection(const S2Polyline& line, const S2Polygon& poly) {
    // TODO(hk): modify s2 library to just let us know if it intersected
    // rather than returning all this.
    vector<S2Polyline*> clipped;
    poly.IntersectWithPolyline(&line, &clipped);
    bool ret = clipped.size() > 0;
    for (size_t i = 0; i < clipped.size(); ++i)
        delete clipped[i];
    return ret;
}

bool GeometryContainer::intersects(const S2Polyline& otherLine) const {
    if (nullptr != _point) {
        return otherLine.MayIntersect(_point->cell);
    } else if (nullptr != _line) {
        return otherLine.Intersects(&_line->line);
    } else if (nullptr != _polygon && nullptr != _polygon->s2Polygon) {
        return polygonLineIntersection(otherLine, *_polygon->s2Polygon);
    } else if (nullptr != _polygon && nullptr != _polygon->bigPolygon) {
        return _polygon->bigPolygon->Intersects(otherLine);
    } else if (nullptr != _multiPoint) {
        for (size_t i = 0; i < _multiPoint->cells.size(); ++i) {
            if (otherLine.MayIntersect(_multiPoint->cells[i])) {
                return true;
            }
        }
    } else if (nullptr != _multiLine) {
        for (size_t i = 0; i < _multiLine->lines.size(); ++i) {
            if (otherLine.Intersects(_multiLine->lines[i].get())) {
                return true;
            }
        }
    } else if (nullptr != _multiPolygon) {
        for (size_t i = 0; i < _multiPolygon->polygons.size(); ++i) {
            if (polygonLineIntersection(otherLine, *_multiPolygon->polygons[i].get())) {
                return true;
            }
        }
    } else if (nullptr != _geometryCollection) {
        const GeometryCollection& c = *_geometryCollection;

        for (size_t i = 0; i < c.points.size(); ++i) {
            if (otherLine.MayIntersect(c.points[i].cell)) {
                return true;
            }
        }

        for (size_t i = 0; i < c.polygons.size(); ++i) {
            if (polygonLineIntersection(otherLine, *c.polygons[i]->s2Polygon)) {
                return true;
            }
        }

        for (size_t i = 0; i < c.lines.size(); ++i) {
            if (c.lines[i]->line.Intersects(&otherLine)) {
                return true;
            }
        }

        for (size_t i = 0; i < c.multiPolygons.size(); ++i) {
            const auto& innerPolys = c.multiPolygons[i]->polygons;
            for (size_t j = 0; j < innerPolys.size(); ++j) {
                if (polygonLineIntersection(otherLine, *innerPolys[j])) {
                    return true;
                }
            }
        }

        for (size_t i = 0; i < c.multiLines.size(); ++i) {
            const auto& innerLines = c.multiLines[i]->lines;
            for (size_t j = 0; j < innerLines.size(); ++j) {
                if (innerLines[j]->Intersects(&otherLine)) {
                    return true;
                }
            }
        }

        for (size_t i = 0; i < c.multiPoints.size(); ++i) {
            const vector<S2Cell>& innerCells = c.multiPoints[i]->cells;
            for (size_t j = 0; j < innerCells.size(); ++j) {
                if (otherLine.MayIntersect(innerCells[j])) {
                    return true;
                }
            }
        }
    }

    return false;
}

// Does 'this' intersect with the provided polygon?
bool GeometryContainer::intersects(const S2Polygon& otherPolygon) const {
    if (nullptr != _point) {
        return otherPolygon.MayIntersect(_point->cell);
    } else if (nullptr != _line) {
        return polygonLineIntersection(_line->line, otherPolygon);
    } else if (nullptr != _polygon && nullptr != _polygon->s2Polygon) {
        return otherPolygon.Intersects(_polygon->s2Polygon.get());
    } else if (nullptr != _polygon && nullptr != _polygon->bigPolygon) {
        return _polygon->bigPolygon->Intersects(otherPolygon);
    } else if (nullptr != _multiPoint) {
        for (size_t i = 0; i < _multiPoint->cells.size(); ++i) {
            if (otherPolygon.MayIntersect(_multiPoint->cells[i])) {
                return true;
            }
        }
    } else if (nullptr != _multiLine) {
        for (size_t i = 0; i < _multiLine->lines.size(); ++i) {
            if (polygonLineIntersection(*_multiLine->lines[i], otherPolygon)) {
                return true;
            }
        }
    } else if (nullptr != _multiPolygon) {
        for (size_t i = 0; i < _multiPolygon->polygons.size(); ++i) {
            if (otherPolygon.Intersects(_multiPolygon->polygons[i].get())) {
                return true;
            }
        }
    } else if (nullptr != _geometryCollection) {
        const GeometryCollection& c = *_geometryCollection;

        for (size_t i = 0; i < c.points.size(); ++i) {
            if (otherPolygon.MayIntersect(c.points[i].cell)) {
                return true;
            }
        }

        for (size_t i = 0; i < c.polygons.size(); ++i) {
            if (otherPolygon.Intersects(c.polygons[i]->s2Polygon.get())) {
                return true;
            }
        }

        for (size_t i = 0; i < c.lines.size(); ++i) {
            if (polygonLineIntersection(c.lines[i]->line, otherPolygon)) {
                return true;
            }
        }

        for (size_t i = 0; i < c.multiPolygons.size(); ++i) {
            const auto& innerPolys = c.multiPolygons[i]->polygons;
            for (size_t j = 0; j < innerPolys.size(); ++j) {
                if (otherPolygon.Intersects(innerPolys[j].get())) {
                    return true;
                }
            }
        }

        for (size_t i = 0; i < c.multiLines.size(); ++i) {
            const auto& innerLines = c.multiLines[i]->lines;
            for (size_t j = 0; j < innerLines.size(); ++j) {
                if (polygonLineIntersection(*innerLines[j], otherPolygon)) {
                    return true;
                }
            }
        }

        for (size_t i = 0; i < c.multiPoints.size(); ++i) {
            const vector<S2Cell>& innerCells = c.multiPoints[i]->cells;
            for (size_t j = 0; j < innerCells.size(); ++j) {
                if (otherPolygon.MayIntersect(innerCells[j])) {
                    return true;
                }
            }
        }
    }

    return false;
}

Status GeometryContainer::parseFromGeoJSON(bool skipValidation) {
    auto obj = _geoElm.Obj();
    GeoParser::GeoJSONType type = GeoParser::parseGeoJSONType(obj);

    if (GeoParser::GEOJSON_UNKNOWN == type) {
        return Status(ErrorCodes::BadValue, str::stream() << "unknown GeoJSON type: " << obj);
    }

    Status status = Status::OK();
    vector<S2Region*> regions;

    if (GeoParser::GEOJSON_POINT == type) {
        _point.reset(new PointWithCRS());
        status = GeoParser::parseGeoJSONPoint(obj, _point.get());
    } else if (GeoParser::GEOJSON_LINESTRING == type) {
        _line.reset(new LineWithCRS());
        status = GeoParser::parseGeoJSONLine(obj, skipValidation, _line.get());
    } else if (GeoParser::GEOJSON_POLYGON == type) {
        _polygon.reset(new PolygonWithCRS());
        status = GeoParser::parseGeoJSONPolygon(obj, skipValidation, _polygon.get());
    } else if (GeoParser::GEOJSON_MULTI_POINT == type) {
        _multiPoint.reset(new MultiPointWithCRS());
        status = GeoParser::parseMultiPoint(obj, _multiPoint.get());
        for (size_t i = 0; i < _multiPoint->cells.size(); ++i) {
            regions.push_back(&_multiPoint->cells[i]);
        }
    } else if (GeoParser::GEOJSON_MULTI_LINESTRING == type) {
        _multiLine.reset(new MultiLineWithCRS());
        status = GeoParser::parseMultiLine(obj, skipValidation, _multiLine.get());
        for (size_t i = 0; i < _multiLine->lines.size(); ++i) {
            regions.push_back(_multiLine->lines[i].get());
        }
    } else if (GeoParser::GEOJSON_MULTI_POLYGON == type) {
        _multiPolygon.reset(new MultiPolygonWithCRS());
        status = GeoParser::parseMultiPolygon(obj, skipValidation, _multiPolygon.get());
        for (size_t i = 0; i < _multiPolygon->polygons.size(); ++i) {
            regions.push_back(_multiPolygon->polygons[i].get());
        }
    } else if (GeoParser::GEOJSON_GEOMETRY_COLLECTION == type) {
        _geometryCollection.reset(new GeometryCollection());
        status = GeoParser::parseGeometryCollection(obj, skipValidation, _geometryCollection.get());

        // Add regions
        for (size_t i = 0; i < _geometryCollection->points.size(); ++i) {
            regions.push_back(&_geometryCollection->points[i].cell);
        }
        for (size_t i = 0; i < _geometryCollection->lines.size(); ++i) {
            regions.push_back(&_geometryCollection->lines[i]->line);
        }
        for (size_t i = 0; i < _geometryCollection->polygons.size(); ++i) {
            regions.push_back(_geometryCollection->polygons[i]->s2Polygon.get());
        }
        for (size_t i = 0; i < _geometryCollection->multiPoints.size(); ++i) {
            MultiPointWithCRS* multiPoint = _geometryCollection->multiPoints[i].get();
            for (size_t j = 0; j < multiPoint->cells.size(); ++j) {
                regions.push_back(&multiPoint->cells[j]);
            }
        }
        for (size_t i = 0; i < _geometryCollection->multiLines.size(); ++i) {
            const MultiLineWithCRS* multiLine = _geometryCollection->multiLines[i].get();
            for (size_t j = 0; j < multiLine->lines.size(); ++j) {
                regions.push_back(multiLine->lines[j].get());
            }
        }
        for (size_t i = 0; i < _geometryCollection->multiPolygons.size(); ++i) {
            const MultiPolygonWithCRS* multiPolygon = _geometryCollection->multiPolygons[i].get();
            for (size_t j = 0; j < multiPolygon->polygons.size(); ++j) {
                regions.push_back(multiPolygon->polygons[j].get());
            }
        }
    } else {
        MONGO_UNREACHABLE_TASSERT(9911954);
    }

    // Check parsing result.
    if (!status.isOK())
        return status;

    if (regions.size() > 0) {
        // S2RegionUnion doesn't take ownership of pointers.
        _s2Region.reset(new S2RegionUnion(&regions));
    }

    return Status::OK();
}

// Examples:
// { $geoWithin : { $geometry : <GeoJSON> } }
// { $geoIntersects : { $geometry : <GeoJSON> } }
// { $geoWithin : { $box : [[x1, y1], [x2, y2]] } }
// { $geoWithin : { $polygon : [[x1, y1], [x1, y2], [x2, y2], [x2, y1]] } }
// { $geoWithin : { $center : [[x1, y1], r], } }
// { $geoWithin : { $centerSphere : [[x, y], radius] } }
// { $geoIntersects : { $geometry : [1, 2] } }
//
// "elem" is the first element of the object after $geoWithin / $geoIntersects predicates.
// i.e. { $box: ... }, { $geometry: ... }
Status GeometryContainer::parseFromQuery(const BSONElement& elem) {
    // Check elem is an object and has geo specifier.
    GeoParser::GeoSpecifier specifier = GeoParser::parseGeoSpecifier(elem);

    if (GeoParser::UNKNOWN == specifier) {
        // Cannot parse geo specifier.
        return Status(ErrorCodes::BadValue, str::stream() << "unknown geo specifier: " << elem);
    }

    Status status = Status::OK();
    _geoElm = elem;
    auto obj = elem.Obj();
    if (GeoParser::BOX == specifier) {
        _box.reset(new BoxWithCRS());
        status = GeoParser::parseLegacyBox(obj, _box.get());
    } else if (GeoParser::CENTER == specifier) {
        _cap.reset(new CapWithCRS());
        status = GeoParser::parseLegacyCenter(obj, _cap.get());
    } else if (GeoParser::POLYGON == specifier) {
        _polygon.reset(new PolygonWithCRS());
        status = GeoParser::parseLegacyPolygon(obj, _polygon.get());
    } else if (GeoParser::CENTER_SPHERE == specifier) {
        _cap.reset(new CapWithCRS());
        status = GeoParser::parseCenterSphere(obj, _cap.get());
    } else if (GeoParser::GEOMETRY == specifier) {
        // GeoJSON geometry or legacy point
        if (BSONType::array == elem.type() || obj.firstElement().isNumber()) {
            // legacy point
            _point.reset(new PointWithCRS());
            status = GeoParser::parseQueryPoint(elem, _point.get());
        } else {
            // GeoJSON geometry
            status = parseFromGeoJSON();
        }
    }
    if (!status.isOK())
        return status;

    // If we support R2 regions, build the region immediately
    if (hasR2Region()) {
        _r2Region.reset(new R2BoxRegion(this));
    }

    return status;
}

// Examples:
// { location: <GeoJSON> }
// { location: [1, 2] }
// { location: [1, 2, 3] }
// { location: {x: 1, y: 2} }
//
// "elem" is the element that contains geo data. e.g. "location": [1, 2]
// We need the type information to determine whether it's legacy point.
Status GeometryContainer::parseFromStorage(const BSONElement& elem, bool skipValidation) {
    if (!elem.isABSONObj()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "geo element must be an array or object: " << elem);
    }

    _geoElm = elem;
    Status status = Status::OK();
    if (BSONType::object == elem.type()) {
        // GeoJSON
        // { location: { type: “Point”, coordinates: [...] } }
        status = parseFromGeoJSON(skipValidation);

        // It's possible that we are dealing with a legacy point. e.g
        // { location: {x: 1, y: 2, type: “Point” } }
        // { location: {x: 1, y: 2} }
        if (status == ErrorCodes::BadValue) {
            // We must reset _point each time we attempt to re-parse, since it may retain info from
            // previous attempts.
            _point.reset(new PointWithCRS());
            Status legacyParsingStatus = GeoParser::parseLegacyPoint(elem, _point.get(), true);
            if (legacyParsingStatus.isOK()) {
                status = legacyParsingStatus;
            } else {
                // Return the original error status, as we may be dealing with an invalid GeoJSON
                // document. e.g. {type: "Point", coordinates: "hello"}
                return status;
            }
        }
    } else {
        // Legacy point
        // { location: [1, 2] }
        // { location: [1, 2, 3] }
        // Allow more than two dimensions or extra fields, like [1, 2, 3]
        // We must reset _point each time we attempt to re-parse, since it may retain info from
        // previous attempts.
        _point.reset(new PointWithCRS());
        status = GeoParser::parseLegacyPoint(elem, _point.get(), true);
    }

    if (!status.isOK())
        return status;

    // If we support R2 regions, build the region immediately
    if (hasR2Region())
        _r2Region.reset(new R2BoxRegion(this));

    return Status::OK();
}

string GeometryContainer::getDebugType() const {
    if (nullptr != _point) {
        return "pt";
    } else if (nullptr != _line) {
        return "ln";
    } else if (nullptr != _box) {
        return "bx";
    } else if (nullptr != _polygon) {
        return "pl";
    } else if (nullptr != _cap) {
        return "cc";
    } else if (nullptr != _multiPoint) {
        return "mp";
    } else if (nullptr != _multiLine) {
        return "ml";
    } else if (nullptr != _multiPolygon) {
        return "my";
    } else if (nullptr != _geometryCollection) {
        return "gc";
    } else {
        MONGO_UNREACHABLE_TASSERT(9911955);
        return "";
    }
}

CRS GeometryContainer::getNativeCRS() const {
    // TODO: Fix geometry collection reporting when/if we support multiple CRSes

    if (nullptr != _point) {
        return _point->crs;
    } else if (nullptr != _line) {
        return _line->crs;
    } else if (nullptr != _box) {
        return _box->crs;
    } else if (nullptr != _polygon) {
        return _polygon->crs;
    } else if (nullptr != _cap) {
        return _cap->crs;
    } else if (nullptr != _multiPoint) {
        return _multiPoint->crs;
    } else if (nullptr != _multiLine) {
        return _multiLine->crs;
    } else if (nullptr != _multiPolygon) {
        return _multiPolygon->crs;
    } else if (nullptr != _geometryCollection) {
        return SPHERE;
    } else {
        MONGO_UNREACHABLE_TASSERT(9911956);
        return FLAT;
    }
}

bool GeometryContainer::supportsProject(CRS otherCRS) const {
    // TODO: Fix geometry collection reporting when/if we support more CRSes

    if (nullptr != _point) {
        return ShapeProjection::supportsProject(*_point, otherCRS);
    } else if (nullptr != _line) {
        return _line->crs == otherCRS;
    } else if (nullptr != _box) {
        return _box->crs == otherCRS;
    } else if (nullptr != _polygon) {
        return ShapeProjection::supportsProject(*_polygon, otherCRS);
    } else if (nullptr != _cap) {
        return _cap->crs == otherCRS;
    } else if (nullptr != _multiPoint) {
        return _multiPoint->crs == otherCRS;
    } else if (nullptr != _multiLine) {
        return _multiLine->crs == otherCRS;
    } else if (nullptr != _multiPolygon) {
        return _multiPolygon->crs == otherCRS;
    } else {
        tassert(9911929, "", nullptr != _geometryCollection);
        return SPHERE == otherCRS;
    }
}

void GeometryContainer::projectInto(CRS otherCRS) {
    if (getNativeCRS() == otherCRS)
        return;

    if (nullptr != _polygon) {
        ShapeProjection::projectInto(_polygon.get(), otherCRS);
        return;
    }

    tassert(9911930, "", nullptr != _point);
    ShapeProjection::projectInto(_point.get(), otherCRS);
}

static double s2MinDistanceRad(const S2Point& s2Point, const MultiPointWithCRS& s2MultiPoint) {
    double minDistance = -1;
    for (vector<S2Point>::const_iterator it = s2MultiPoint.points.begin();
         it != s2MultiPoint.points.end();
         ++it) {
        double nextDistance = S2Distance::distanceRad(s2Point, *it);
        if (minDistance < 0 || nextDistance < minDistance) {
            minDistance = nextDistance;
        }
    }

    return minDistance;
}

static double s2MinDistanceRad(const S2Point& s2Point, const MultiLineWithCRS& s2MultiLine) {
    double minDistance = -1;
    for (const auto& line : s2MultiLine.lines) {
        double nextDistance = S2Distance::minDistanceRad(s2Point, *line);
        if (minDistance < 0 || nextDistance < minDistance) {
            minDistance = nextDistance;
        }
    }

    return minDistance;
}

static double s2MinDistanceRad(const S2Point& s2Point, const MultiPolygonWithCRS& s2MultiPolygon) {
    double minDistance = -1;
    for (const auto& polygon : s2MultiPolygon.polygons) {
        double nextDistance = S2Distance::minDistanceRad(s2Point, *polygon);
        if (minDistance < 0 || nextDistance < minDistance) {
            minDistance = nextDistance;
        }
    }

    return minDistance;
}

static double s2MinDistanceRad(const S2Point& s2Point,
                               const GeometryCollection& geometryCollection) {
    double minDistance = -1;
    for (vector<PointWithCRS>::const_iterator it = geometryCollection.points.begin();
         it != geometryCollection.points.end();
         ++it) {
        tassert(9911931, "", SPHERE == it->crs);
        double nextDistance = S2Distance::distanceRad(s2Point, it->point);
        if (minDistance < 0 || nextDistance < minDistance) {
            minDistance = nextDistance;
        }
    }

    for (const auto& line : geometryCollection.lines) {
        tassert(9911932, "", SPHERE == line->crs);
        double nextDistance = S2Distance::minDistanceRad(s2Point, line->line);
        if (minDistance < 0 || nextDistance < minDistance) {
            minDistance = nextDistance;
        }
    }

    for (const auto& polygon : geometryCollection.polygons) {
        tassert(9911933, "", SPHERE == polygon->crs);
        // We don't support distances for big polygons yet.
        tassert(9911934, "", polygon->s2Polygon);
        double nextDistance = S2Distance::minDistanceRad(s2Point, *(polygon->s2Polygon));
        if (minDistance < 0 || nextDistance < minDistance) {
            minDistance = nextDistance;
        }
    }

    for (const auto& multiPoint : geometryCollection.multiPoints) {
        double nextDistance = s2MinDistanceRad(s2Point, *multiPoint);
        if (minDistance < 0 || nextDistance < minDistance) {
            minDistance = nextDistance;
        }
    }

    for (const auto& multiLine : geometryCollection.multiLines) {
        double nextDistance = s2MinDistanceRad(s2Point, *multiLine);
        if (minDistance < 0 || nextDistance < minDistance) {
            minDistance = nextDistance;
        }
    }

    for (const auto& multiPolygon : geometryCollection.multiPolygons) {
        double nextDistance = s2MinDistanceRad(s2Point, *multiPolygon);
        if (minDistance < 0 || nextDistance < minDistance) {
            minDistance = nextDistance;
        }
    }

    return minDistance;
}

double GeometryContainer::minDistance(const PointWithCRS& otherPoint) const {
    const CRS crs = getNativeCRS();

    if (FLAT == crs) {
        tassert(9911935, "", nullptr != _point);

        if (FLAT == otherPoint.crs) {
            return distance(_point->oldPoint, otherPoint.oldPoint);
        } else {
            S2LatLng latLng(otherPoint.point);
            return distance(_point->oldPoint,
                            Point(latLng.lng().degrees(), latLng.lat().degrees()));
        }
    } else {
        tassert(9911936, "", SPHERE == crs);

        double minDistance = -1;

        if (nullptr != _point) {
            // SERVER-52953: Calculating the distance between identical points can sometimes result
            // in a small positive value due to a loss of floating point precision on certain
            // platforms. As such, we perform a simple equality check to guarantee that equivalent
            // points will always produce a distance of 0.
            if (_point->point == otherPoint.point) {
                minDistance = 0;
            } else {
                minDistance = S2Distance::distanceRad(otherPoint.point, _point->point);
            }
        } else if (nullptr != _line) {
            minDistance = S2Distance::minDistanceRad(otherPoint.point, _line->line);
        } else if (nullptr != _polygon) {
            // We don't support distances for big polygons yet.
            tassert(9911937, "", nullptr != _polygon->s2Polygon);
            minDistance = S2Distance::minDistanceRad(otherPoint.point, *_polygon->s2Polygon);
        } else if (nullptr != _cap) {
            minDistance = S2Distance::minDistanceRad(otherPoint.point, _cap->cap);
        } else if (nullptr != _multiPoint) {
            minDistance = s2MinDistanceRad(otherPoint.point, *_multiPoint);
        } else if (nullptr != _multiLine) {
            minDistance = s2MinDistanceRad(otherPoint.point, *_multiLine);
        } else if (nullptr != _multiPolygon) {
            minDistance = s2MinDistanceRad(otherPoint.point, *_multiPolygon);
        } else if (nullptr != _geometryCollection) {
            minDistance = s2MinDistanceRad(otherPoint.point, *_geometryCollection);
        }

        tassert(9911938, "", minDistance != -1);
        return minDistance * kRadiusOfEarthInMeters;
    }
}

const CapWithCRS* GeometryContainer::getCapGeometryHack() const {
    return _cap.get();
}

StoredGeometry* StoredGeometry::parseFrom(const BSONElement& element, bool skipValidation) {
    if (!element.isABSONObj())
        return nullptr;

    std::unique_ptr<StoredGeometry> stored(new StoredGeometry);
    if (!stored->geometry.parseFromStorage(element, skipValidation).isOK())
        return nullptr;
    stored->element = element;
    return stored.release();
}

/**
 * Find and parse all geometry elements on the appropriate field path from the document.
 */
void StoredGeometry::extractGeometries(const BSONObj& doc,
                                       const string& path,
                                       std::vector<std::unique_ptr<StoredGeometry>>* geometries,
                                       bool skipValidation) {
    BSONElementSet geomElements;
    // NOTE: Annoyingly, we cannot just expand arrays b/c single 2d points are arrays, we need
    // to manually expand all results to check if they are geometries
    ::mongo::multikey_dotted_path_support::extractAllElementsAlongPath(
        doc, path, geomElements, false /* expand arrays */);

    for (BSONElementSet::iterator it = geomElements.begin(); it != geomElements.end(); ++it) {
        const BSONElement& el = *it;
        std::unique_ptr<StoredGeometry> stored(StoredGeometry::parseFrom(el, skipValidation));

        if (stored.get()) {
            // Valid geometry element
            geometries->push_back(std::move(stored));
        } else if (el.type() == BSONType::array) {
            // Many geometries may be in an array
            BSONObjIterator arrIt(el.Obj());
            while (arrIt.more()) {
                const BSONElement nextEl = arrIt.next();
                stored.reset(StoredGeometry::parseFrom(nextEl, skipValidation));

                if (stored.get()) {
                    // Valid geometry element
                    geometries->push_back(std::move(stored));
                } else {
                    LOGV2_WARNING(23760,
                                  "geoNear stage read non-geometry element in array",
                                  "nextElement"_attr = redact(nextEl),
                                  "element"_attr = redact(el));
                }
            }
        } else {
            LOGV2_WARNING(
                23761, "geoNear stage read non-geometry element", "element"_attr = redact(el));
        }
    }
}

}  // namespace mongo
