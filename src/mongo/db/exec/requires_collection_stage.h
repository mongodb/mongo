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
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * A base class for plan stages which access a collection. In addition to providing derived classes
 * access to the Collection pointer, the primary purpose of this class is to assume responsibility
 * for checking that the collection is still valid (e.g. has not been dropped) when recovering from
 * yield.
 *
 * Subclasses must implement doSaveStateRequiresCollection() and doRestoreStateRequiresCollection()
 * in order to supply custom yield preparation or yield recovery logic.
 *
 * Templated on 'CollectionT', which may be instantiated using either Collection* or const
 * Collection*. This abstracts the implementation of this base class for use by derived classes
 * which read (e.g. COLLSCAN and MULTI_ITERATOR) and derived classes that write (e.g. UPDATE and
 * DELETE). Derived classes should use the 'RequiresCollectionStage' or
 * 'RequiresMutableCollectionStage' aliases provided below.
 */
template <typename CollectionT>
class RequiresCollectionStageBase : public PlanStage {
public:
    RequiresCollectionStageBase(const char* stageType, OperationContext* opCtx, CollectionT coll)
        : PlanStage(stageType, opCtx),
          _collection(coll),
          _collectionUUID(_collection->uuid()),
          _databaseEpoch(getDatabaseEpoch(_collection)),
          _nss(_collection->ns()) {
        invariant(_collection);
    }

    virtual ~RequiresCollectionStageBase() = default;

protected:
    void doSaveState() final;

    void doRestoreState() final;

    /**
     * Performs yield preparation specific to a stage which subclasses from RequiresCollectionStage.
     */
    virtual void doSaveStateRequiresCollection() = 0;

    /**
     * Performs yield recovery specific to a stage which subclasses from RequiresCollectionStage.
     */
    virtual void doRestoreStateRequiresCollection() = 0;

    CollectionT collection() const {
        return _collection;
    }

    UUID uuid() const {
        return _collectionUUID;
    }

private:
    // This can only be called when the plan stage is attached to an operation context. The
    // collection pointer 'coll' must be non-null and must point to a valid collection.
    uint64_t getDatabaseEpoch(CollectionT coll) const {
        invariant(coll);
        auto databaseHolder = DatabaseHolder::get(getOpCtx());
        auto db = databaseHolder->getDb(getOpCtx(), coll->ns().ns());
        invariant(db);
        return db->epoch();
    }

    CollectionT _collection;
    const UUID _collectionUUID;
    const uint64_t _databaseEpoch;

    // TODO SERVER-31695: The namespace will no longer be needed once queries can survive collection
    // renames.
    const NamespaceString _nss;
};

// Type alias for use by PlanStages that read a Collection.
using RequiresCollectionStage = RequiresCollectionStageBase<const Collection*>;

// Type alias for use by PlanStages that write to a Collection.
using RequiresMutableCollectionStage = RequiresCollectionStageBase<Collection*>;

}  // namespace mongo
