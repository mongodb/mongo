// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/external_data_source_option_gen.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/util/decorable.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo {

/**
 * This class makes sure that virtual collections that are created for external data sources are
 * dropped when it's destroyed.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ExternalDataSourceScopeGuard {
public:
    // Makes ExternalDataSourceScopeGuard a decoration of ClientCursor.
    static const ClientCursor::Decoration<std::shared_ptr<ExternalDataSourceScopeGuard>> get;

    // Updates the operation context of decorated ExternalDataSourceScopeGuard object of 'cursor'
    // so that it can drop virtual collections in the new 'opCtx'.
    static void updateOperationContext(const ClientCursor* cursor, OperationContext* opCtx) {
        if (auto self = get(cursor); self) {
            self->_opCtx = opCtx;
        }
    }

    ExternalDataSourceScopeGuard(
        OperationContext* opCtx,
        const std::vector<std::pair<NamespaceString, std::vector<ExternalDataSourceInfo>>>&
            usedExternalDataSources);

    // It does not make sense to support copy ctor because this object must drop created virtual
    // collections.
    ExternalDataSourceScopeGuard(const ExternalDataSourceScopeGuard&) = delete;

    ExternalDataSourceScopeGuard(ExternalDataSourceScopeGuard&& other) noexcept
        : _opCtx(other._opCtx),
          _toBeDroppedVirtualCollections(std::move(other._toBeDroppedVirtualCollections)) {
        // Ownership of created virtual collections are moved to this object and the other object
        // must not try to drop them any more.
        other._opCtx = nullptr;
    }

    ~ExternalDataSourceScopeGuard() {
        dropVirtualCollections();
    }

private:
    // Must not throw. This can be called from the guard's destructor, and destructors must not
    // throw, since the destructor can be invoked when there is already an active exception being
    // handled. So this function makes a best effort to clean up, but if something goes wrong, the
    // error should logged but not thrown.
    void dropVirtualCollections();

    OperationContext* _opCtx;
    std::vector<NamespaceString> _toBeDroppedVirtualCollections;
};

}  // namespace mongo
