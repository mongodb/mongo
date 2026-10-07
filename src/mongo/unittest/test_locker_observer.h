// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/locker.h"

#include <memory>

namespace mongo {
namespace unittest {

/**
 * Registers a test-only ClientObserver that installs a Locker on each new OperationContext,
 * like production does. No-op if the opCtx already has one.
 */
[[MONGO_MOD_PUBLIC]] inline void installTestLockerObserver(ServiceContext* service) {
    class EnsureLockerClientObserver final : public ServiceContext::ClientObserver {
    public:
        void onCreateClient(Client* client) final {}
        void onDestroyClient(Client* client) final {}

        void onCreateOperationContext(OperationContext* opCtx) final {
            if (!opCtx->lockState_DO_NOT_USE()) {
                opCtx->setLockState_DO_NOT_USE(
                    std::make_unique<Locker>(opCtx->getServiceContext()));
            }
        }

        void onDestroyOperationContext(OperationContext* opCtx) final {}
    };

    service->registerClientObserver(std::make_unique<EnsureLockerClientObserver>());
}

}  // namespace unittest
}  // namespace mongo
