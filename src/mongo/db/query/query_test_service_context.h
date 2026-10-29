// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/util/modules.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

/**
 * QueryTestServiceContext is a helper class for tests that require only a single Client under a
 * single ServiceContext for their execution context. The owned ServiceContext is decorated with a
 * CollatorFactoryMock.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] QueryTestServiceContext {
public:
    QueryTestServiceContext(std::unique_ptr<TickSourceMock<Nanoseconds>> tickSource =
                                std::make_unique<TickSourceMock<Nanoseconds>>());
    ~QueryTestServiceContext();

    ServiceContext* getServiceContext() const;

    Client* getClient() const;

    ServiceContext::UniqueOperationContext makeOperationContext();

    TickSourceMock<Nanoseconds>* tickSource() {
        return _tickSource;
    }

private:
    TickSourceMock<Nanoseconds>* _tickSource;
    ServiceContext::UniqueServiceContext _serviceContext;
    ServiceContext::UniqueClient _client;
};

/**
 * QueryTestScopedGlobalServiceContext is a helper class for tests that require only a single Client
 * under a single ScopedGlobalServiceContext for their execution context. The owned
 * ScopedGlobalServiceContext is decorated with a CollatorFactoryMock.
 */
class QueryTestScopedGlobalServiceContext {
public:
    QueryTestScopedGlobalServiceContext();

    ServiceContext* getServiceContext() const {
        return _scopedGlobalServiceContext->getServiceContext();
    }

    Client* getClient() const {
        return _client.get();
    }

    ServiceContext::UniqueOperationContext makeOperationContext() {
        return getClient()->makeOperationContext();
    }

private:
    std::unique_ptr<ScopedGlobalServiceContextForTest> _scopedGlobalServiceContext;
    ServiceContext::UniqueClient _client;
};

}  // namespace mongo
