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

#include "mongo/db/storage/temporary_record_store.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

/**
 * This is an implementation of an RAII type that manages a temporary RecordStore on a KVEngine.
 *
 * This object should not exist any longer than the provided OperationContext, as the destructor
 * uses it to drop the record store on the KVEngine.
 */
class TemporaryKVRecordStore : public TemporaryRecordStore {
public:
    TemporaryKVRecordStore(OperationContext* opCtx,
                           KVEngine* kvEngine,
                           std::unique_ptr<RecordStore> rs)
        : TemporaryRecordStore(std::move(rs)), _opCtx(opCtx), _kvEngine(kvEngine){};

    // Move constructor.
    TemporaryKVRecordStore(TemporaryKVRecordStore&& other) noexcept
        : TemporaryRecordStore(std::move(other._rs)),
          _opCtx(other._opCtx),
          _kvEngine(other._kvEngine) {}

    ~TemporaryKVRecordStore();

private:
    OperationContext* _opCtx;
    KVEngine* _kvEngine;
};

}  // namespace mongo
