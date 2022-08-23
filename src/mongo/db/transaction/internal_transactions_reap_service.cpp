/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/transaction/internal_transactions_reap_service.h"

#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/internal_transactions_reap_service_gen.h"
#include "mongo/logv2/log.h"

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

    stdx::lock_guard lg(_mutex);
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
    ThreadClient tc("reap-internal-transactions", service);
    {
        stdx::lock_guard<Client> lk(*tc.get());
        tc->setSystemOperationKillableByStepdown(lk);
    }
    auto uniqueOpCtx = tc->makeOperationContext();
    auto opCtx = uniqueOpCtx.get();
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    std::vector<LogicalSessionId> lsidsToRemove;
    {
        using std::swap;
        stdx::lock_guard lg(_mutex);
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
