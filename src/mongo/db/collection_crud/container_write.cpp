/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/collection_crud/container_write.h"

#include "mongo/db/op_observer/op_observer.h"

namespace mongo::container_write {

Status insert(OperationContext* opCtx,
              RecoveryUnit& ru,
              const CollectionPtr& coll,
              IntegerKeyedContainer& container,
              int64_t key,
              std::span<const char> value) {
    auto status = container.insert(ru, key, value);
    if (!status.isOK()) {
        return status;
    }

    opCtx->getServiceContext()->getOpObserver()->onContainerInsert(
        opCtx, coll->ns(), coll->uuid(), container.ident()->getIdent(), key, value);

    return Status::OK();
}

Status insert(OperationContext* opCtx,
              RecoveryUnit& ru,
              const CollectionPtr& coll,
              StringKeyedContainer& container,
              std::span<const char> key,
              std::span<const char> value) {
    auto status = container.insert(ru, key, value);
    if (!status.isOK()) {
        return status;
    }

    opCtx->getServiceContext()->getOpObserver()->onContainerInsert(
        opCtx, coll->ns(), coll->uuid(), container.ident()->getIdent(), key, value);

    return Status::OK();
}

Status remove(OperationContext* opCtx,
              RecoveryUnit& ru,
              const CollectionPtr& coll,
              IntegerKeyedContainer& container,
              int64_t key) {
    auto status = container.remove(ru, key);
    if (!status.isOK()) {
        return status;
    }

    opCtx->getServiceContext()->getOpObserver()->onContainerDelete(
        opCtx, coll->ns(), coll->uuid(), container.ident()->getIdent(), key);

    return Status::OK();
}

Status remove(OperationContext* opCtx,
              RecoveryUnit& ru,
              const CollectionPtr& coll,
              StringKeyedContainer& container,
              std::span<const char> key) {
    auto status = container.remove(ru, key);
    if (!status.isOK()) {
        return status;
    }

    opCtx->getServiceContext()->getOpObserver()->onContainerDelete(
        opCtx, coll->ns(), coll->uuid(), container.ident()->getIdent(), key);

    return Status::OK();
}

}  // namespace mongo::container_write
