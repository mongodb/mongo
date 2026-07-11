// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"

namespace mongo {

HierarchicalCancelableOperationContextFactory::HierarchicalCancelableOperationContextFactory(
    CancellationToken parentCancelToken, ExecutorPtr executor)
    : _cancelSource{parentCancelToken},
      _cancelToken{_cancelSource.token()},
      _executor{std::move(executor)},
      _hierarchyDepth{0} {}

HierarchicalCancelableOperationContextFactory::HierarchicalCancelableOperationContextFactory(
    CancellationToken parentCancelToken, ExecutorPtr executor, int hierarchyDepth)
    : _cancelSource{parentCancelToken},
      _cancelToken{_cancelSource.token()},
      _executor{std::move(executor)},
      _hierarchyDepth{hierarchyDepth} {}

std::unique_ptr<HierarchicalCancelableOperationContextFactory>
HierarchicalCancelableOperationContextFactory::createChild() {
    return std::unique_ptr<HierarchicalCancelableOperationContextFactory>(
        new HierarchicalCancelableOperationContextFactory(
            _cancelToken, _executor, _hierarchyDepth + 1));
}

std::shared_ptr<HierarchicalCancelableOperationContextFactory>
HierarchicalCancelableOperationContextFactory::createSharedChild() {
    return std::shared_ptr<HierarchicalCancelableOperationContextFactory>(
        new HierarchicalCancelableOperationContextFactory(
            _cancelToken, _executor, _hierarchyDepth + 1));
}

CancelableOperationContext HierarchicalCancelableOperationContextFactory::makeOperationContext(
    Client* client, std::function<void(OperationContext*)> opCtxModifier) const {
    auto opCtx = client->makeOperationContext();
    opCtxModifier(opCtx.get());
    return CancelableOperationContext{std::move(opCtx), _cancelToken, _executor};
}

}  // namespace mongo
