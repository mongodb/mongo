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

#include <functional>

#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context.h"

namespace mongo::sbe {
/**
 * A callback which gets called whenever a stage which accesses the storage engine (e.g. "scan",
 * "seek", or "ixscan") obtains or re-obtains its AutoGet*.
 */
using LockAcquisitionCallback =
    std::function<void(OperationContext*, const AutoGetCollectionForReadMaybeLockFree&)>;

/**
 * Given a collection UUID, acquires 'coll', invokes the provided 'lockAcquisiionCallback', and
 * returns the collection's name.
 *
 * This is intended for use during the preparation of an SBE plan. The caller must hold the
 * appropriate db_raii object in order to ensure that SBE plan preparation sees a consistent view of
 * the catalog.
 */
NamespaceString acquireCollection(OperationContext* opCtx,
                                  CollectionUUID collUuid,
                                  const LockAcquisitionCallback& lockAcquisitionCallback,
                                  boost::optional<AutoGetCollectionForReadMaybeLockFree>& coll);

/**
 * Re-acquires 'coll', intended for use during SBE yield recovery or when a closed SBE plan is
 * re-opened. In addition to acquiring 'coll', throws a UserException if the collection has been
 * dropped or renamed. SBE query execution currently cannot survive such events if they occur during
 * a yield or between getMores.
 */
void restoreCollection(OperationContext* opCtx,
                       const NamespaceString& collName,
                       CollectionUUID collUuid,
                       const LockAcquisitionCallback& lockAcquisitionCallback,
                       boost::optional<AutoGetCollectionForReadMaybeLockFree>& coll);
}  // namespace mongo::sbe
