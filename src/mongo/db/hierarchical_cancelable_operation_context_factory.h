// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * TODO SERVER-103945: Remove this class. This is just a stop gap solution for the memory
 * usage caused by long living cancellation sources.
 *
 * This factory creates a new cancellation token that new listeners can be added to. This helps
 * mitigate the number of listeners added to the parent token by making new listeners attach
 * to the cancel token of this class instead of the parent cancel token.
 *
 * Note that each time a HierarchicalCancelableOperationContextFactory is created with the same
 * parent token, the onCancel listener for the parent token will added permanently until the parent
 * token is destroyed.
 */
class HierarchicalCancelableOperationContextFactory {
public:
    HierarchicalCancelableOperationContextFactory(CancellationToken parentCancelToken,
                                                  ExecutorPtr executor);

    HierarchicalCancelableOperationContextFactory(
        const HierarchicalCancelableOperationContextFactory&) = delete;
    HierarchicalCancelableOperationContextFactory& operator=(
        const HierarchicalCancelableOperationContextFactory&) = delete;

    HierarchicalCancelableOperationContextFactory(HierarchicalCancelableOperationContextFactory&&) =
        delete;
    HierarchicalCancelableOperationContextFactory& operator=(
        HierarchicalCancelableOperationContextFactory&&) = delete;

    std::unique_ptr<HierarchicalCancelableOperationContextFactory> createChild();

    /**
     * Creates a child factory as a shared_ptr. Use this when the child needs to be captured
     * in lambdas that may be copied (e.g., for future chains), as shared_ptr allows shared
     * ownership to keep the child alive until all captures are destroyed.
     */
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> createSharedChild();

    int getHierarchyDepth() const {
        return _hierarchyDepth;
    };

    CancellationToken token() const {
        return _cancelToken;
    }

    CancelableOperationContext makeOperationContext(
        Client* client,
        std::function<void(OperationContext*)> opCtxModifier = [](OperationContext*) {}) const;

private:
    HierarchicalCancelableOperationContextFactory(CancellationToken parentCancelToken,
                                                  ExecutorPtr executor,
                                                  int hierarchyDepth);

    const CancellationSource _cancelSource;
    const CancellationToken _cancelToken;
    const ExecutorPtr _executor;
    const int _hierarchyDepth;
};

}  // namespace mongo
