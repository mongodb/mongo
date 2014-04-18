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

#include "mongo/db/exec/2d.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/catalog/collection.h"

namespace mongo {

    TwoD::TwoD(const TwoDParams& params, WorkingSet* ws)
        : _params(params), _workingSet(ws), _initted(false),
          _descriptor(NULL), _am(NULL) {
    }

    TwoD::~TwoD() { }

    bool TwoD::isEOF() {
        return _initted && (NULL == _browse.get());
    }

    PlanStage::StageState TwoD::work(WorkingSetID* out) {
        if (isEOF()) { return PlanStage::IS_EOF; }

        if (!_initted) {
            _initted = true;

            if ( !_params.collection )
                return PlanStage::IS_EOF;

            IndexCatalog* indexCatalog = _params.collection->getIndexCatalog();

            _descriptor = indexCatalog->findIndexByKeyPattern(_params.indexKeyPattern);
            if ( _descriptor == NULL )
                return PlanStage::IS_EOF;

            _am = static_cast<TwoDAccessMethod*>( indexCatalog->getIndex( _descriptor ) );
            verify( _am );

            if (NULL != _params.gq.getGeometry()._cap.get()) {
                _browse.reset(new twod_exec::GeoCircleBrowse(_params, _am));
            }
            else if (NULL != _params.gq.getGeometry()._polygon.get()) {
                _browse.reset(new twod_exec::GeoPolygonBrowse(_params, _am));
            }
            else {
                verify(NULL != _params.gq.getGeometry()._box.get());
                _browse.reset(new twod_exec::GeoBoxBrowse(_params, _am));
            }

            // Fill out static portion of plan stats.
            // We will retrieve the geo hashes used by the geo browser
            // when the search is complete.
            _specificStats.type = _browse->_type;
            _specificStats.field = _params.gq.getField();
            _specificStats.converterParams = _browse->_converter->getParams();

            return PlanStage::NEED_TIME;
        }

        verify(NULL != _browse.get());

        if (!_browse->ok()) {
            // Grab geo hashes before disposing geo browser.
            _specificStats.expPrefixes.swap(_browse->_expPrefixes);
            _browse.reset();
            return PlanStage::IS_EOF;
        }

        WorkingSetID id = _workingSet->allocate();
        WorkingSetMember* member = _workingSet->get(id);
        member->loc = _browse->currLoc();
        member->obj = member->loc.obj();
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;

        _browse->advance();

        *out = id;
        _commonStats.advanced++;
        _commonStats.works++;
        return PlanStage::ADVANCED;
    }

    void TwoD::prepareToYield() {
        if (NULL != _browse) {
            _browse->noteLocation();
        }
    }

    void TwoD::recoverFromYield() {
        if (NULL != _browse) {
            _browse->checkLocation();
        }
    }

    void TwoD::invalidate(const DiskLoc& dl, InvalidationType type) {
        if (NULL != _browse) {
            // If the invalidation actually tossed out a result...
            if (_browse->invalidate(dl)) {
                // Create a new WSM
                WorkingSetID id = _workingSet->allocate();
                WorkingSetMember* member = _workingSet->get(id);
                member->loc = dl;
                member->obj = member->loc.obj();
                member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;

                // And flag it for later.
                WorkingSetCommon::fetchAndInvalidateLoc(member);
                _workingSet->flagForReview(id);
            }
        }
    }

    PlanStageStats* TwoD::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_GEO_2D));
        ret->specific.reset(new TwoDStats(_specificStats));
        return ret.release();
    }
}

namespace mongo {
namespace twod_exec {


    //
    // Impls of browse below
    //

    //
    // GeoCircleBrowse
    //

