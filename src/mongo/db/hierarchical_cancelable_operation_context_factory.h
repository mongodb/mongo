/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/cancelable_operation_context.h"

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

    CancelableOperationContext makeOperationContext(Client* client) const {
        return CancelableOperationContext{client->makeOperationContext(), _cancelToken, _executor};
    }

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
