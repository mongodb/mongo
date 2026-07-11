// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/metadata/path_arrayness.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/restore_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <string_view>

namespace mongo {

/**
 * A class for plan stages which access a collection. In addition to providing derived classes
 * access to the Collection pointer, the primary purpose of this class is to assume responsibility
 * for checking that the collection is still valid (e.g. has not been dropped) when recovering from
 * yield.
 *
 * Subclasses must implement doSaveStateRequiresCollection() and doRestoreStateRequiresCollection()
 * in order to supply custom yield preparation or yield recovery logic.
 */
class RequiresCollectionStage : public PlanStage {
public:
    RequiresCollectionStage(std::string_view stageType,
                            ExpressionContext* expCtx,
                            CollectionAcquisition coll)
        : PlanStage(stageType, expCtx),
          _collection(coll),
          _collectionPtr(&coll.getCollectionPtr()),
          _collectionUUID(coll.getCollectionPtr()->uuid()),
          _catalogEpoch(getCatalogEpoch()),
          _nss(coll.getCollectionPtr()->ns()),
          _pathArraynessChecker{.nonArrayPaths =
                                    expCtx->nonArrayPathsForNss(coll.getCollectionPtr()->ns()),
                                .prevEpoch = boost::none} {}

    ~RequiresCollectionStage() override = default;

protected:
    void doSaveState() final;

    void doRestoreState(const RestoreContext& context) final;

    /**
     * Performs yield preparation specific to a stage which subclasses from RequiresCollectionStage.
     */
    virtual void doSaveStateRequiresCollection() = 0;

    /**
     * Performs yield recovery specific to a stage which subclasses from RequiresCollectionStage.
     */
    virtual void doRestoreStateRequiresCollection() = 0;

    const CollectionAcquisition& collection() const {
        return _collection;
    }

    const CollectionPtr& collectionPtr() const {
        return *_collectionPtr;
    }

    UUID uuid() const {
        return _collectionUUID;
    }

private:
    // This can only be called when the plan stage is attached to an operation context.
    uint64_t getCatalogEpoch() const;

    // Pointer to a CollectionPtr that is stored at a high level in a AutoGetCollection or other
    // helper. It needs to stay valid until the PlanExecutor saves its state. To avoid this pointer
    // from dangling it needs to be reset when doRestoreState() is called and it is reset to a
    // different CollectionPtr.
    CollectionAcquisition _collection;
    const CollectionPtr* _collectionPtr;
    const UUID _collectionUUID;
    const uint64_t _catalogEpoch;

    // TODO SERVER-31695: The namespace will no longer be needed once queries can survive collection
    // renames.
    const NamespaceString _nss;

    PathArraynessChecker _pathArraynessChecker;
};

// Type alias for use by PlanStages that write to a Collection.
class RequiresWritableCollectionStage : public RequiresCollectionStage {
public:
    RequiresWritableCollectionStage(std::string_view stageType,
                                    ExpressionContext* expCtx,
                                    CollectionAcquisition coll)
        : RequiresCollectionStage(stageType, expCtx, coll), _collectionAcquisition(coll) {}

    const CollectionAcquisition& collectionAcquisition() const {
        return _collectionAcquisition;
    }

private:
    const CollectionAcquisition _collectionAcquisition;
};

}  // namespace mongo
