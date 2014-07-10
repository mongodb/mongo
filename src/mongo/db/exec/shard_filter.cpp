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

#include "mongo/db/exec/shard_filter.h"

#include "mongo/db/keypattern.h"

namespace mongo {

    // static
    const char* ShardFilterStage::kStageType = "SHARDING_FILTER";

    ShardFilterStage::ShardFilterStage(const CollectionMetadataPtr& metadata,
                                       WorkingSet* ws,
                                       PlanStage* child)
        : _ws(ws), _child(child), _commonStats(kStageType), _metadata(metadata) { }

    ShardFilterStage::~ShardFilterStage() { }

    bool ShardFilterStage::isEOF() { return _child->isEOF(); }

    PlanStage::StageState ShardFilterStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        // If we've returned as many results as we're limited to, isEOF will be true.
        if (isEOF()) { return PlanStage::IS_EOF; }

        StageState status = _child->work(out);

        if (PlanStage::ADVANCED == status) {
            // If we're sharded make sure that we don't return any data that hasn't been migrated
            // off of our shared yet.
            if (_metadata) {
                KeyPattern kp(_metadata->getKeyPattern());

                WorkingSetMember* member = _ws->get(*out);

                // This performs excessive BSONObj creation but that's OK for now.
                if (!_metadata->keyBelongsToMe(kp.extractSingleKey(member->obj))) {
                    _ws->free(*out);
                    ++_specificStats.chunkSkips;
                    return PlanStage::NEED_TIME;
                }
            }

            // If we're here either we have shard state and our doc passed, or we have no shard
            // state.  Either way, we advance.
            ++_commonStats.advanced;
            return status;
        }
        else {
            if (PlanStage::NEED_TIME == status) {
                ++_commonStats.needTime;
            }
            return status;
        }
    }

    void ShardFilterStage::prepareToYield() {
        ++_commonStats.yields;
        _child->prepareToYield();
    }

    void ShardFilterStage::recoverFromYield() {
        ++_commonStats.unyields;
        _child->recoverFromYield();
    }

    void ShardFilterStage::invalidate(const DiskLoc& dl, InvalidationType type) {
        ++_commonStats.invalidates;
        _child->invalidate(dl, type);
    }

    vector<PlanStage*> ShardFilterStage::getChildren() const {
        vector<PlanStage*> children;
        children.push_back(_child.get());
        return children;
    }

    PlanStageStats* ShardFilterStage::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_SHARDING_FILTER));
        ret->children.push_back(_child->getStats());
        ret->specific.reset(new ShardingFilterStats(_specificStats));
        return ret.release();
    }

    const CommonStats* ShardFilterStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* ShardFilterStage::getSpecificStats() {
        return &_specificStats;
    }

}  // namespace mongo
