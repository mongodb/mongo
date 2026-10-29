// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


// IWYU pragma: no_include "cxxabi.h"
#include "mongo/db/storage/storage_engine_change_context.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <mutex>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {

const ServiceContext::Decoration<StorageEngineChangeContext> getStorageEngineChangeContext =
    ServiceContext::declareDecoration<StorageEngineChangeContext>();

struct StorageEngineChangeOperationContextDoneNotifier {
    void operator()(ServiceContext* service) {
        if (service) {
            auto* changeContext = StorageEngineChangeContext::get(service);
            changeContext->notifyOpCtxDestroyed();
        }
    }
};

// Using unique_ptr here instead of a custom type takes advantage
// of decoration construction and destruction optimizations.
using NotifyDecorationType =
    std::unique_ptr<ServiceContext, StorageEngineChangeOperationContextDoneNotifier>;
const auto notifyDecoration = OperationContext::declareDecoration<NotifyDecorationType>();

void setNotifyWhenDone(OperationContext* opCtx, ServiceContext* service) {
    auto& deco = notifyDecoration(opCtx);
    invariant(!deco);
    deco.reset(service);
}

}  // namespace

/* static */
StorageEngineChangeContext* StorageEngineChangeContext::get(ServiceContext* service) {
    return &getStorageEngineChangeContext(service);
}

WriteRarelyRWMutex::WriteLock StorageEngineChangeContext::killOpsForStorageEngineChange(
    ServiceContext* service) {
    invariant(this == StorageEngineChangeContext::get(service));
    // Prevent new operations from being created.
    auto storageChangeLk = service->getStorageChangeMutex().writeLock();
    std::unique_lock lk(_mutex);
    {
        ServiceContext::LockedClientsCursor clientCursor(service);
        // Interrupt all active operations with a real recovery unit.
        while (auto* client = clientCursor.next()) {
            if (client == Client::getCurrent())
                continue;
            OperationId killedOperationId;
            {
                ClientLock lk(client);
                auto opCtxToKill = client->getOperationContext();
                if (!opCtxToKill || !shard_role_details::getRecoveryUnit(opCtxToKill) ||
                    shard_role_details::getRecoveryUnit(opCtxToKill)->isNoop())
                    continue;
                service->killOperation(lk, opCtxToKill, ErrorCodes::InterruptedDueToStorageChange);
                setNotifyWhenDone(opCtxToKill, service);
                ++_numOpCtxtsToWaitFor;
                killedOperationId = opCtxToKill->getOpID();
            }
            LOGV2_DEBUG(5781190,
                        1,
                        "Killed OpCtx for storage change",
                        "killedOperationId"_attr = killedOperationId,
                        "client"_attr = client->desc());
        }
    }

    // Wait for active operation contexts to be released.
    _allOldStorageOperationContextsReleased.wait(lk, [&] { return _numOpCtxtsToWaitFor == 0; });
    // Free the old storage engine.
    service->clearStorageEngine();
    return storageChangeLk;
}

void StorageEngineChangeContext::changeStorageEngine(ServiceContext* service,
                                                     WriteRarelyRWMutex::WriteLock lk,
                                                     std::unique_ptr<StorageEngine> engine) {
    invariant(this == StorageEngineChangeContext::get(service));
    service->setStorageEngine(std::move(engine));
    // The lock is released at end of scope, allowing OperationContexts to be created again.
}

void StorageEngineChangeContext::notifyOpCtxDestroyed() noexcept {
    std::unique_lock lk(_mutex);
    invariant(--_numOpCtxtsToWaitFor >= 0);
    LOGV2_DEBUG(5781191,
                1,
                "An OpCtx with old storage was destroyed",
                "numOpCtxtsToWaitFor"_attr = _numOpCtxtsToWaitFor);
    if (_numOpCtxtsToWaitFor == 0)
        _allOldStorageOperationContextsReleased.notify_one();
}
}  // namespace mongo
