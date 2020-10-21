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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/kv/temporary_kv_record_store.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

TemporaryKVRecordStore::~TemporaryKVRecordStore() {
    invariant(_recordStoreHasBeenFinalized);
}

void TemporaryKVRecordStore::finalizeTemporaryTable(OperationContext* opCtx,
                                                    FinalizationAction action) {
    invariant(!_recordStoreHasBeenFinalized);

    _recordStoreHasBeenFinalized = true;

    // This function is added as an onCommit() handler in certain places, at which point it is not
    // possible to get a WriteConflictException. We're only concerned when calling this function in
    // a WriteUnitOfWork that can still be rolled back.
    if (opCtx->recoveryUnit()->isActive()) {
        opCtx->recoveryUnit()->onRollback([this]() { _recordStoreHasBeenFinalized = false; });
    }

    if (action == FinalizationAction::kKeep)
        return;

    // Need at least Global IS before calling into the storage engine, to protect against it being
    // destructed while we're using it.
    invariant(opCtx->lockState()->isReadLocked());

    auto status = _kvEngine->dropIdent(opCtx->recoveryUnit(), _rs->getIdent());

    if (!status.isOK()) {
        LOGV2_ERROR(4841503, "Failed to drop temporary table", "ident"_attr = _rs->getIdent());
    }
    dassert(status, str::stream() << "Failed to drop temporary table. Ident: " << _rs->getIdent());
}

}  // namespace mongo
