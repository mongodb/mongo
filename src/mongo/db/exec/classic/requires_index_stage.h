// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

namespace mongo {

/**
 * A base class for plan stages which require access to a particular index within a particular
 * collection. Provides subclasses access to the index's const CollectionPtr&, as well as to catalog
 * types representing the index itself such as the IndexDescriptor. This base class is responsible
 * for checking that the collection and index are still valid (e.g. have not been dropped) when
 * recovering from yield.
 *
 * Subclasses must implement doSaveStateRequiresIndex() and doRestoreStateRequiresIndex() in order
 * to supply custom yield preparation and yield recovery logic.
 */
class RequiresIndexStage : public RequiresCollectionStage {
public:
    RequiresIndexStage(std::string_view stageType,
                       ExpressionContext* expCtx,
                       CollectionAcquisition collection,
                       const IndexCatalogEntry* indexEntry,
                       WorkingSet* workingSet);

    ~RequiresIndexStage() override = default;

protected:
    /**
     * Performs yield preparation specific to a stage which subclasses from RequiresIndexStage.
     */
    virtual void doSaveStateRequiresIndex() = 0;

    /**
     * Performs yield recovery specific to a stage which subclasses from RequiresIndexStage.
     */
    virtual void doRestoreStateRequiresIndex() = 0;

    void doSaveStateRequiresCollection() override;

    void doRestoreStateRequiresCollection() override;

    const IndexDescriptor* indexDescriptor() const {
        return _entry ? _entry->descriptor() : nullptr;
    }

    const IndexCatalogEntry* indexEntry() const {
        return _entry;
    }

    const SortedDataIndexAccessMethod* indexAccessMethod() const {
        return _entry ? _entry->accessMethod()->asSortedData() : nullptr;
    }

    WorkingSetRegisteredIndexId workingSetIndexId() const {
        return _workingSetIndexId;
    }

private:
    // Set to nullptr during a yield. During a restore, we do an index catalog lookup using the
    // index ident to determine whether the index still exists and reset the entry pointer.
    const IndexCatalogEntry* _entry;

    const std::string _indexIdent;
    const std::string _indexName;

    // An identifier for the index required by this stage. Any working set member allocated to
    // represent an index key from this index must include this id.
    const WorkingSetRegisteredIndexId _workingSetIndexId;
};

}  // namespace mongo
