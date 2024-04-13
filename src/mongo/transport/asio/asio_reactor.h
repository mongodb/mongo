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

#include <asio.hpp>
#include <memory>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/executor_stats.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"

namespace mongo::transport {

/**
 * AsioReactor is in charge of network event handling.
 */
class AsioReactor final : public Reactor {
public:
    AsioReactor();
    void run() noexcept override;
    void runFor(Milliseconds time) noexcept override;
    void stop() override;
    void drain() override;
    std::unique_ptr<ReactorTimer> makeTimer() override;
    Date_t now() override;
    void schedule(Task task) override;
    void dispatch(Task task) override;
    bool onReactorThread() const override;
    void appendStats(BSONObjBuilder& bob) const override;
    asio::io_context& getIoContext();

private:
    // Provides `ClockSource` API for the reactor's clock source.
    class ReactorClockSource final : public ClockSource {
    public:
        explicit ReactorClockSource(AsioReactor* reactor) : _reactor(reactor) {}

        Milliseconds getPrecision() override {
            MONGO_UNREACHABLE;
        }

        Date_t now() override {
            return _reactor->now();
        }

    private:
        AsioReactor* const _reactor;
    };

    ReactorClockSource _clkSource;
    ExecutorStats _stats;
    asio::io_context _ioContext;
    Atomic<bool> _closedForScheduling;
};

}  // namespace mongo::transport
