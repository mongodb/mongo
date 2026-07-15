// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer.h"

#include <span>
#include <string_view>

[[MONGO_MOD_PUBLIC]];

/**
 * An API for performing writes directly to the 'StorageEngine.'
 */
namespace mongo::storage_engine_direct_crud {

// This function has the same behavior as KVEngine::insertIntoIdent()
Status insert(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              std::span<const char> key,
              std::span<const char> value,
              BlindWritePolicy policy = BlindWritePolicy::nonBlind);

// This function has the same behavior as KVEngine::insertIntoIdent()
Status insert(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              int64_t key,
              std::span<const char> value,
              BlindWritePolicy policy = BlindWritePolicy::nonBlind);


// Inserts every key in 'keys' with the same 'value', as a batched (range) write. Stops and returns
// the first failing status, leaving the enclosing WriteUnitOfWork to roll back partial work.
Status insert(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              std::span<std::span<const char>> keys,
              std::span<const char> value,
              BlindWritePolicy policy = BlindWritePolicy::nonBlind);

// This function has the same behavior as KVEngine::updateInIdent()
Status update(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              std::span<const char> key,
              std::span<const char> value,
              BlindWritePolicy policy = BlindWritePolicy::nonBlind);

// This function has the same behavior as KVEngine::updateInIdent()
Status update(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              int64_t key,
              std::span<const char> value,
              BlindWritePolicy policy = BlindWritePolicy::nonBlind);

// This function has the same behavior as KVEngine::getFromIdent()
StatusWith<UniqueBuffer> get(StorageEngine& engine,
                             RecoveryUnit& ru,
                             std::string_view ident,
                             std::span<const char> key);

// This function has the same behavior as KVEngine::getFromIdent()
StatusWith<UniqueBuffer> get(StorageEngine& engine,
                             RecoveryUnit& ru,
                             std::string_view ident,
                             int64_t key);

// This function has the same behavior as KVEngine::deleteFromIdent()
Status remove(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              std::span<const char> key,
              BlindWritePolicy policy = BlindWritePolicy::nonBlind);

// This function has the same behavior as KVEngine::deleteFromIdent()
Status remove(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              int64_t key,
              BlindWritePolicy policy = BlindWritePolicy::nonBlind);

// Removes every key in 'keys', as a batched (range) write. Stops and returns the first failing
// status, leaving the enclosing WriteUnitOfWork to roll back partial work.
Status remove(StorageEngine& engine,
              RecoveryUnit& ru,
              std::string_view ident,
              std::span<std::span<const char>> keys,
              BlindWritePolicy policy = BlindWritePolicy::nonBlind);

}  // namespace mongo::storage_engine_direct_crud
