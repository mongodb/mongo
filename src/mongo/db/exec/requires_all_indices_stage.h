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

#include "mongo/db/exec/requires_collection_stage.h"
#include "mongo/db/index/index_descriptor.h"

namespace mongo {

/**
 * A base class for plan stages which require access to _all_ indices of a collection, and should
 * cause the query to die on yield recovery if any index is dropped. Plan stages which depend on a
 * single index, such as IXSCAN, should instead use RequiresIndexStage.
 */
class RequiresAllIndicesStage : public RequiresCollectionStage {
public:
    RequiresAllIndicesStage(const char* stageType, OperationContext* opCtx, const Collection* coll)
        : RequiresCollectionStage(stageType, opCtx, coll) {
        auto allEntriesShared = coll->getIndexCatalog()->getAllReadyEntriesShared();
        _indexCatalogEntries.reserve(allEntriesShared.size());
        _indexNames.reserve(allEntriesShared.size());
        for (auto&& index : allEntriesShared) {
            _indexCatalogEntries.emplace_back(index);
            _indexNames.push_back(index->descriptor()->indexName());
        }
    }

    virtual ~RequiresAllIndicesStage() = default;

protected:
    void doSaveStateRequiresCollection() override final {}

    void doRestoreStateRequiresCollection() override final;

    /**
     * Subclasses may call this to indicate that they no longer require all indices on the
     * collection to survive. After calling this, yield recovery will never fail.
     */
    void releaseAllIndicesRequirement() {
        _indexCatalogEntries.clear();
        _indexNames.clear();
    }

private:
    // This stage holds weak pointers to all of the index catalog entries known at the time of
    // construction. During yield recovery, we attempt to lock each weak pointer in order to
    // determine whether an index we rely on has been destroyed. If we can lock the weak pointer, we
    // need to check the 'isDropped()' flag on the index catalog entry. If any index has been
    // destroyed, then we throw a query-fatal exception during restore.
    std::vector<std::weak_ptr<const IndexCatalogEntry>> _indexCatalogEntries;

    // The names of the indices above. Used for error reporting.
    std::vector<std::string> _indexNames;
};

}  // namespace mongo
