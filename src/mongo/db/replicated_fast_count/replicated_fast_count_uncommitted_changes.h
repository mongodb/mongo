/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_size_and_info.h"
#include "mongo/util/uuid.h"

#include <boost/container/flat_map.hpp>

namespace mongo {

class MONGO_MOD_PUBLIC UncommittedFastCountChange {
public:
    /**
     * Returns an immutable reference to the UncommittedFastCountChange instance associated with
     * this particular OperationContext, returning an empty instance if none exist.
     */
    static const UncommittedFastCountChange& getForRead(OperationContext* opCtx);

    /**
     * Returns a mutable reference to the UncommittedFastCountChange instance associated with
     * this particular OperationContext, creating one if none exist.
     *
     * If an instance is created, a callback will be registered for the RecoveryUnit attached to the
     * OperationContext that will update the changes tracked in _trackedChanges.
     */
    static UncommittedFastCountChange& getForWrite(OperationContext* opCtx);

    /**
     * Given a collection UUID, returns the current uncommitted value of size and count for that
     * collection. If the collection UUID does not exist, an empty CollectionSizeCount is returned.
     */
    CollectionSizeCount find(const UUID& uuid) const;

    /**
     * Records a change in count and size for the given collection UUID.
     */
    void record(const UUID& uuid, int64_t numDelta, int64_t sizeDelta);

private:
    // Map of collection UUID to uncommitted values for size and count.
    boost::container::flat_map<UUID, CollectionSizeCount> _trackedChanges;
};

}  // namespace mongo

