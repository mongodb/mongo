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

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

/**
 * QueryTestServiceContext is a helper class for tests that require only a single Client under a
 * single ServiceContext for their execution context. The owned ServiceContext is decorated with a
 * CollatorFactoryMock.
 */
class QueryTestServiceContext {
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
