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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/eof.h"

namespace mongo {

    // static
    const char* EOFStage::kStageType = "EOF";

    EOFStage::EOFStage() : _commonStats(kStageType) { }

    EOFStage::~EOFStage() { }

    bool EOFStage::isEOF() {
        return true;
    }

    PlanStage::StageState EOFStage::work(WorkingSetID* out) {
        ++_commonStats.works;
        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);
        return PlanStage::IS_EOF;
    }

    void EOFStage::saveState() {
        ++_commonStats.yields;
    }

    void EOFStage::restoreState(OperationContext* opCtx) {
        ++_commonStats.unyields;
    }

    void EOFStage::invalidate(const DiskLoc& dl, InvalidationType type) {
        ++_commonStats.invalidates;
    }

    vector<PlanStage*> EOFStage::getChildren() const {
        vector<PlanStage*> empty;
        return empty;
    }

    PlanStageStats* EOFStage::getStats() {
        _commonStats.isEOF = isEOF();
        return new PlanStageStats(_commonStats, STAGE_EOF);
    }

    const CommonStats* EOFStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* EOFStage::getSpecificStats() {
        return NULL;
    }

}  // namespace mongo
