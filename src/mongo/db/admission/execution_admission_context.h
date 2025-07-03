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
    ExecutionAdmissionContext() = default;
    ExecutionAdmissionContext(const ExecutionAdmissionContext& other);
    ExecutionAdmissionContext& operator=(const ExecutionAdmissionContext& other);

    int32_t getDelinquentAcquisitions() const {
        return _delinquentAcquisitions.loadRelaxed();
    }

    int64_t getTotalAcquisitionDelinquencyMillis() const {
        return _totalAcquisitionDelinquencyMillis.loadRelaxed();
    }

    int64_t getMaxAcquisitionDelinquencyMillis() const {
        return _maxAcquisitionDelinquencyMillis.loadRelaxed();
    }

    void recordDelinquentAcquisition(int64_t delay) {
        _delinquentAcquisitions.fetchAndAddRelaxed(1);
        _totalAcquisitionDelinquencyMillis.fetchAndAddRelaxed(delay);
        _maxAcquisitionDelinquencyMillis.storeRelaxed(
            std::max(_maxAcquisitionDelinquencyMillis.loadRelaxed(), delay));
    }

    /**
     * Retrieve the ExecutionAdmissionContext decoration the provided OperationContext
     */
    static ExecutionAdmissionContext& get(OperationContext* opCtx);

private:
    AtomicWord<int32_t> _delinquentAcquisitions{0};
    AtomicWord<int64_t> _totalAcquisitionDelinquencyMillis{0};
    AtomicWord<int64_t> _maxAcquisitionDelinquencyMillis{0};
};

}  // namespace mongo
