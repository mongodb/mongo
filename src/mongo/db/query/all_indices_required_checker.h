/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <map>
#include <string>

namespace mongo {

/**
 * A utility which, on construction, takes note of all indices for a given collection. The caller
 * can subsequently check whether any of those indices have been dropped.
 */
class AllIndicesRequiredChecker {
public:
    /**
     * Constructs an 'AllIndicesRequiredChecker' which can be used later to ensure that none of the
     * indices from 'collections' have been dropped. The caller must hold the appropriate db_raii
     * object in order to read the collection's index catalog.
     */
    explicit AllIndicesRequiredChecker(const MultipleCollectionAccessor& collections);

    /**
     * Throws a 'QueryPlanKilled' error if any of the indices which existed at the time of
     * construction have since been dropped.
     */
    void check(OperationContext* opCtx, const MultipleCollectionAccessor& collections) const;

private:
    void saveIndicesForCollection(const CollectionPtr& collection);

    void checkIndicesForCollection(OperationContext* opCtx, const CollectionPtr& collection) const;

    // This map of map holds index idents to all of the index catalog entries known at the time of
    // construction, grouped first by collection UUID then by index name. Later, we can attempt to
    // lookup the index entry by its ident in order to determine whether an index in the list has
    // been dropped.
    std::map<UUID, StringMap<std::string>> _identEntries;
};

}  // namespace mongo
