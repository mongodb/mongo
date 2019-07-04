/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/shard_name.h"

#include <memory>

#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {

using std::shared_ptr;
using std::unique_ptr;
using std::vector;
using std::string;

// static
const char* ShardNameStage::kStageType = "SHARD_NAME";

ShardNameStage::ShardNameStage(OperationContext* opCtx,
                               ScopedCollectionMetadata metadata,
                               WorkingSet* ws,
                               std::unique_ptr<PlanStage> child)
    : PlanStage(kStageType, opCtx), _ws(ws), _shardNamer(std::move(metadata)) {
    _children.emplace_back(std::move(child));
}

ShardNameStage::~ShardNameStage() {}

bool ShardNameStage::isEOF() {
    return child()->isEOF();
}

PlanStage::StageState ShardNameStage::doWork(WorkingSetID* out) {
    // If we've returned as many results as we're limited to, isEOF will be true.
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    StageState status = child()->work(out);

    if (PlanStage::ADVANCED == status) {
        // If we're sharded make sure to add shardName to the output.
        if (_shardNamer.isCollectionSharded()) {
            WorkingSetMember* member = _ws->get(*out);
            const StringData shardName = _shardNamer.shardName();

            // Populate the working set member with the shard name and return it.
            member->metadata().setShardName(shardName);
        }

        // If we're here either we have shard state and added the shardName, or we have no shard
        // state.  Either way, we advance.
        return status;
    }

    return status;
}

unique_ptr<PlanStageStats> ShardNameStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, STAGE_SHARD_NAME);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* ShardNameStage::getSpecificStats() const {
    // No specific stats are tracked for the shard name stage.
    return nullptr;
}

}  // namespace mongo
