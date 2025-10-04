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

#include "mongo/db/storage/storage_engine_direct_crud.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/shared_buffer.h"

#include <span>

namespace mongo::storage_engine_direct_crud {

Status insert(StorageEngine& engine,
              RecoveryUnit& ru,
              StringData ident,
              std::span<const char> key,
              std::span<const char> value) {
    return engine.getEngine()->insertIntoIdent(ru, ident, key, value);
}

Status insert(StorageEngine& engine,
              RecoveryUnit& ru,
              StringData ident,
              int64_t key,
              std::span<const char> value) {
    return engine.getEngine()->insertIntoIdent(ru, ident, key, value);
}

StatusWith<UniqueBuffer> get(StorageEngine& engine,
                             RecoveryUnit& ru,
                             StringData ident,
                             std::span<const char> key) {
    return engine.getEngine()->getFromIdent(ru, ident, key);
}

StatusWith<UniqueBuffer> get(StorageEngine& engine,
                             RecoveryUnit& ru,
                             StringData ident,
                             int64_t key) {
    return engine.getEngine()->getFromIdent(ru, ident, key);
}

Status remove(StorageEngine& engine,
              RecoveryUnit& ru,
              StringData ident,
              std::span<const char> key) {
    return engine.getEngine()->deleteFromIdent(ru, ident, key);
}

Status remove(StorageEngine& engine, RecoveryUnit& ru, StringData ident, int64_t key) {
    return engine.getEngine()->deleteFromIdent(ru, ident, key);
}

}  // namespace mongo::storage_engine_direct_crud
