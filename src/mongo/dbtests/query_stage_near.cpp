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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
 * This file tests near search functionality.
 */


#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/exec/near.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;
using stdx::make_unique;

/**
 * Stage which takes in an array of BSONObjs and returns them.
 * If the BSONObj is in the form of a Status, returns the Status as a FAILURE.
 */
class MockStage : public PlanStage {
public:
    MockStage(const vector<BSONObj>& data, WorkingSet* workingSet)
        : _data(data), _pos(0), _workingSet(workingSet), _stats("MOCK_STAGE") {}

    virtual ~MockStage() {}

    virtual StageState work(WorkingSetID* out) {
        ++_stats.works;

        if (isEOF())
            return PlanStage::IS_EOF;

        BSONObj next = _data[_pos++];

        if (WorkingSetCommon::isValidStatusMemberObject(next)) {
            Status status = WorkingSetCommon::getMemberObjectStatus(next);
            *out = WorkingSetCommon::allocateStatusMember(_workingSet, status);
            return PlanStage::FAILURE;
        }

        *out = _workingSet->allocate();
        WorkingSetMember* member = _workingSet->get(*out);
        member->state = WorkingSetMember::OWNED_OBJ;
        member->obj = Snapshotted<BSONObj>(SnapshotId(), next);

        return PlanStage::ADVANCED;
    }

    virtual bool isEOF() {
        return _pos == static_cast<int>(_data.size());
    }

    virtual void saveState() {}

    virtual void restoreState(OperationContext* opCtx) {}

    virtual void invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {}
    virtual vector<PlanStage*> getChildren() const {
        return vector<PlanStage*>();
    }

    virtual StageType stageType() const {
        return STAGE_UNKNOWN;
    }

    virtual unique_ptr<PlanStageStats> getStats() {
        return make_unique<PlanStageStats>(_stats, STAGE_UNKNOWN);
    }

    virtual const CommonStats* getCommonStats() const {
        return &_stats;
    }

    virtual const SpecificStats* getSpecificStats() const {
        return NULL;
    }

private:
    vector<BSONObj> _data;
    int _pos;

    // Not owned here
    WorkingSet* const _workingSet;

    CommonStats _stats;
};

/**
 * Stage which implements a basic distance search, and interprets the "distance" field of
 * fetched documents as the distance.
 */
class MockNearStage : public NearStage {
public:
    struct MockInterval {
        MockInterval(const vector<BSONObj>& data, double min, double max)
            : data(data), min(min), max(max) {}

        vector<BSONObj> data;
        double min;
        double max;
    };

    MockNearStage(WorkingSet* workingSet)
        : NearStage(NULL,
                    workingSet,
                    NULL,
                    new PlanStageStats(CommonStats("MOCK_DISTANCE_SEARCH_STAGE"), STAGE_UNKNOWN)),
          _pos(0) {}

    virtual ~MockNearStage() {}

    void addInterval(vector<BSONObj> data, double min, double max) {
        _intervals.mutableVector().push_back(new MockInterval(data, min, max));
    }

    virtual StatusWith<CoveredInterval*> nextInterval(OperationContext* txn,
                                                      WorkingSet* workingSet,
                                                      Collection* collection) {
        if (_pos == static_cast<int>(_intervals.size()))
            return StatusWith<CoveredInterval*>(NULL);

        const MockInterval& interval = *_intervals.vector()[_pos++];

        bool lastInterval = _pos == static_cast<int>(_intervals.vector().size());
        return StatusWith<CoveredInterval*>(
            new CoveredInterval(new MockStage(interval.data, workingSet),
                                true,
                                interval.min,
                                interval.max,
                                lastInterval));
    }

    virtual StatusWith<double> computeDistance(WorkingSetMember* member) {
        ASSERT(member->hasObj());
        return StatusWith<double>(member->obj.value()["distance"].numberDouble());
    }

    virtual StageState initialize(OperationContext* txn,
                                  WorkingSet* workingSet,
                                  Collection* collection,
                                  WorkingSetID* out) {
        return IS_EOF;
    }

    virtual void finishSaveState() {}

    virtual void finishRestoreState(OperationContext* txn) {}

    virtual void finishInvalidate(OperationContext* txn,
                                  const RecordId& dl,
                                  InvalidationType type) {}

private:
    OwnedPointerVector<MockInterval> _intervals;
    int _pos;
};

static vector<BSONObj> advanceStage(PlanStage* stage, WorkingSet* workingSet) {
    vector<BSONObj> results;

    WorkingSetID nextMemberID;
    PlanStage::StageState state = PlanStage::NEED_TIME;

    while (PlanStage::NEED_TIME == state) {
        while (PlanStage::ADVANCED == (state = stage->work(&nextMemberID))) {
            results.push_back(workingSet->get(nextMemberID)->obj.value());
        }
    }

    return results;
}

static void assertAscendingAndValid(const vector<BSONObj>& results) {
    double lastDistance = -1.0;
    for (vector<BSONObj>::const_iterator it = results.begin(); it != results.end(); ++it) {
        double distance = (*it)["distance"].numberDouble();
        bool shouldInclude = (*it)["$included"].eoo() || (*it)["$included"].trueValue();
        ASSERT(shouldInclude);
        ASSERT_GREATER_THAN_OR_EQUALS(distance, lastDistance);
        lastDistance = distance;
    }
}

TEST(query_stage_near, Basic) {
    vector<BSONObj> mockData;
    WorkingSet workingSet;

    MockNearStage nearStage(&workingSet);

    // First set of results
    mockData.clear();
    mockData.push_back(BSON("distance" << 0.5));
    mockData.push_back(BSON("distance" << 2.0 << "$included" << false));  // Not included
    mockData.push_back(BSON("distance" << 0.0));
    nearStage.addInterval(mockData, 0.0, 1.0);

    // Second set of results
    mockData.clear();
    mockData.push_back(BSON("distance" << 1.5));
    mockData.push_back(BSON("distance" << 2.0 << "$included" << false));  // Not included
    mockData.push_back(BSON("distance" << 1.0));
    nearStage.addInterval(mockData, 1.0, 2.0);

    // Last set of results
    mockData.clear();
    mockData.push_back(BSON("distance" << 2.5));
    mockData.push_back(BSON("distance" << 3.0));  // Included
    mockData.push_back(BSON("distance" << 2.0));
    nearStage.addInterval(mockData, 2.0, 3.0);

    vector<BSONObj> results = advanceStage(&nearStage, &workingSet);
    ASSERT_EQUALS(results.size(), 7u);
    assertAscendingAndValid(results);
}

TEST(query_stage_near, EmptyResults) {
    vector<BSONObj> mockData;
    WorkingSet workingSet;

    MockNearStage nearStage(&workingSet);

    // Empty set of results
    mockData.clear();
    nearStage.addInterval(mockData, 0.0, 1.0);

    // Non-empty sest of results
    mockData.clear();
    mockData.push_back(BSON("distance" << 1.5));
    mockData.push_back(BSON("distance" << 2.0));
    mockData.push_back(BSON("distance" << 1.0));
    nearStage.addInterval(mockData, 1.0, 2.0);

    vector<BSONObj> results = advanceStage(&nearStage, &workingSet);
    ASSERT_EQUALS(results.size(), 3u);
    assertAscendingAndValid(results);
}
}
