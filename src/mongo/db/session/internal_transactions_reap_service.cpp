// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/session/internal_transactions_reap_service.h"

#include "mongo/db/client.h"
#include "mongo/db/session/internal_transactions_reap_service_gen.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"

#include <cstddef>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(pauseInternalTransactionsReaperAfterSwap);

const auto serviceDecoration = ServiceContext::declareDecoration<InternalTransactionsReapService>();

}  // namespace

InternalTransactionsReapService* InternalTransactionsReapService::get(
    ServiceContext* serviceContext) {
    return &serviceDecoration(serviceContext);
}

InternalTransactionsReapService* InternalTransactionsReapService::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

const ReplicaSetAwareServiceRegistry::Registerer<InternalTransactionsReapService>
    internalTransactionsReapServiceRegisterer("InternalTransactionsReapService");

InternalTransactionsReapService::InternalTransactionsReapService() {
    _threadPool = std::make_shared<ThreadPool>([] {
        ThreadPool::Options options;
        options.poolName = "InternalTransactionsReapService";
        options.minThreads = 0;
        options.maxThreads = 1;
        return options;
    }());
}

void InternalTransactionsReapService::onEagerlyReapedSessions(
    ServiceContext* service, std::vector<LogicalSessionId> lsidsToRemove) {
    InternalTransactionsReapService::get(service)->addEagerlyReapedSessions(
        service, std::move(lsidsToRemove));
}

void InternalTransactionsReapService::addEagerlyReapedSessions(
    ServiceContext* service, std::vector<LogicalSessionId> lsidsToRemove) {
    auto reapThreshold = internalSessionsReapThreshold.loadRelaxed();
    if (reapThreshold == 0) {
        // A threshold of 0 disables eager reaping.
        return;
    }

    std::lock_guard lg(_mutex);
    if (!_enabled) {
        return;
    }

    _lsidsToEagerlyReap.insert(
        _lsidsToEagerlyReap.end(), lsidsToRemove.begin(), lsidsToRemove.end());

    // reapThreshold is an integer, but is always greater than or equal to 0 so it
    // should be safe to cast to size_t.
    bool isAtThreshold = _lsidsToEagerlyReap.size() >= static_cast<size_t>(reapThreshold);
    bool isCurrentlyDrainingSessions = _drainedSessionsFuture && !_drainedSessionsFuture->isReady();

    if (isAtThreshold && !isCurrentlyDrainingSessions) {
        // Kick off reaping the buffer of internal transaction sessions.
        _drainedSessionsFuture.reset();
        _drainedSessionsFuture = ExecutorFuture<void>(_threadPool).then([this, service] {
            _reapInternalTransactions(service);
        });
    }
}

void InternalTransactionsReapService::onStartup(OperationContext* opCtx) {
    _threadPool->startup();
}

void InternalTransactionsReapService::onShutdown() {
    _threadPool->shutdown();
    _threadPool->join();
}

void InternalTransactionsReapService::_reapInternalTransactions(ServiceContext* service) try {
    ThreadClient tc("reap-internal-transactions", service->getService());
    auto uniqueOpCtx = tc->makeOperationContext();
    auto opCtx = uniqueOpCtx.get();
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    std::vector<LogicalSessionId> lsidsToRemove;
    {
        using std::swap;
        std::lock_guard lg(_mutex);
        swap(lsidsToRemove, _lsidsToEagerlyReap);
    }

    pauseInternalTransactionsReaperAfterSwap.pauseWhileSet(opCtx);

    LOGV2_DEBUG(6697300,
                2,
                "Eagerly reaping internal transactions from disk",
                "numToReap"_attr = lsidsToRemove.size());

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    auto numReaped = mongoDSessionCatalog->removeSessionsTransactionRecords(opCtx, lsidsToRemove);

    LOGV2_DEBUG(
        6697301, 2, "Eagerly reaped internal transactions from disk", "numReaped"_attr = numReaped);
} catch (const DBException& ex) {
    // Ignore errors.
    LOGV2(6697302,
          "Failed to eagerly reap internal transactions from disk",
          "error"_attr = redact(ex));
}

void InternalTransactionsReapService::waitForCurrentDrain_forTest() {
    if (_drainedSessionsFuture) {
        _drainedSessionsFuture->wait();
    }
    return;
}

bool InternalTransactionsReapService::hasCurrentDrain_forTest() {
    return _drainedSessionsFuture && !_drainedSessionsFuture->isReady();
}

}  // namespace mongo
