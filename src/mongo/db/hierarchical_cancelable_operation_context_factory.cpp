// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"

namespace mongo {

HierarchicalCancelableOperationContextFactory::HierarchicalCancelableOperationContextFactory(
    CancellationToken parentCancelToken, ExecutorPtr executor)
    : _parentFactory{nullptr},
      _cancelSource{parentCancelToken},
      _cancelToken{_cancelSource.token()},
      _executor{std::move(executor)},
      _hierarchyDepth{0} {}

HierarchicalCancelableOperationContextFactory::HierarchicalCancelableOperationContextFactory(
    std::shared_ptr<const HierarchicalCancelableOperationContextFactory> parentFactory,
    CancellationToken parentCancelToken,
    ExecutorPtr executor,
    int hierarchyDepth)
    : _parentFactory{std::move(parentFactory)},
      _cancelSource{parentCancelToken},
      _cancelToken{_cancelSource.token()},
      _executor{std::move(executor)},
      _hierarchyDepth{hierarchyDepth} {}

std::shared_ptr<HierarchicalCancelableOperationContextFactory>
HierarchicalCancelableOperationContextFactory::createChild() {
    return std::shared_ptr<HierarchicalCancelableOperationContextFactory>(
        new HierarchicalCancelableOperationContextFactory(
            shared_from_this(), _cancelToken, _executor, _hierarchyDepth + 1));
}

CancelableOperationContext HierarchicalCancelableOperationContextFactory::makeOperationContext(
    Client* client, std::function<void(OperationContext*)> opCtxModifier) const {
    auto opCtx = client->makeOperationContext();
    opCtxModifier(opCtx.get());
    return CancelableOperationContext{std::move(opCtx), _cancelToken, _executor};
}

}  // namespace mongo
