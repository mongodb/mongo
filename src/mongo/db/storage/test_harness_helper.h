// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Sets up an OperationContext with a Recovery Unit. Uses a ServiceContextNoop.
 *
 * A particular HarnessHelper implementation will implement registerXXXHarnessHelperFactory() and
 * newXXXHarnessHelper() such that generic unit tests can create and test that particular
 * HarnessHelper implementation. The newRecoveryUnit() implementation dictates what RecoveryUnit
 * implementation the OperationContext has.
 */
class [[MONGO_MOD_OPEN]] HarnessHelper : public ScopedGlobalServiceContextForTest {
public:
    explicit HarnessHelper() : _threadClient(getGlobalServiceContext()->getService()) {}

    virtual ServiceContext::UniqueOperationContext newOperationContext(Client* const client) {
        auto opCtx = client->makeOperationContext();
        shard_role_details::setRecoveryUnit(
            opCtx.get(), newRecoveryUnit(), WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        return opCtx;
    }

    virtual ServiceContext::UniqueOperationContext newOperationContext() {
        return newOperationContext(client());
    }

    Client* client() const {
        return Client::getCurrent();
    }

    ServiceContext* serviceContext() {
        return getGlobalServiceContext();
    }

    const ServiceContext* serviceContext() const {
        return getGlobalServiceContext();
    }

    virtual std::unique_ptr<RecoveryUnit> newRecoveryUnit() = 0;

protected:
    ThreadClient _threadClient;
};

}  // namespace mongo
