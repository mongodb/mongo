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

#include <cstdint>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/operation_context.h"

namespace mongo {

/**
 * Records keys that have violated duplicate key constraints on unique indexes. The keys are backed
 * by a temporary collection that the caller is responsible for creating, destroying, and holding
 * locks while passing into mutating functions.
 */
class DuplicateKeyTracker {
    MONGO_DISALLOW_COPYING(DuplicateKeyTracker);

public:
    DuplicateKeyTracker(const IndexCatalogEntry* indexCatalogEntry, const NamespaceString& tempNss);
    ~DuplicateKeyTracker();

    /**
     * Generates a unique namespace that should be used to construct the temporary backing
     * Collection for this tracker.
     */
    static NamespaceString makeTempNamespace();

    /**
     * Given a set of duplicate keys, insert them into tempCollection.
     *
     * The caller must hold locks for 'tempCollection' and be in a WriteUnitOfWork.
     */
    Status recordDuplicates(OperationContext* opCtx,
                            Collection* tempCollection,
                            const std::vector<BSONObj>& keys);

    /**
     * Returns Status::OK if all previously recorded duplicate key constraint violations have been
     * resolved for the index. Returns a DuplicateKey error if there are still duplicate key
     * constraint violations on the index.
     *
     * The caller must hold locks for 'tempCollection'.
     */
    Status constraintsSatisfiedForIndex(OperationContext* opCtx, Collection* tempCollection) const;

    const NamespaceString& nss() const {
        return _nss;
    }

private:
    std::int64_t _idCounter;

    const IndexCatalogEntry* _indexCatalogEntry;
    const NamespaceString _nss;
};

}  // namespace mongo
