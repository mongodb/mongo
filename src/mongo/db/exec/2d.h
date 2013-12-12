/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/exec/2dcommon.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/geo/geoquery.h"

#pragma once

namespace mongo {

    struct TwoDParams {
        TwoDParams() : filter(NULL) { }
        GeoQuery gq;
        MatchExpression* filter;
        BSONObj indexKeyPattern;
        string ns;
    };

    class TwoD : public PlanStage {
    public:
        TwoD(const TwoDParams& params, WorkingSet* ws);
        virtual ~TwoD();

        virtual bool isEOF();
        virtual StageState work(WorkingSetID* out);

        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl);

        virtual PlanStageStats* getStats();
    private:
        scoped_ptr<mongo::twod_exec::GeoBrowse> _browse;
        TwoDParams _params;
        WorkingSet* _workingSet;
        bool _initted;
        IndexDescriptor* _descriptor;
        TwoDAccessMethod* _am;
        CommonStats _commonStats;
    };
}

namespace mongo {
namespace twod_exec {

    //
    // Impls of browse below
    //

    class GeoCircleBrowse : public GeoBrowse {
    public:
        GeoCircleBrowse(const TwoDParams& params, TwoDAccessMethod* accessMethod);

        virtual GeoHash expandStartHash() { return _start; }

        virtual bool fitsInBox(double width) {
            return width >= std::max(xScanDistance, yScanDistance);
        }

        virtual double intersectsBox(Box& cur) {
            return cur.intersects(_bBox);
        }

        virtual KeyResult approxKeyCheck(const Point& p, double& d);

        virtual bool exactDocCheck(const Point& p, double& d);

        GeoDistType _type;
        GeoHash _start;
        Point _startPt;
        double _maxDistance; // user input
        double xScanDistance; // effected by GeoDistType
        double yScanDistance; // effected by GeoDistType
        Box _bBox;

        shared_ptr<GeoHashConverter> _converter;
    };

    class GeoBoxBrowse : public GeoBrowse {
    public:
        GeoBoxBrowse(const TwoDParams& params, TwoDAccessMethod* accessMethod);

        void fixBox(Box& box);

        virtual GeoHash expandStartHash() {
            return _start;
        }

        virtual bool fitsInBox(double width) {
            return width >= _wantLen;
        }

        virtual double intersectsBox(Box& cur) {
            return cur.intersects(_wantRegion);
        }

        virtual KeyResult approxKeyCheck(const Point& p, double& d) {
            if(_want.onBoundary(p, _fudge)) return BORDER;
            else return _want.inside(p, _fudge) ? GOOD : BAD;

        }

        virtual bool exactDocCheck(const Point& p, double& d){
            return _want.inside(p);
        }

        Box _want;
        Box _wantRegion;
        double _wantLen;
        double _fudge;
        GeoHash _start;
        shared_ptr<GeoHashConverter> _converter;
    };

    class GeoPolygonBrowse : public GeoBrowse {
    public:
        GeoPolygonBrowse(const TwoDParams& params, TwoDAccessMethod* accessMethod);

        // The initial geo hash box for our first expansion
        virtual GeoHash expandStartHash() {
            return _converter->hash(_bounds.center());
        }

        // Whether the current box width is big enough for our search area
        virtual bool fitsInBox(double width) {
            return _maxDim <= width;
        }

        // Whether the current box overlaps our search area
        virtual double intersectsBox(Box& cur) {
            return cur.intersects(_bounds);
        }

        virtual KeyResult approxKeyCheck(const Point& p, double& d) {
            int in = _poly.contains(p, _converter->getError());
            if(in == 0) return BORDER;
            else return in > 0 ? GOOD : BAD;
        }

        virtual bool exactDocCheck(const Point& p, double& d){
            return _poly.contains(p);
        }

    private:
        Polygon _poly;
        Box _bounds;
        double _maxDim;
        GeoHash _start;
        shared_ptr<GeoHashConverter> _converter;
    };
}  // namespace twod_exec
}  // namespace mongo
