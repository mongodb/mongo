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

namespace mongo {

/**
 * A base class for plan stages which require access to a particular index within a particular
 * collection. Provides subclasses access to the index's Collection*, as well as to catalog types
 * representing the index itself such as the IndexDescriptor. This base class is responsible for
 * checking that the collection and index are still valid (e.g. have not been dropped) when
 * recovering from yield.
 *
 * Subclasses must implement doSaveStateRequiresIndex() and doRestoreStateRequiresIndex() in order
 * to supply custom yield preparation and yield recovery logic.
 */
class RequiresIndexStage : public RequiresCollectionStage {
public:
    RequiresIndexStage(const char* stageType,
                       OperationContext* opCtx,
                       const IndexDescriptor* indexDescriptor)
        : RequiresCollectionStage(stageType, opCtx, indexDescriptor->getCollection()),
          _weakIndexCatalogEntry(collection()->getIndexCatalog()->getEntryShared(indexDescriptor)),
          _indexCatalogEntry(_weakIndexCatalogEntry.lock()),
          _indexDescriptor(indexDescriptor),
          _indexAccessMethod(_indexCatalogEntry->accessMethod()),
          _indexName(_indexDescriptor->indexName()) {
        invariant(_indexCatalogEntry);
        invariant(_indexDescriptor);
        invariant(_indexAccessMethod);
    }

    virtual ~RequiresIndexStage() = default;

protected:
    /**
     * Performs yield preparation specific to a stage which subclasses from RequiresIndexStage.
     */
    virtual void doSaveStateRequiresIndex() = 0;

    /**
     * Performs yield recovery specific to a stage which subclasses from RequiresIndexStage.
     */
    virtual void doRestoreStateRequiresIndex() = 0;

    void doSaveStateRequiresCollection() override final;

    void doRestoreStateRequiresCollection() override final;

    const IndexDescriptor* indexDescriptor() const {
        return _indexDescriptor;
    }

    const IndexAccessMethod* indexAccessMethod() const {
        return _indexAccessMethod;
    }

private:
    // This stage shares ownership of the index catalog entry when the query is running, and
    // relinquishes its shared ownership when in a saved state for a yield or between getMores. We
    // keep a weak_ptr to the entry in order to reacquire shared ownership on yield recovery,
    // throwing a query-fatal exception if the weak_ptr indicates that the underlying catalog object
    // has been destroyed.
    //
    // This is necessary to protect against that case that our index is dropped and then recreated
    // during yield. Such an event should cause the query to be killed, since index cursors may have
    // pointers into catalog objects that no longer exist. Since indices do not have UUIDs,
    // different epochs of the index cannot be distinguished. The weak_ptr allows us to relinquish
    // ownership of the index during yield, but also determine whether the pointed-to object has
    // been destroyed during yield recovery.
    std::weak_ptr<const IndexCatalogEntry> _weakIndexCatalogEntry;
    std::shared_ptr<const IndexCatalogEntry> _indexCatalogEntry;

    const IndexDescriptor* _indexDescriptor;
    const IndexAccessMethod* _indexAccessMethod;

    const std::string _indexName;
};

}  // namespace mongo
