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

#include <cstdint>

#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/ticketholder_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace admission {
namespace throughput_probing {

Status validateInitialConcurrency(int32_t concurrency, const boost::optional<TenantId>&);
Status validateMinConcurrency(int32_t concurrency, const boost::optional<TenantId>&);
Status validateMaxConcurrency(int32_t concurrency, const boost::optional<TenantId>&);

}  // namespace throughput_probing

/**
 * Adjusts the level of concurrency on the read and write ticket holders by probing up/down and
 * attempting to maximize throughput. Assumes both ticket holders have the same starting
 * concurrency level and always keeps the same concurrency level for both.
 */
class ThroughputProbing {
public:
    ThroughputProbing(ServiceContext* svcCtx,
                      TicketHolder* readTicketHolder,
                      TicketHolder* writeTicketHolder,
                      Milliseconds interval);

    void appendStats(BSONObjBuilder& builder) const;

    void start();

private:
    enum class ProbingState {
        kStable,
        kUp,
        kDown,
    };

    void _run(Client*);

    void _probeStable(double throughput);
    void _probeUp(double throughput);
    void _probeDown(double throughput);

    void _resetConcurrency();
    void _increaseConcurrency();
    void _decreaseConcurrency();

    void _resize(TicketHolder* ticketholder, int newTickets);

    TicketHolder* _readTicketHolder;
    TicketHolder* _writeTicketHolder;

    // This value is split between reads and writes based on the read/write ratio.
    double _stableConcurrency;
    double _stableThroughput = 0;
    ProbingState _state = ProbingState::kStable;
    Timer _timer;

    int64_t _prevNumFinishedProcessing = -1;

    struct Stats {
        void serialize(BSONObjBuilder& builder) const;

        AtomicWord<int64_t> timesDecreased;
        AtomicWord<int64_t> timesIncreased;
        AtomicWord<int64_t> totalAmountDecreased;
        AtomicWord<int64_t> totalAmountIncreased;
        AtomicWord<int64_t> resizeDurationMicros;
    } _stats;

    PeriodicJobAnchor _job;
};

class ThroughputProbingTicketHolderManager : public TicketHolderManager {
public:
    ThroughputProbingTicketHolderManager(ServiceContext* svcCtx,
                                         std::unique_ptr<TicketHolder> readTicketHolder,
                                         std::unique_ptr<TicketHolder> writeTicketHolder,
                                         Milliseconds interval);
    virtual bool supportsRuntimeSizeAdjustment() const override {
        return false;
    }

protected:
    virtual void _appendImplStats(BSONObjBuilder& builder) const override;

private:
    /**
     * Task which adjusts the number of concurrent read/write transactions.
     */
    std::unique_ptr<admission::ThroughputProbing> _monitor;
};
}  // namespace admission
}  // namespace mongo
