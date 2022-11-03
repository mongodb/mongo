/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/clientcursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/external_data_source_option_gen.h"

namespace mongo {
/**
 * This class makes sure that virtual collections that are created for external data sources are
 * dropped when it's destroyed.
 */
class ExternalDataSourceScopeGuard {
public:
    // Makes ExternalDataSourceScopeGuard a decoration of ClientCursor.
    static const ClientCursor::Decoration<std::shared_ptr<ExternalDataSourceScopeGuard>> get;

    // Updates the operation context of decorated ExternalDataSourceScopeGuard object of 'cursor'
    // so that it can drop virtual collections in the new 'opCtx'.
    static void updateOperationContext(const ClientCursor* cursor, OperationContext* opCtx) {
        if (auto self = get(cursor); self) {
            get(cursor)->_opCtx = opCtx;
        }
    }

    ExternalDataSourceScopeGuard() : _opCtx(nullptr), _toBeDroppedVirtualCollections() {}

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
    void dropVirtualCollections() noexcept;

    OperationContext* _opCtx;
    std::vector<NamespaceString> _toBeDroppedVirtualCollections;
};
}  // namespace mongo
