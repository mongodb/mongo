/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/admission/execution_control/ticketing_system.h"

#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/execution_control/execution_control_heuristic_parameters_gen.h"
#include "mongo/db/admission/execution_control/execution_control_parameters_gen.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/admission/ticketing/ticketholder.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mock_periodic_runner.h"
#include "mongo/util/tick_source_mock.h"

#include <memory>
#include <utility>

namespace mongo::admission::execution_control {
namespace {

using OperationType = execution_control::OperationType;

class TicketingSystemTest : public ServiceContextTest,
                            public testing::WithParamInterface<OperationType> {
protected:
    static constexpr int kTickets = 10;

    TicketingSystemTest()
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(ServiceContext::make(
                  nullptr, nullptr, std::make_unique<TickSourceMock<Microseconds>>()))) {
        _svcCtx->setPeriodicRunner(std::make_unique<MockPeriodicRunner>());
        _installTicketingSystem();
    }

    ~TicketingSystemTest() override {
        TicketingSystem::use(_svcCtx, nullptr);
    }

    OperationType opType() const {
        return GetParam();
    }

    TicketingSystem* ticketingSystem() {
        return TicketingSystem::get(_svcCtx);
    }

    std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
    makeClientAndOpCtx() {
        auto client = _svcCtx->getService()->makeClient("");
        auto opCtx = client->makeOperationContext();
        return {std::move(client), std::move(opCtx)};
    }

    void enableDeprioritization(bool gate, bool heuristic, bool backgroundTasks) {
        auto* ts = ticketingSystem();
        ASSERT_OK(ts->setHeuristicDeprioritization(heuristic));
        ASSERT_OK(ts->setBackgroundTasksDeprioritization(backgroundTasks));
        ASSERT_OK(ts->setDeprioritizationGate(gate));
    }

    TicketHolder* normalHolder() const {
        return opType() == OperationType::kRead ? _normalRead : _normalWrite;
    }

    TicketHolder* lowHolder() const {
        return opType() == OperationType::kRead ? _lowRead : _lowWrite;
    }

    void assertOnlyHolderUsed(TicketHolder* expected) {
        TicketHolder* holders[] = {_normalRead, _normalWrite, _lowRead, _lowWrite};
        for (auto* h : holders) {
            if (h == expected) {
                ASSERT_EQ(h->used(), 1) << "Expected holder should have 1 ticket used";
            } else {
                ASSERT_EQ(h->used(), 0) << "Non-target holder should have 0 tickets used";
            }
        }
    }

    void assertOnlyHolderUsedExempt(TicketHolder* expected) {
        TicketHolder* holders[] = {_normalRead, _normalWrite, _lowRead, _lowWrite};
        for (auto* h : holders) {
            BSONObjBuilder b;
            h->appendExemptStats(b);
            auto started = b.obj().getField("startedProcessing").Long();
            if (h == expected) {
                ASSERT_EQ(started, 1) << "Expected holder should have 1 exempt ticket issued";
            } else {
                ASSERT_EQ(started, 0) << "Non-target holder should have 0 exempt tickets issued";
            }
        }
    }

