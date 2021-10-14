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

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/temporary_record_store.h"

namespace mongo {
namespace sbe {
/**
 * A way of interacting with the TemporaryRecordStore in PlanStages.
 * TODO SERVER-60639: Remove this when SERVER-60639 is done.
 */
class RecordStore {
public:
    RecordStore(OperationContext* opCtx, std::unique_ptr<TemporaryRecordStore> rs)
        : _opCtx(opCtx), _rs(std::move(rs)){};

    RecordStore(RecordStore&& other) : _opCtx(other._opCtx), _rs(std::move(other._rs)) {
        other._opCtx = nullptr;
    }
    ~RecordStore() {
        deleteTemporaryRecordStoreIfLock();
    }

    RecordStore& operator=(RecordStore&& other) {
        if (this == &other) {
            return *this;
        }
        _opCtx = other._opCtx;
        _rs = std::move(other._rs);
        other._opCtx = nullptr;
        return *this;
    }

    void doDetachFromOperationContext() {
        _opCtx = nullptr;
    }

    void doAttachToOperationContext(OperationContext* opCtx) {
        _opCtx = opCtx;
    }

    void deleteTemporaryRecordStoreIfLock() {
        // A best-effort to destroy the TRS, we can only do this if the query is holding a
        // global lock.
        if (_opCtx->lockState()->wasGlobalLockTaken()) {
            _rs->finalizeTemporaryTable(_opCtx, TemporaryRecordStore::FinalizationAction::kDelete);
        }
    }
    // add attachToOperationContext, detachFromOperationContext, call this from
    // HashAgg::attachToOperationContext, detachFromOperationContext.

private:
    OperationContext* _opCtx;
    std::unique_ptr<TemporaryRecordStore> _rs;
};
}  // namespace sbe
}  // namespace mongo
