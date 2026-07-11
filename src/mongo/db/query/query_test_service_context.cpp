// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_test_service_context.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/topology/sharding_state.h"

#include <memory>

namespace mongo {

QueryTestServiceContext::QueryTestServiceContext(
    std::unique_ptr<TickSourceMock<Nanoseconds>> tickSource)
    : _tickSource(tickSource.get()),
      _serviceContext(ServiceContext::make(nullptr, nullptr, std::move(tickSource))),
      _client(_serviceContext->getService()->makeClient("query_test")) {
    ShardingState::create(getServiceContext());
    CollatorFactoryInterface::set(getServiceContext(), std::make_unique<CollatorFactoryMock>());
}

QueryTestServiceContext::~QueryTestServiceContext() = default;

ServiceContext* QueryTestServiceContext::getServiceContext() const {
    return _serviceContext.get();
}

Client* QueryTestServiceContext::getClient() const {
    return _client.get();
}

ServiceContext::UniqueOperationContext QueryTestServiceContext::makeOperationContext() {
    return getClient()->makeOperationContext();
}

QueryTestScopedGlobalServiceContext::QueryTestScopedGlobalServiceContext()
    : _scopedGlobalServiceContext(std::make_unique<ScopedGlobalServiceContextForTest>()),
      _client(getServiceContext()->getService()->makeClient("query_test")) {
    ShardingState::create(getServiceContext());
    CollatorFactoryInterface::set(getServiceContext(), std::make_unique<CollatorFactoryMock>());
}

}  // namespace mongo
