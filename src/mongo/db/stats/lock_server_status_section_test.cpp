// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

    waitForClientState(queuedReaderOpCtx, Locker::ClientState::kQueuedReader);
    waitForClientState(queuedWriterOpCtx, Locker::ClientState::kQueuedWriter);

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
