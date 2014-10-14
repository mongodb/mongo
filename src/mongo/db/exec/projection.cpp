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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/db/exec/projection.h"

#include "mongo/db/diskloc.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    static const char* kIdField = "_id";

    // static
    const char* ProjectionStage::kStageType = "PROJECTION";

    ProjectionStage::ProjectionStage(const ProjectionStageParams& params,
                                     WorkingSet* ws,
                                     PlanStage* child)
        : _ws(ws),
          _child(child),
          _commonStats(kStageType),
          _projImpl(params.projImpl) {

        _projObj = params.projObj;

        if (ProjectionStageParams::NO_FAST_PATH == _projImpl) {
            _exec.reset(new ProjectionExec(params.projObj, 
                                           params.fullExpression,
                                           *params.whereCallback));
        }
        else {
            // We shouldn't need the full expression if we're fast-pathing.
            invariant(NULL == params.fullExpression);

            // Sanity-check the input.
            invariant(_projObj.isOwned());
            invariant(!_projObj.isEmpty());

            // Figure out what fields are in the projection.
            getSimpleInclusionFields(_projObj, &_includedFields);

            // If we're pulling data out of one index we can pre-compute the indices of the fields
            // in the key that we pull data from and avoid looking up the field name each time.
            if (ProjectionStageParams::COVERED_ONE_INDEX == params.projImpl) {
                // Sanity-check.
                _coveredKeyObj = params.coveredKeyObj;
                invariant(_coveredKeyObj.isOwned());

                BSONObjIterator kpIt(_coveredKeyObj);
                while (kpIt.more()) {
                    BSONElement elt = kpIt.next();
                    unordered_set<StringData, StringData::Hasher>::iterator fieldIt;
                    fieldIt = _includedFields.find(elt.fieldNameStringData());

                    if (_includedFields.end() == fieldIt) {
                        // Push an unused value on the back to keep _includeKey and _keyFieldNames
                        // in sync.
                        _keyFieldNames.push_back(StringData());
                        _includeKey.push_back(false);
                    }
                    else {
                        // If we are including this key field store its field name.
                        _keyFieldNames.push_back(*fieldIt);
                        _includeKey.push_back(true);
                    }
                }
            }
            else {
                invariant(ProjectionStageParams::SIMPLE_DOC == params.projImpl);
            }
        }
    }

    // static
    void ProjectionStage::getSimpleInclusionFields(const BSONObj& projObj,
                                                   FieldSet* includedFields) {
        // The _id is included by default.
        bool includeId = true;

        // Figure out what fields are in the projection.  TODO: we can get this from the
        // ParsedProjection...modify that to have this type instead of a vector.
        BSONObjIterator projObjIt(projObj);
        while (projObjIt.more()) {
            BSONElement elt = projObjIt.next();
            // Must deal with the _id case separately as there is an implicit _id: 1 in the
            // projection.
            if (mongoutils::str::equals(elt.fieldName(), kIdField)
                && !elt.trueValue()) {
                includeId = false;
                continue;
            }
            includedFields->insert(elt.fieldNameStringData());
        }

        if (includeId) {
            includedFields->insert(kIdField);
        }
    }

    // static
    void ProjectionStage::transformSimpleInclusion(const BSONObj& in,
                                                   const FieldSet& includedFields,
                                                   BSONObjBuilder& bob) {
        // Look at every field in the source document and see if we're including it.
        BSONObjIterator inputIt(in);
        while (inputIt.more()) {
            BSONElement elt = inputIt.next();
            unordered_set<StringData, StringData::Hasher>::const_iterator fieldIt;
            fieldIt = includedFields.find(elt.fieldNameStringData());
            if (includedFields.end() != fieldIt) {
                // If so, add it to the builder.
                bob.append(elt);
            }
        }
    }

    Status ProjectionStage::transform(WorkingSetMember* member) {
        // The default no-fast-path case.
        if (ProjectionStageParams::NO_FAST_PATH == _projImpl) {
            return _exec->transform(member);
        }

        BSONObjBuilder bob;

        // Note that even if our fast path analysis is bug-free something that is
        // covered might be invalidated and just be an obj.  In this case we just go
        // through the SIMPLE_DOC path which is still correct if the covered data
        // is not available.
        //
        // SIMPLE_DOC implies that we expect an object so it's kind of redundant.
        if ((ProjectionStageParams::SIMPLE_DOC == _projImpl) || member->hasObj()) {
            // If we got here because of SIMPLE_DOC the planner shouldn't have messed up.
            invariant(member->hasObj());

            // Apply the SIMPLE_DOC projection.
            transformSimpleInclusion(member->obj, _includedFields, bob);
        }
        else {
            invariant(ProjectionStageParams::COVERED_ONE_INDEX == _projImpl);
            // We're pulling data out of the key.
            invariant(1 == member->keyData.size());
            size_t keyIndex = 0;

            // Look at every key element...
            BSONObjIterator keyIterator(member->keyData[0].keyData);
            while (keyIterator.more()) {
                BSONElement elt = keyIterator.next();
                // If we're supposed to include it...
                if (_includeKey[keyIndex]) {
                    // Do so.
                    bob.appendAs(elt, _keyFieldNames[keyIndex]);
                }
                ++keyIndex;
            }
        }

        member->state = WorkingSetMember::OWNED_OBJ;
        member->keyData.clear();
        member->loc = DiskLoc();
        member->obj = bob.obj();
        return Status::OK();
    }

    ProjectionStage::~ProjectionStage() { }

    bool ProjectionStage::isEOF() { return _child->isEOF(); }

    PlanStage::StageState ProjectionStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        WorkingSetID id = WorkingSet::INVALID_ID;
        StageState status = _child->work(&id);

        // Note that we don't do the normal if isEOF() return EOF thing here.  Our child might be a
        // tailable cursor and isEOF() would be true even if it had more data...
        if (PlanStage::ADVANCED == status) {
            WorkingSetMember* member = _ws->get(id);
            // Punt to our specific projection impl.
            Status projStatus = transform(member);
            if (!projStatus.isOK()) {
                warning() << "Couldn't execute projection, status = "
                          << projStatus.toString() << endl;
                *out = WorkingSetCommon::allocateStatusMember(_ws, projStatus);
                return PlanStage::FAILURE;
            }

            *out = id;
            ++_commonStats.advanced;
        }
        else if (PlanStage::FAILURE == status) {
            *out = id;
            // If a stage fails, it may create a status WSM to indicate why it
            // failed, in which case 'id' is valid.  If ID is invalid, we
            // create our own error message.
            if (WorkingSet::INVALID_ID == id) {
                mongoutils::str::stream ss;
                ss << "projection stage failed to read in results from child";
                Status status(ErrorCodes::InternalError, ss);
                *out = WorkingSetCommon::allocateStatusMember( _ws, status);
            }
        }

        return status;
    }

    void ProjectionStage::saveState() {
        ++_commonStats.yields;
        _child->saveState();
    }

    void ProjectionStage::restoreState(OperationContext* opCtx) {
        ++_commonStats.unyields;
        _child->restoreState(opCtx);
    }

    void ProjectionStage::invalidate(const DiskLoc& dl, InvalidationType type) {
        ++_commonStats.invalidates;
        _child->invalidate(dl, type);
    }

    vector<PlanStage*> ProjectionStage::getChildren() const {
        vector<PlanStage*> children;
        children.push_back(_child.get());
        return children;
    }

    PlanStageStats* ProjectionStage::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_PROJECTION));

        ProjectionStats* projStats = new ProjectionStats(_specificStats);
        projStats->projObj = _projObj;
        ret->specific.reset(projStats);

        ret->children.push_back(_child->getStats());
        return ret.release();
    }

    const CommonStats* ProjectionStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* ProjectionStage::getSpecificStats() {
        return &_specificStats;
    }

}  // namespace mongo
