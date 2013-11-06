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

#include "mongo/db/exec/projection.h"

#include "mongo/db/diskloc.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    ProjectionStage::ProjectionStage(LiteProjection* liteProjection,
                                     bool covered,
                                     const MatchExpression* fullExpression,
                                     WorkingSet* ws,
                                     PlanStage* child,
                                     const MatchExpression* filter)
        : _liteProjection(liteProjection),
          _covered(covered),
          _ws(ws),
          _child(child),
          _filter(filter),
          _fullExpression(fullExpression) { }

    ProjectionStage::~ProjectionStage() { }

    bool ProjectionStage::isEOF() { return _child->isEOF(); }

    PlanStage::StageState ProjectionStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        if (isEOF()) { return PlanStage::IS_EOF; }
        WorkingSetID id;
        StageState status = _child->work(&id);

        if (PlanStage::ADVANCED == status) {
            WorkingSetMember* member = _ws->get(id);

            BSONObj newObj;
            if (_covered) {
                // TODO: Rip execution out of the lite_projection and pass a WSM to something
                // which does the right thing depending on the covered vs. noncovered cases.
                BSONObjBuilder bob;
                if (_liteProjection->_includeID) {
                    BSONElement elt;
                    member->getFieldDotted("_id", &elt);
                    verify(!elt.eoo());
                    bob.appendAs(elt, "_id");
                }

                BSONObjIterator it(_liteProjection->_source);
                while (it.more()) {
                    BSONElement specElt = it.next();
                    if (mongoutils::str::equals("_id", specElt.fieldName())) {
                        continue;
                    }

                    BSONElement keyElt;
                    // We can project a field that doesn't exist.  We just ignore it.
                    if (member->getFieldDotted(specElt.fieldName(), &keyElt) && !keyElt.eoo()) {
                        bob.appendAs(keyElt, specElt.fieldName());
                    }
                }
                newObj = bob.obj();
            }
            else {
                // Planner should have done this.
                verify(member->hasObj());

                MatchDetails matchDetails;
                matchDetails.requestElemMatchKey();

                if (_liteProjection->transformRequiresDetails()) {
                    verify(_fullExpression->matchesBSON(member->obj, &matchDetails));
                }

                Status projStatus = _liteProjection->transform(member->obj, &newObj, &matchDetails);
                if (!projStatus.isOK()) {
                    warning() << "Couldn't execute projection, status = " << projStatus.toString() << endl;
                    return PlanStage::FAILURE;
                }
            }

            member->state = WorkingSetMember::OWNED_OBJ;
            member->obj = newObj;
            member->keyData.clear();
            member->loc = DiskLoc();

            *out = id;
            ++_commonStats.advanced;
        }
        else if (PlanStage::NEED_FETCH == status) {
            *out = id;
            ++_commonStats.needFetch;
        }

        return status;
    }

    void ProjectionStage::prepareToYield() {
        ++_commonStats.yields;
        _child->prepareToYield();
    }

    void ProjectionStage::recoverFromYield() {
        ++_commonStats.unyields;
        _child->recoverFromYield();
    }

    void ProjectionStage::invalidate(const DiskLoc& dl) {
        ++_commonStats.invalidates;
        _child->invalidate(dl);
    }

    PlanStageStats* ProjectionStage::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_PROJECTION));
        ret->children.push_back(_child->getStats());
        return ret.release();
    }

}  // namespace mongo
