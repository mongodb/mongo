/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/cost_model/cost_model_manager.h"

#include "mongo/db/query/cost_model/cost_model_on_update.h"

namespace mongo::cost_model {
namespace {
/**
 * Populate given cost model coefficients object with default values.
 */
void initializeCoefficients(CostModelCoefficients& coefficients) {
    // These cost should reflect estimated aggregated execution time in milliseconds.
    // The coeffeicient ns converts values from nanoseconds to milliseconds.
    constexpr double nsToMs = 1.0e-6;

    coefficients.setDefaultStartupCost(1.0 * nsToMs);

    coefficients.setScanIncrementalCost(422.31145989 * nsToMs);
    coefficients.setScanStartupCost(6175.527218993269 * nsToMs);

    coefficients.setIndexScanIncrementalCost(403.68075869 * nsToMs);
    coefficients.setIndexScanStartupCost(14054.983953111061 * nsToMs);

    coefficients.setSeekCost(1174.84136356 * nsToMs);
    coefficients.setSeekStartupCost(7488.662376624863 * nsToMs);

    coefficients.setFilterIncrementalCost(83.7274685 * nsToMs);
    coefficients.setFilterStartupCost(1461.3148783443378 * nsToMs);

    coefficients.setEvalIncrementalCost(430.6176946 * nsToMs);
    coefficients.setEvalStartupCost(1103.4048573163343 * nsToMs);

    coefficients.setGroupByIncrementalCost(413.07932374 * nsToMs);
    coefficients.setGroupByStartupCost(1199.8878012735659 * nsToMs);

    coefficients.setUnwindIncrementalCost(586.57200195 * nsToMs);
    coefficients.setUnwindStartupCost(1.0 * nsToMs);

    coefficients.setNestedLoopJoinIncrementalCost(161.62301944 * nsToMs);
    coefficients.setNestedLoopJoinStartupCost(402.8455479458652 * nsToMs);

    coefficients.setHashJoinIncrementalCost(250.61365634 * nsToMs);
    coefficients.setHashJoinStartupCost(1.0 * nsToMs);  // Already calibrated.

    coefficients.setMergeJoinIncrementalCost(111.23423304 * nsToMs);
    coefficients.setMergeJoinStartupCost(1517.7970800404169 * nsToMs);

    coefficients.setUniqueIncrementalCost(269.71368614 * nsToMs);
    coefficients.setUniqueStartupCost(1.0 * nsToMs);  // Already calibrated.

    coefficients.setCollationIncrementalCost(2500 * nsToMs);  // TODO: not yet calibrated
    coefficients.setCollationStartupCost(1.0 * nsToMs);       // TODO: not yet calibrated

    coefficients.setCollationWithLimitIncrementalCost(1000 * nsToMs);  // TODO: not yet calibrated
    coefficients.setCollationWithLimitStartupCost(1.0 * nsToMs);       // TODO: not yet calibrated

    coefficients.setUnionIncrementalCost(111.94945268 * nsToMs);
    coefficients.setUnionStartupCost(69.88096657391543 * nsToMs);

    coefficients.setExchangeIncrementalCost(100 * nsToMs);  // TODO: not yet calibrated
    coefficients.setExchangeStartupCost(1.0 * nsToMs);      // TODO: not yet calibrated

    coefficients.setLimitSkipIncrementalCost(62.42111111 * nsToMs);
    coefficients.setLimitSkipStartupCost(655.1342592592522 * nsToMs);

    coefficients.setSortedMergeIncrementalCost(0.2 * nsToMs);   // TODO: not yet calibrated
    coefficients.setSortedMergeStartupCost(0.000001 * nsToMs);  // TODO: not yet calibrated
}
}  // namespace

CostModelManager::CostModelManager() {
    initializeCoefficients(_coefficients);
}

CostModelCoefficients CostModelManager::getCoefficients() const {
    std::shared_lock rLock(_mutex);  // NOLINT

    return _coefficients;
}

CostModelCoefficients CostModelManager::getDefaultCoefficients() {
    CostModelCoefficients defaultCoefs;

    initializeCoefficients(defaultCoefs);

    return defaultCoefs;
}

void CostModelManager::updateCostModelCoefficients(const BSONObj& overrides) {
    CostModelCoefficients newCoefs;
    if (overrides.isEmpty()) {
        initializeCoefficients(newCoefs);
    } else {
        auto coefsObj = _coefficients.toBSON();
        newCoefs = CostModelCoefficients::parse(IDLParserContext{"CostModelCoefficients"},
                                                coefsObj.addFields(overrides));
    }

    stdx::unique_lock wLock(_mutex);
    _coefficients = std::move(newCoefs);
}
}  // namespace mongo::cost_model