    ServiceContext* _svcCtx{getServiceContext()};
    TicketHolder* _normalRead = nullptr;
    TicketHolder* _normalWrite = nullptr;
    TicketHolder* _lowRead = nullptr;
    TicketHolder* _lowWrite = nullptr;

private:
    void _installTicketingSystem() {
        auto normalRead = std::make_unique<TicketHolder>(
            _svcCtx, kTickets, false /* trackPeakUsed */, TicketHolder::kDefaultMaxQueueDepth);
        auto normalWrite = std::make_unique<TicketHolder>(
            _svcCtx, kTickets, false /* trackPeakUsed */, TicketHolder::kDefaultMaxQueueDepth);
        auto lowRead = std::make_unique<TicketHolder>(
            _svcCtx, kTickets, false /* trackPeakUsed */, TicketHolder::kDefaultMaxQueueDepth);
        auto lowWrite = std::make_unique<TicketHolder>(
            _svcCtx, kTickets, false /* trackPeakUsed */, TicketHolder::kDefaultMaxQueueDepth);

        _normalRead = normalRead.get();
        _normalWrite = normalWrite.get();
        _lowRead = lowRead.get();
        _lowWrite = lowWrite.get();

        TicketingSystem::RWTicketHolder normal{std::move(normalRead), std::move(normalWrite)};
        TicketingSystem::RWTicketHolder low{std::move(lowRead), std::move(lowWrite)};

        TicketingSystem::use(
            _svcCtx,
            std::make_unique<TicketingSystem>(
                _svcCtx,
                std::move(normal),
                std::move(low),
                ExecutionControlConcurrencyAdjustmentAlgorithmEnum::kFixedConcurrentTransactions));
    }
};

INSTANTIATE_TEST_SUITE_P(ReadAndWrite,
                         TicketingSystemTest,
                         testing::Values(OperationType::kRead, OperationType::kWrite));

TEST_P(TicketingSystemTest, NormalPriorityUsesCorrectPool) {
    auto [client, opCtx] = makeClientAndOpCtx();

    auto ticket = ticketingSystem()->waitForTicketUntil(opCtx.get(), opType(), Date_t::max());
    ASSERT(ticket.has_value());

    assertOnlyHolderUsed(normalHolder());
}

TEST_P(TicketingSystemTest, LowPriorityFallsBackToNormalPoolWhenPrioritizationDisabled) {
    auto [client, opCtx] = makeClientAndOpCtx();

    ScopedAdmissionPriority<ExecutionAdmissionContext> lowPriority(
        opCtx.get(), AdmissionContext::Priority::kLow);

    auto ticket = ticketingSystem()->waitForTicketUntil(opCtx.get(), opType(), Date_t::max());
    ASSERT(ticket.has_value());

    assertOnlyHolderUsed(normalHolder());
}

TEST_P(TicketingSystemTest, LowPriorityUsesLowPoolWhenPrioritizationEnabled) {
    enableDeprioritization(true /* gate */, true /* heuristic */, false /* backgroundTasks */);
    auto [client, opCtx] = makeClientAndOpCtx();

    ScopedAdmissionPriority<ExecutionAdmissionContext> lowPriority(
        opCtx.get(), AdmissionContext::Priority::kLow);

    auto ticket = ticketingSystem()->waitForTicketUntil(opCtx.get(), opType(), Date_t::max());
    ASSERT(ticket.has_value());

    assertOnlyHolderUsed(lowHolder());
}

TEST_P(TicketingSystemTest, ExemptPriorityAlwaysUsesNormalPool) {
    enableDeprioritization(true /* gate */, true /* heuristic */, false /* backgroundTasks */);
    auto [client, opCtx] = makeClientAndOpCtx();

    ScopedAdmissionPriority<ExecutionAdmissionContext> exemptPriority(
        opCtx.get(), AdmissionContext::Priority::kExempt);
    ExecutionAdmissionContext::get(opCtx.get()).setAdmission_forTest(100);

    auto ticket = ticketingSystem()->waitForTicketUntil(opCtx.get(), opType(), Date_t::max());
    ASSERT(ticket.has_value());

    // Exempt priority bypasses the semaphore, so used() won't reflect the acquisition.
    // Verify via exempt stats that the ticket was issued by the correct normal holder.
    assertOnlyHolderUsedExempt(normalHolder());
}

TEST_P(TicketingSystemTest, HeuristicDeprioritizesHighAdmissionsToLowPool) {
    enableDeprioritization(true /* gate */, true /* heuristic */, false /* backgroundTasks */);
    auto [client, opCtx] = makeClientAndOpCtx();

    const auto threshold = gHeuristicNumAdmissionsDeprioritizeThreshold.load();
    ExecutionAdmissionContext::get(opCtx.get()).setAdmission_forTest(threshold);

    auto ticket = ticketingSystem()->waitForTicketUntil(opCtx.get(), opType(), Date_t::max());
    ASSERT(ticket.has_value());

    assertOnlyHolderUsed(lowHolder());
}

TEST_P(TicketingSystemTest, HeuristicDoesNotDeprioritizeBelowThreshold) {
    enableDeprioritization(true /* gate */, true /* heuristic */, false /* backgroundTasks */);
    auto [client, opCtx] = makeClientAndOpCtx();

    ExecutionAdmissionContext::get(opCtx.get()).setAdmission_forTest(1);

    auto ticket = ticketingSystem()->waitForTicketUntil(opCtx.get(), opType(), Date_t::max());
    ASSERT(ticket.has_value());

    assertOnlyHolderUsed(normalHolder());
}

TEST_P(TicketingSystemTest, HeuristicDisabledDoesNotDeprioritizeHighAdmissions) {
    // backgroundTasks on so usesPrioritization() = true, but heuristic off.
    enableDeprioritization(true /* gate */, false /* heuristic */, true /* backgroundTasks */);
    auto [client, opCtx] = makeClientAndOpCtx();

    ExecutionAdmissionContext::get(opCtx.get()).setAdmission_forTest(100);

    auto ticket = ticketingSystem()->waitForTicketUntil(opCtx.get(), opType(), Date_t::max());
    ASSERT(ticket.has_value());

    assertOnlyHolderUsed(normalHolder());
}

TEST_P(TicketingSystemTest, BackgroundTaskDeprioritizedToLowPool) {
    enableDeprioritization(true /* gate */, false /* heuristic */, true /* backgroundTasks */);
    auto [client, opCtx] = makeClientAndOpCtx();

    ScopedTaskTypeBackground backgroundTask(opCtx.get());

    auto ticket = ticketingSystem()->waitForTicketUntil(opCtx.get(), opType(), Date_t::max());
    ASSERT(ticket.has_value());

    assertOnlyHolderUsed(lowHolder());
}

TEST_P(TicketingSystemTest, BackgroundTaskNotDeprioritizedWhenFlagDisabled) {
    // heuristic on so usesPrioritization() = true, but backgroundTasks off.
    enableDeprioritization(true /* gate */, true /* heuristic */, false /* backgroundTasks */);
    auto [client, opCtx] = makeClientAndOpCtx();

    ScopedTaskTypeBackground backgroundTask(opCtx.get());
    ExecutionAdmissionContext::get(opCtx.get()).setAdmission_forTest(100);

    auto ticket = ticketingSystem()->waitForTicketUntil(opCtx.get(), opType(), Date_t::max());
    ASSERT(ticket.has_value());

    assertOnlyHolderUsed(normalHolder());
}

TEST_P(TicketingSystemTest, NonDeprioritizableTaskUsesNormalPool) {
    enableDeprioritization(true /* gate */, true /* heuristic */, false /* backgroundTasks */);
    auto [client, opCtx] = makeClientAndOpCtx();

    ScopedTaskTypeNonDeprioritizable nonDeprioritizable(opCtx.get());
    ExecutionAdmissionContext::get(opCtx.get()).setAdmission_forTest(100);

    auto ticket = ticketingSystem()->waitForTicketUntil(opCtx.get(), opType(), Date_t::max());
    ASSERT(ticket.has_value());

    assertOnlyHolderUsed(normalHolder());
}

TEST_P(TicketingSystemTest, MultiDocTransactionNotDeprioritized) {
    enableDeprioritization(true /* gate */, true /* heuristic */, false /* backgroundTasks */);
    auto [client, opCtx] = makeClientAndOpCtx();

    opCtx->setInMultiDocumentTransaction();
    ExecutionAdmissionContext::get(opCtx.get()).setAdmission_forTest(100);

    auto ticket = ticketingSystem()->waitForTicketUntil(opCtx.get(), opType(), Date_t::max());
    ASSERT(ticket.has_value());

    assertOnlyHolderUsed(normalHolder());
}

TEST_P(TicketingSystemTest, GateClosedDisablesAllDeprioritization) {
    // Gate off means usesPrioritization() = false regardless of heuristic/backgroundTasks.
    enableDeprioritization(false /* gate */, true /* heuristic */, true /* backgroundTasks */);
    auto [client, opCtx] = makeClientAndOpCtx();

    ExecutionAdmissionContext::get(opCtx.get()).setAdmission_forTest(100);

    auto ticket = ticketingSystem()->waitForTicketUntil(opCtx.get(), opType(), Date_t::max());
    ASSERT(ticket.has_value());

    assertOnlyHolderUsed(normalHolder());
}

TEST_P(TicketingSystemTest, GateClosedFallsBackLowToNormal) {
    auto [client, opCtx] = makeClientAndOpCtx();

    ScopedAdmissionPriority<ExecutionAdmissionContext> lowPriority(
        opCtx.get(), AdmissionContext::Priority::kLow);

    auto ticket = ticketingSystem()->waitForTicketUntil(opCtx.get(), opType(), Date_t::max());
    ASSERT(ticket.has_value());

    assertOnlyHolderUsed(normalHolder());
}

}  // namespace
}  // namespace mongo::admission::execution_control
