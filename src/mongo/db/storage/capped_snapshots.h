/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/capped_visibility.h"
#include "mongo/db/catalog/collection.h"

namespace mongo {
class OperationContext;
class RecoveryUnit;

/**
 * CappedSnapshots is a container for managing the capped visibility snapshots for collections for a
 * specific RecoveryUnit. Callers must establish a capped visibility snapshot before opening a
 * storage snapshot. Once the storage snapshot is closed, due to a committed unit of work or end of
 * a read transaction, the snapshot is invalidated, and another call is required to establish the
 * snapshot.
 */
class CappedSnapshots {
public:
    static CappedSnapshots& get(RecoveryUnit* ru);
    static CappedSnapshots& get(OperationContext* opCtx);

    /**
     * Must be called before opening a forward cursor on this capped collection. Establishes a
     * consistent view of the capped visibility for this collection. The snapshot is invalidated for
     * this collection when the storage engine snapshot is closed.
     */
    void establish(OperationContext* opCtx, const CollectionPtr& coll);
    void establish(OperationContext* opCtx, const Collection* coll);

    /**
     * Retrieve a previously established visibility snapshot. If no prior call to establish() has
     * been made or if the storage snapshot has been closed since the last call to establish(), this
     * will return boost::none, indicating that the caller may be attempting to do something unsafe
     * that would return records past the capped visibility point.
     */
    boost::optional<CappedVisibilitySnapshot> getSnapshot(StringData ident) const;

private:
    void _setSnapshot(StringData ident, CappedVisibilitySnapshot snapshot);

private:
    StringMap<CappedVisibilitySnapshot> _snapshots;
};
}  // namespace mongo
