/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/util/concurrency/admission_context.h"

namespace mongo {

class OperationContext;

/**
 * Stores state and statistics related to execution control for a given transactional context.
 */
class ExecutionAdmissionContext : public AdmissionContext {
public:
    struct DelinquencyStats {
        DelinquencyStats() = default;
        DelinquencyStats(const DelinquencyStats& other);
        DelinquencyStats& operator=(const DelinquencyStats& other);

        AtomicWord<int64_t> delinquentAcquisitions{0};
        AtomicWord<int64_t> totalAcquisitionDelinquencyMillis{0};
        AtomicWord<int64_t> maxAcquisitionDelinquencyMillis{0};
    };

    ExecutionAdmissionContext() = default;
    ExecutionAdmissionContext(const ExecutionAdmissionContext& other);
    ExecutionAdmissionContext& operator=(const ExecutionAdmissionContext& other);

    int64_t getDelinquentAcquisitions() const {
        return _readDelinquencyStats.delinquentAcquisitions.loadRelaxed() +
            _writeDelinquencyStats.delinquentAcquisitions.loadRelaxed();
    }

    int64_t getTotalAcquisitionDelinquencyMillis() const {
        return _readDelinquencyStats.totalAcquisitionDelinquencyMillis.loadRelaxed() +
            _writeDelinquencyStats.totalAcquisitionDelinquencyMillis.loadRelaxed();
    }

    int64_t getMaxAcquisitionDelinquencyMillis() const {
        return std::max(_readDelinquencyStats.maxAcquisitionDelinquencyMillis.loadRelaxed(),
                        _writeDelinquencyStats.maxAcquisitionDelinquencyMillis.loadRelaxed());
    }

    /**
     * Indicates that a read or write ticket was held for 'delay' milliseconds past due.
     */
    void recordDelinquentReadAcquisition(Milliseconds delay) {
        recordDelinquentAcquisition(delay, _readDelinquencyStats);
    }
    void recordDelinquentWriteAcquisition(Milliseconds delay) {
        recordDelinquentAcquisition(delay, _writeDelinquencyStats);
    }

    /**
     * Getters for stats related to delinquency in acquiring read and write tickets.
     */
    const DelinquencyStats& readDelinquencyStats() const {
        return _readDelinquencyStats;
    }
    const DelinquencyStats& writeDelinquencyStats() const {
        return _writeDelinquencyStats;
    }

    /**
     * Retrieve the ExecutionAdmissionContext decoration the provided OperationContext
     */
    static ExecutionAdmissionContext& get(OperationContext* opCtx);

private:
    void recordDelinquentAcquisition(Milliseconds delay, DelinquencyStats& stats) {
        const int64_t delayMs = delay.count();
        stats.delinquentAcquisitions.fetchAndAddRelaxed(1);
        stats.totalAcquisitionDelinquencyMillis.fetchAndAddRelaxed(delayMs);
        stats.maxAcquisitionDelinquencyMillis.storeRelaxed(
            std::max(stats.maxAcquisitionDelinquencyMillis.loadRelaxed(), delayMs));
    }

    DelinquencyStats _readDelinquencyStats;
    DelinquencyStats _writeDelinquencyStats;
};

}  // namespace mongo
