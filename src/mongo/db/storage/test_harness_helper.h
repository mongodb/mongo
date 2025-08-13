/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/service_context_test_fixture.h"

namespace mongo {

/**
 * Sets up an OperationContext with a Recovery Unit. Uses a ServiceContextNoop.
 *
 * A particular HarnessHelper implementation will implement registerXXXHarnessHelperFactory() and
 * newXXXHarnessHelper() such that generic unit tests can create and test that particular
 * HarnessHelper implementation. The newRecoveryUnit() implementation dictates what RecoveryUnit
 * implementation the OperationContext has.
 */
class HarnessHelper : public ScopedGlobalServiceContextForTest {
public:
    ~HarnessHelper() override = default;
    explicit HarnessHelper()
        : _threadClient(getGlobalServiceContext()->getService(ClusterRole::ShardServer)) {}

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
