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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/util/uuid.h"

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
    RequiresCollectionStage(const char* stageType,
                            ExpressionContext* expCtx,
                            const CollectionPtr& coll)
        : PlanStage(stageType, expCtx),
          _collection(&coll),
          _collectionUUID(coll->uuid()),
          _catalogEpoch(getCatalogEpoch()),
          _nss(coll->ns()) {}

    virtual ~RequiresCollectionStage() = default;

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

    const CollectionPtr& collection() const {
        return *_collection;
    }

    UUID uuid() const {
        return _collectionUUID;
    }

private:
    // This can only be called when the plan stage is attached to an operation context.
    uint64_t getCatalogEpoch() const {
        return CollectionCatalog::get(opCtx()).getEpoch();
    }

    // Pointer to a CollectionPtr that is stored at a high level in a AutoGetCollection or other
    // helper. It needs to stay valid until the PlanExecutor saves its state. To avoid this pointer
    // from dangling it needs to be reset when doRestoreState() is called and it is reset to a
    // different CollectionPtr.
    const CollectionPtr* _collection;
    const UUID _collectionUUID;
    const uint64_t _catalogEpoch;

    // TODO SERVER-31695: The namespace will no longer be needed once queries can survive collection
    // renames.
    const NamespaceString _nss;
};

// Type alias for use by PlanStages that write to a Collection.
using RequiresMutableCollectionStage = RequiresCollectionStage;

}  // namespace mongo
