/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/storage/ticketholder_monitor.h"

namespace mongo::execution_control {
namespace throughput_probing {

Status validateInitialConcurrency(int32_t concurrency, const boost::optional<TenantId>&);
Status validateMinConcurrency(int32_t concurrency, const boost::optional<TenantId>&);
Status validateMaxConcurrency(int32_t concurrency, const boost::optional<TenantId>&);

}  // namespace throughput_probing

/**
 * Adjusts the level of concurrency on the read and write ticket holders by probing up/down and
 * attempting to maximize throughput. Assumes both ticket holders have the same starting concurrency
 * level and always keeps the same concurrency level for both.
 */
class ThroughputProbing : public TicketHolderMonitor {
public:
    ThroughputProbing(ServiceContext* svcCtx,
                      TicketHolder* readTicketHolder,
                      TicketHolder* writeTicketHolder,
                      Milliseconds interval);

    virtual void appendStats(BSONObjBuilder& builder) const override;

private:
    enum class ProbingState {
        kStable,
        kUp,
        kDown,
    };

    void _run(Client*) override;

    void _probeStable(double throughput);
    void _probeUp(double throughput);
    void _probeDown(double throughput);

    void _setConcurrency(int32_t concurrency);

    int32_t _stableConcurrency;
    double _stableThroughput = 0;
    ProbingState _state = ProbingState::kStable;
    Timer _timer;

    int64_t _prevNumFinishedProcessing = -1;

    struct Stats {
        void serialize(BSONObjBuilder& builder) const;

        AtomicWord<int64_t> opsPerSec;
        AtomicWord<int64_t> timesDecreased;
        AtomicWord<int64_t> timesIncreased;
        AtomicWord<int64_t> totalAmountDecreased;
        AtomicWord<int64_t> totalAmountIncreased;
    } _stats;
};

}  // namespace mongo::execution_control
