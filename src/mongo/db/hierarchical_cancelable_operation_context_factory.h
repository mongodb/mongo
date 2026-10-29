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
class HierarchicalCancelableOperationContextFactory
    : public std::enable_shared_from_this<HierarchicalCancelableOperationContextFactory> {
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

    /**
     * Creates a child factory whose cancellation source is chained onto this factory's token.
     *
     * The child holds a reference to this factory automatically, so this factory cannot be
     * destroyed while the child is still in use. That matters because a cancellation signal reaches
     * the child only by propagating through this factory. Destroying this factory dismisses its
     * source rather than canceling it, and dismissal does not run cancellation callbacks, so the
     * link would be broken: the child would never observe a cancellation from above, leaving it
     * permanently unable to report cancellation or to kill the operation contexts it hands out.
     *
     * This factory must be shared_ptr-owned, or shared_from_this() throws bad_weak_ptr.
     */
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> createChild();

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
    HierarchicalCancelableOperationContextFactory(
        std::shared_ptr<const HierarchicalCancelableOperationContextFactory> parentFactory,
        CancellationToken parentCancelToken,
        ExecutorPtr executor,
        int hierarchyDepth);

    /**
     * Keep a reference to the parent factory to prevent it from being destroyed while this factory
     * is still in use.
     */
    const std::shared_ptr<const HierarchicalCancelableOperationContextFactory> _parentFactory;
    const CancellationSource _cancelSource;
    const CancellationToken _cancelToken;
    const ExecutorPtr _executor;
    const int _hierarchyDepth;
};

}  // namespace mongo
