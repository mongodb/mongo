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

#include "mongo/db/storage/execution_control/throughput_probing.h"
#include "mongo/db/storage/execution_control/throughput_probing_gen.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::execution_control {

ThroughputProbing::ThroughputProbing(ServiceContext* svcCtx,
                                     TicketHolder* readTicketHolder,
                                     TicketHolder* writeTicketHolder,
                                     Milliseconds interval)
    : TicketHolderMonitor(svcCtx, readTicketHolder, writeTicketHolder, interval),
      _stableConcurrency(_readTicketHolder->outof()) {}

void ThroughputProbing::appendStats(BSONObjBuilder& builder) const {
    _stats.serialize(builder);
}

void ThroughputProbing::_run(Client* client) {
    auto numFinishedProcessing =
        _readTicketHolder->numFinishedProcessing() + _writeTicketHolder->numFinishedProcessing();
    invariant(numFinishedProcessing >= _prevNumFinishedProcessing);

    auto throughput = (numFinishedProcessing - _prevNumFinishedProcessing) /
        static_cast<double>(durationCount<Milliseconds>(_interval()));
    _stats.throughput.store(throughput);

    switch (_state) {
        case ProbingState::kStable:
            _probeStable(throughput);
            break;
        case ProbingState::kUp:
            _probeUp(throughput);
            break;
        case ProbingState::kDown:
            _probeDown(throughput);
            break;
    }

    _prevNumFinishedProcessing = numFinishedProcessing;
}

void ThroughputProbing::_probeStable(double throughput) {
    invariant(_state == ProbingState::kStable);

    LOGV2_DEBUG(7346000, 3, "Throughput Probing: stable", "throughput"_attr = throughput);

    // Record the baseline reading.
    _stableThroughput = throughput;

    auto outof = _readTicketHolder->outof();
    auto peakUsed = std::max(_readTicketHolder->getAndResetPeakUsed(),
                             _writeTicketHolder->getAndResetPeakUsed());
    if (outof < kMaxConcurrency && peakUsed >= outof) {
        // At least one of the ticket pools is exhausted, so try increasing concurrency.
        _state = ProbingState::kUp;
        _setConcurrency(
            std::lround(_stableConcurrency * (1 + throughput_probing::gStepMultiple.load())));
    } else if (_readTicketHolder->used() > kMinConcurrency ||
               _writeTicketHolder->used() > kMinConcurrency) {
        // Neither of the ticket pools are exhausted, so try decreasing concurrency to just below
        // the current level of usage.
        _state = ProbingState::kDown;
        _setConcurrency(std::lround(peakUsed * (1 - throughput_probing::gStepMultiple.load())));
    }
}

void ThroughputProbing::_probeUp(double throughput) {
    invariant(_state == ProbingState::kUp);

    LOGV2_DEBUG(7346001, 3, "Throughput Probing: up", "throughput"_attr = throughput);

    if (throughput > _stableThroughput) {
        // Increasing concurrency caused throughput to increase, so promote this new level of
        // concurrency to stable.
        auto concurrency = _readTicketHolder->outof();
        _stats.timesIncreased.fetchAndAdd(1);
        _stats.totalAmountIncreased.fetchAndAdd(concurrency - _stableConcurrency);
        _state = ProbingState::kStable;
        _stableThroughput = throughput;
        _stableConcurrency = concurrency;
    } else if (_readTicketHolder->outof() > kMinConcurrency) {
        // Increasing concurrency did not cause throughput to increase, so try decreasing
        // concurrency instead.
        _state = ProbingState::kDown;
        _setConcurrency(
            std::lround(_stableConcurrency * (1 - throughput_probing::gStepMultiple.load())));
    }
}

void ThroughputProbing::_probeDown(double throughput) {
    invariant(_state == ProbingState::kDown);

    LOGV2_DEBUG(7346002, 3, "Throughput Probing: down", "throughput"_attr = throughput);

    if (throughput > _stableThroughput) {
        // Decreasing concurrency caused throughput to increase, so promote this new level of
        // concurrency to stable.
        auto concurrency = _readTicketHolder->outof();
        _stats.timesDecreased.fetchAndAdd(1);
        _stats.totalAmountDecreased.fetchAndAdd(_stableConcurrency - concurrency);
        _state = ProbingState::kStable;
        _stableThroughput = throughput;
        _stableConcurrency = concurrency;
    } else {
        // Decreasing concurrency did not cause throughput to increase, so go back to stable and get
        // a new baseline to compare against.
        _state = ProbingState::kStable;
        _setConcurrency(_stableConcurrency);
    }
}

void ThroughputProbing::_setConcurrency(int32_t concurrency) {
    concurrency = std::clamp(concurrency, kMinConcurrency, kMaxConcurrency);
    _readTicketHolder->resize(concurrency);
    _writeTicketHolder->resize(concurrency);

    LOGV2_DEBUG(
        7346003, 3, "Throughput Probing: set concurrency", "concurrency"_attr = concurrency);
}

void ThroughputProbing::Stats::serialize(BSONObjBuilder& builder) const {
    builder.append("throughput", throughput.load());
    builder.append("timesDecreased", static_cast<long long>(timesDecreased.load()));
    builder.append("timesIncreased", static_cast<long long>(timesIncreased.load()));
    builder.append("totalAmountDecreased", static_cast<long long>(totalAmountDecreased.load()));
    builder.append("totalAmountIncreased", static_cast<long long>(totalAmountIncreased.load()));
}

}  // namespace mongo::execution_control
