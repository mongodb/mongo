/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/storage_engine_change_context.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
const ServiceContext::Decoration<StorageEngineChangeContext> getStorageEngineChangeContext =
    ServiceContext::declareDecoration<StorageEngineChangeContext>();
}  // namespace

/* static */
StorageEngineChangeContext* StorageEngineChangeContext::get(ServiceContext* service) {
    return &getStorageEngineChangeContext(service);
}

class StorageEngineChangeOperationContextDoneNotifier {
public:
    static const OperationContext::Decoration<StorageEngineChangeOperationContextDoneNotifier> get;

    StorageEngineChangeOperationContextDoneNotifier() = default;
    ~StorageEngineChangeOperationContextDoneNotifier();

    /*
     * The 'setNotifyWhenDone' method makes this decoration notify the
     * StorageEngineChangeContext for the associated service context when it is destroyed.
     * It must be called under the client lock for the decorated OperationContext.
     */
    void setNotifyWhenDone(ServiceContext* service);

private:
    ServiceContext* _service = nullptr;
};

/* static */
const OperationContext::Decoration<StorageEngineChangeOperationContextDoneNotifier>
    StorageEngineChangeOperationContextDoneNotifier::get =
        OperationContext::declareDecoration<StorageEngineChangeOperationContextDoneNotifier>();

StorageEngineChangeOperationContextDoneNotifier::
    ~StorageEngineChangeOperationContextDoneNotifier() {
    if (_service) {
        auto* changeContext = StorageEngineChangeContext::get(_service);
        changeContext->notifyOpCtxDestroyed();
    }
}

void StorageEngineChangeOperationContextDoneNotifier::setNotifyWhenDone(ServiceContext* service) {
    invariant(!_service);
    invariant(service);
    _service = service;
}

StorageChangeLock::Token StorageEngineChangeContext::killOpsForStorageEngineChange(
    ServiceContext* service) {
    invariant(this == StorageEngineChangeContext::get(service));
    // Prevent new operations from being created.
    auto storageChangeLk = service->getStorageChangeLock().acquireExclusiveStorageChangeToken();
    stdx::unique_lock lk(_mutex);
    {
        ServiceContext::LockedClientsCursor clientCursor(service);
        // Interrupt all active operations with a real recovery unit.
        while (auto* client = clientCursor.next()) {
            if (client == Client::getCurrent())
                continue;
            OperationId killedOperationId;
            {
                stdx::lock_guard<Client> lk(*client);
                auto opCtxToKill = client->getOperationContext();
                if (!opCtxToKill || !opCtxToKill->recoveryUnit() ||
                    opCtxToKill->recoveryUnit()->isNoop())
                    continue;
                service->killOperation(lk, opCtxToKill, ErrorCodes::InterruptedDueToStorageChange);
                auto& doneNotifier =
                    StorageEngineChangeOperationContextDoneNotifier::get(opCtxToKill);
                doneNotifier.setNotifyWhenDone(service);
                ++_numOpCtxtsToWaitFor;
            }
            LOGV2_DEBUG(5781190,
                        1,
                        "Killed OpCtx for storage change",
                        "killedOperationId"_attr = killedOperationId);
        }
    }

    // Wait for active operation contexts to be released.
    _allOldStorageOperationContextsReleased.wait(lk, [&] { return _numOpCtxtsToWaitFor == 0; });
    // Free the old storage engine.
    service->clearStorageEngine();
    return storageChangeLk;
}

void StorageEngineChangeContext::changeStorageEngine(ServiceContext* service,
                                                     StorageChangeLock::Token token,
                                                     std::unique_ptr<StorageEngine> engine) {
    invariant(this == StorageEngineChangeContext::get(service));
    service->setStorageEngine(std::move(engine));
    // Token -- which is a lock -- is released at end of scope, allowing OperationContexts to be
    // created again.
}

void StorageEngineChangeContext::notifyOpCtxDestroyed() noexcept {
    stdx::unique_lock lk(_mutex);
    invariant(--_numOpCtxtsToWaitFor >= 0);
    LOGV2_DEBUG(5781191,
                1,
                "An OpCtx with old storage was destroyed",
                "numOpCtxtsToWaitFor"_attr = _numOpCtxtsToWaitFor);
    if (_numOpCtxtsToWaitFor == 0)
        _allOldStorageOperationContextsReleased.notify_one();
}
}  // namespace mongo
