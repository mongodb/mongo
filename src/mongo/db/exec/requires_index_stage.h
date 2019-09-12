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
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_descriptor.h"

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
                       const IndexDescriptor* indexDescriptor,
                       WorkingSet* workingSet);

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

    WorkingSetRegisteredIndexId workingSetIndexId() const {
        return _workingSetIndexId;
    }

private:
    // We keep a weak_ptr to the index catalog entry in order to detect when the underlying catalog
    // object has been destroyed, e.g. due to an index drop. In this scenario, the
    // RequiresIndexStage will throw a query-fatal exception after an attempt to lock the weak_ptr
    // indicates that the pointed-to object was deleted.
    //
    // This is necessary to protect against that case that our index is dropped and then recreated
    // during yield. Such an event should cause the query to be killed, since index cursors may have
    // pointers into catalog objects that no longer exist. Since indices do not have UUIDs,
    // different epochs of the index cannot be distinguished. The weak_ptr allows us to safely.
    // determine whether the pointed-to object has been destroyed during yield recovery via the
    // shared_ptr control block.
    std::weak_ptr<const IndexCatalogEntry> _weakIndexCatalogEntry;

    const IndexDescriptor* _indexDescriptor;
    const IndexAccessMethod* _indexAccessMethod;

    std::string _indexName;

    // An indentifier for the index required by this stage. Any working set member allocated to
    // represent an index key from this index must include this id.
    WorkingSetRegisteredIndexId _workingSetIndexId;
};

}  // namespace mongo
