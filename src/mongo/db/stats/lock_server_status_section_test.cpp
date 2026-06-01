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

#include "mongo/db/admission/execution_control/execution_control_parameters_gen.h"
#include "mongo/db/admission/execution_control/ticketing_system.h"
#include "mongo/db/admission/ticketing/ticketholder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/utility/in_place_factory.hpp>

namespace mongo {
namespace {

namespace ec = admission::execution_control;

class UseReaderWriterGlobalThrottling {
public:
    UseReaderWriterGlobalThrottling(ServiceContext* svcCtx, int numTickets) : _svcCtx(svcCtx) {
        const bool trackPeakUsed = false;
        constexpr auto maxQueueDepth = TicketHolder::kDefaultMaxQueueDepth;
        auto ticketingSystem = std::make_unique<ec::TicketingSystem>(
            _svcCtx,
            ec::TicketingSystem::RWTicketHolder{
                std::make_unique<TicketHolder>(_svcCtx, numTickets, trackPeakUsed, maxQueueDepth),
                std::make_unique<TicketHolder>(_svcCtx, numTickets, trackPeakUsed, maxQueueDepth)},
            ec::TicketingSystem::RWTicketHolder{
                std::make_unique<TicketHolder>(_svcCtx, numTickets, trackPeakUsed, maxQueueDepth),
                std::make_unique<TicketHolder>(_svcCtx, numTickets, trackPeakUsed, maxQueueDepth)},
            ec::ExecutionControlConcurrencyAdjustmentAlgorithmEnum::kFixedConcurrentTransactions);
        ec::TicketingSystem::use(_svcCtx, std::move(ticketingSystem));
    }

    ~UseReaderWriterGlobalThrottling() {
        ec::TicketingSystem::use(_svcCtx, nullptr);
    }

private:
    ServiceContext* _svcCtx;
};

class GlobalLockServerStatusSectionTest : public ServiceContextTest {
protected:
    ServerStatusSection* findGlobalLockSection() {
        auto registry = ServerStatusSectionRegistry::instance();
        auto sectionIt = std::find_if(registry->begin(), registry->end(), [](const auto& entry) {
            return entry.second->getSectionName() == "globalLock";
        });

        return sectionIt == registry->end() ? nullptr : sectionIt->second.get();
    }

    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
    makeClientsWithOperationContexts(size_t numClients) {
        std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
            clients;
        clients.reserve(numClients);
        for (size_t i = 0; i < numClients; ++i) {
            auto client = getServiceContext()->getService()->makeClient(
                str::stream() << "global lock server status test client " << i);
            auto opCtx = client->makeOperationContext();
            clients.emplace_back(std::move(client), std::move(opCtx));
        }
        return clients;
    }

    void waitForClientState(OperationContext* opCtx, Locker::ClientState expectedState) {
        auto* locker = shard_role_details::getLocker(opCtx);
        for (int i = 0; i < 10000; ++i) {
            if (locker->getClientState() == expectedState) {
                return;
            }
            sleepmillis(1);
        }

        ASSERT_EQ(expectedState, locker->getClientState());
    }
};

TEST_F(GlobalLockServerStatusSectionTest, ReportsActiveAndQueuedReaderWriterCounts) {
    auto* section = findGlobalLockSection();
    ASSERT(section);
    UseReaderWriterGlobalThrottling throttle(getServiceContext(), 1);

    auto clients = makeClientsWithOperationContexts(4);
    auto* activeReaderOpCtx = clients[0].second.get();
    auto* activeWriterOpCtx = clients[1].second.get();
    auto* queuedReaderOpCtx = clients[2].second.get();
    auto* queuedWriterOpCtx = clients[3].second.get();

    boost::optional<Lock::GlobalLock> activeReaderLock(
        boost::in_place_init, activeReaderOpCtx, MODE_IS);
    ASSERT(activeReaderLock->isLocked());

    boost::optional<Lock::GlobalLock> activeWriterLock(
        boost::in_place_init, activeWriterOpCtx, MODE_IX);
    ASSERT(activeWriterLock->isLocked());

    unittest::JoinThread queuedReaderThread{[&] {
        Lock::GlobalLock queuedReaderLock(queuedReaderOpCtx, MODE_IS);
    }};

    unittest::JoinThread queuedWriterThread{[&] {
        Lock::GlobalLock queuedWriterLock(queuedWriterOpCtx, MODE_IX);
    }};

    ScopeGuard cleanup = [&] {
        activeReaderLock.reset();
        activeWriterLock.reset();
    };

    waitForClientState(queuedReaderOpCtx, Locker::kQueuedReader);
    waitForClientState(queuedWriterOpCtx, Locker::kQueuedWriter);

    auto statusOpCtx = makeOperationContext();
    const auto sectionObj = section->generateSection(statusOpCtx.get(), BSONElement{});

    ASSERT_GTE(sectionObj["totalTime"].numberLong(), 0);

    const auto currentQueue = sectionObj["currentQueue"].Obj();
    ASSERT_EQ(2, currentQueue["total"].numberInt());
    ASSERT_EQ(1, currentQueue["readers"].numberInt());
    ASSERT_EQ(1, currentQueue["writers"].numberInt());

    const auto activeClients = sectionObj["activeClients"].Obj();
    ASSERT_EQ(2, activeClients["total"].numberInt());
    ASSERT_EQ(1, activeClients["readers"].numberInt());
    ASSERT_EQ(1, activeClients["writers"].numberInt());
}

}  // namespace
}  // namespace mongo
