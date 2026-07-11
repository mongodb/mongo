// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/storage_engine_direct_crud.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/shared_buffer.h"

#include <span>
#include <string_view>

namespace mongo::storage_engine_direct_crud {

Status insert(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              std::span<const char> key,
              std::span<const char> value,
              BlindWritePolicy policy) {
    return engine.getEngine()->insertIntoIdent(ru, ident, key, value, policy);
}

Status insert(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              int64_t key,
              std::span<const char> value,
              BlindWritePolicy policy) {
    return engine.getEngine()->insertIntoIdent(ru, ident, key, value, policy);
}

Status update(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              std::span<const char> key,
              std::span<const char> value,
              BlindWritePolicy policy) {
    return engine.getEngine()->updateInIdent(ru, ident, key, value, policy);
}

Status update(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              int64_t key,
              std::span<const char> value,
              BlindWritePolicy policy) {
    return engine.getEngine()->updateInIdent(ru, ident, key, value, policy);
}

StatusWith<UniqueBuffer> get(StorageEngine& engine,
                             RecoveryUnit& ru,
                             std::string_view ident,
                             std::span<const char> key) {
    return engine.getEngine()->getFromIdent(ru, ident, key);
}

StatusWith<UniqueBuffer> get(StorageEngine& engine,
                             RecoveryUnit& ru,
                             std::string_view ident,
                             int64_t key) {
    return engine.getEngine()->getFromIdent(ru, ident, key);
}

Status remove(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              std::span<const char> key,
              BlindWritePolicy policy) {
    return engine.getEngine()->deleteFromIdent(ru, ident, key, policy);
}

Status remove(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              int64_t key,
              BlindWritePolicy policy) {
    return engine.getEngine()->deleteFromIdent(ru, ident, key, policy);
}

}  // namespace mongo::storage_engine_direct_crud