    GeoCircleBrowse::GeoCircleBrowse(const TwoDParams& params, TwoDAccessMethod* accessMethod)
        : GeoBrowse(accessMethod, "circle", params.filter) {

        _converter = accessMethod->getParams().geoHashConverter;

        const CapWithCRS& cap = *params.gq.getGeometry()._cap;

        _startPt = cap.circle.center;
        _start = _converter->hash(_startPt);
        _maxDistance = cap.circle.radius;

        if (FLAT == cap.crs) {
            _type = GEO_PLANE;
            xScanDistance = _maxDistance + _converter->getError();
            yScanDistance = _maxDistance + _converter->getError();
        } else {
            _type = GEO_SPHERE;
            yScanDistance = rad2deg(_maxDistance) + _converter->getError();
            xScanDistance = computeXScanDistance(_startPt.y, yScanDistance);
        }

        // Bounding box includes fudge factor.
        // TODO:  Is this correct, since fudge factor may be spherically transformed?
        _bBox._min = Point(_startPt.x - xScanDistance, _startPt.y - yScanDistance);
        _bBox._max = Point(_startPt.x + xScanDistance, _startPt.y + yScanDistance);

        ok();
    }

    GeoAccumulator::KeyResult GeoCircleBrowse::approxKeyCheck(const Point& p, double& d) {
        // Inexact hash distance checks.
        double error = 0;
        switch (_type) {
        case GEO_PLANE:
            d = distance(_startPt, p);
            error = _converter->getError();
            break;
        case GEO_SPHERE: {
            checkEarthBounds(p);
            d = spheredist_deg(_startPt, p);
            error = _converter->getErrorSphere();
            break;
        }
        default: verify(false);
        }

        // If our distance is in the error bounds...
        if(d >= _maxDistance - error && d <= _maxDistance + error) return BORDER;
        return d > _maxDistance ? BAD : GOOD;
    }

     bool GeoCircleBrowse::exactDocCheck(const Point& p, double& d){
        switch (_type) {
        case GEO_PLANE: {
            if(distanceWithin(_startPt, p, _maxDistance)) return true;
            break;
        }
        case GEO_SPHERE:
            checkEarthBounds(p);
            if(spheredist_deg(_startPt, p) <= _maxDistance) return true;
            break;
        default: verify(false);
        }

        return false;
    }

    //
    // GeoBoxBrowse
    //

    GeoBoxBrowse::GeoBoxBrowse(const TwoDParams& params, TwoDAccessMethod* accessMethod)
        : GeoBrowse(accessMethod, "box", params.filter) {

        _converter = accessMethod->getParams().geoHashConverter;

        _want = params.gq.getGeometry()._box->box;
        _wantRegion = _want;
        // Need to make sure we're checking regions within error bounds of where we want
        _wantRegion.fudge(_converter->getError());
        fixBox(_wantRegion);
        fixBox(_want);

        Point center = _want.center();
        _start = _converter->hash(center.x, center.y);

        _fudge = _converter->getError();
        _wantLen = _fudge +
                   std::max((_want._max.x - _want._min.x),
                             (_want._max.y - _want._min.y)) / 2;

        ok();
    }

    void GeoBoxBrowse::fixBox(Box& box) {
        if(box._min.x > box._max.x)
            std::swap(box._min.x, box._max.x);
        if(box._min.y > box._max.y)
            std::swap(box._min.y, box._max.y);

        double gMin = _converter->getMin();
        double gMax = _converter->getMax();

        if(box._min.x < gMin) box._min.x = gMin;
        if(box._min.y < gMin) box._min.y = gMin;
        if(box._max.x > gMax) box._max.x = gMax;
        if(box._max.y > gMax) box._max.y = gMax;
    }

    //
    // GeoPolygonBrowse
    //

    GeoPolygonBrowse::GeoPolygonBrowse(const TwoDParams& params, TwoDAccessMethod* accessMethod)
        : GeoBrowse(accessMethod, "polygon", params.filter) {

        _converter = accessMethod->getParams().geoHashConverter;

        _poly = params.gq.getGeometry()._polygon->oldPolygon;
        _bounds = _poly.bounds();
        // We need to check regions within the error bounds of these bounds
        _bounds.fudge(_converter->getError()); 
        // We don't need to look anywhere outside the space
        _bounds.truncate(_converter->getMin(), _converter->getMax()); 
        _maxDim = _converter->getError() + _bounds.maxDim() / 2;

        ok();
    }

}  // namespace twod_exec
}  // namespace mongo
