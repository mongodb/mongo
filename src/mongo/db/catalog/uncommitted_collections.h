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

#include <map>
#include <memory>
#include <vector>

#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

namespace mongo {
class UncommittedCollections {
public:
    UncommittedCollections() = default;
    ~UncommittedCollections() = default;

    static UncommittedCollections& get(OperationContext* opCtx);

    static void addToTxn(OperationContext* opCtx,
                         std::unique_ptr<Collection> coll,
                         Timestamp createTime);

    static Collection* getForTxn(OperationContext* opCtx, const NamespaceStringOrUUID& nss);
    static Collection* getForTxn(OperationContext* opCtx, const NamespaceString& nss);
    static Collection* getForTxn(OperationContext* opCtx, const UUID& uuid);

    /**
     * Registers any uncommitted collections with the CollectionCatalog. If registering a collection
     * name conflicts with an existing entry, this method will throw a `WriteConflictException`.
     */
    void commit(OperationContext* opCtx, UUID uuid, Timestamp createTs);

    bool hasExclusiveAccessToCollection(OperationContext* opCtx, const NamespaceString& nss) const;

    void clear() {
        _collections.clear();
        _nssIndex.clear();
    }

private:
    std::map<UUID, std::unique_ptr<Collection>> _collections;
    std::map<NamespaceString, UUID> _nssIndex;
};
}  // namespace mongo
