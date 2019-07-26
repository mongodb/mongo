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

#pragma once

#include "mongo/db/exec/requires_collection_stage.h"

namespace mongo {

/**
 * Implements "fast count" by asking the underlying RecordStore for its number of records, applying
 * the skip and limit it necessary. The result is stored in '_specificStats'. Only used to answer
 * count commands when both the query and hint are empty.
 */
class RecordStoreFastCountStage final : public RequiresCollectionStage {
public:
    static const char* kStageType;

    RecordStoreFastCountStage(OperationContext* opCtx,
                              Collection* collection,
                              long long skip,
                              long long limit);

    bool isEOF() override {
        return _commonStats.isEOF;
    }

    StageState doWork(WorkingSetID* out) override;

    StageType stageType() const override {
        return StageType::STAGE_RECORD_STORE_FAST_COUNT;
    }

    std::unique_ptr<PlanStageStats> getStats() override;

    const SpecificStats* getSpecificStats() const override {
        return &_specificStats;
    }

protected:
    void doSaveStateRequiresCollection() override {}

    void doRestoreStateRequiresCollection() override {}

private:
    long long _skip = 0;
    long long _limit = 0;

    CountStats _specificStats;
};

}  // namespace mongo
