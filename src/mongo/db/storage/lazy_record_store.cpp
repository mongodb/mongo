/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/storage/lazy_record_store.h"

#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {
LazyRecordStore::~LazyRecordStore() {
    invariant(!_hasPendingCreation,
              "WriteUnitOfWork outlived LazyRecordStore which created a RecordStore");
}

std::unique_ptr<RecordStore> LazyRecordStore::_createRecordStore(OperationContext* opCtx,
                                                                 std::string_view ident,
                                                                 LazyRecordStore* lrs) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    bool nested = ru.inUnitOfWork();
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();

    WriteUnitOfWork wuow(opCtx);
    auto rs = storageEngine->makeInternalRecordStore(opCtx, ident, KeyFormat::Long);
    // When we create tables inside a nested WUOW, rollback doesn't happen until the top-level WUOW
    // is done. This is after we store the table on the LRS, so we need to go through the LRS's drop
    // path to unset the rolled-back table. In the non-nested case, the rollback happens before we
    // store the table and so we bypass that and directly call the storage engine to drop it.
    if (nested && lrs) {
        lrs->_hasPendingCreation = true;
        struct Change final : public RecoveryUnit::Change {
            LazyRecordStore* lrs;
            Change(LazyRecordStore* lrs) : lrs(lrs) {}
            void rollback(OperationContext* opCtx) noexcept final {
                lrs->_hasPendingCreation = false;
                lrs->drop(opCtx, StorageEngine::Immediate{});
            }
            void commit(OperationContext* opCtx, boost::optional<Timestamp>) noexcept final {
                lrs->_hasPendingCreation = false;
            }
        };
        ru.registerChange(std::make_unique<Change>(lrs));
    } else {
        ru.onRollback([ident](OperationContext* rollbackOpCtx) {
            auto se = rollbackOpCtx->getServiceContext()->getStorageEngine();
            se->addDropPendingIdent(StorageEngine::Immediate{}, std::make_shared<Ident>(ident));
        });
    }
    wuow.commit();
    return rs;
}

LazyRecordStore::LazyRecordStore(OperationContext* opCtx,
                                 std::string_view ident,
                                 CreateMode createMode)
    : _tableOrIdent([&]() -> decltype(_tableOrIdent) {
          auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
          switch (createMode) {
              case CreateMode::immediate:
                  return _createRecordStore(opCtx, ident, this);
              case CreateMode::deferred:
                  return std::string{ident};
              case CreateMode::openExisting: {
                  auto engine = storageEngine->getEngine();
                  auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
                  return engine->getInternalRecordStore(ru, ident, KeyFormat::Long);
              }
              default:
                  MONGO_UNREACHABLE;
          }
      }()) {}

void LazyRecordStore::createTable(OperationContext* opCtx, std::string_view ident) {
    _createRecordStore(opCtx, ident, nullptr);
}

RecordStore& LazyRecordStore::getOrCreateTable(OperationContext* opCtx) {
    if (auto ident = std::get_if<std::string>(&_tableOrIdent)) {
        _tableOrIdent = _createRecordStore(opCtx, *ident, this);
    }
    return *std::get<std::unique_ptr<RecordStore>>(_tableOrIdent);
}

bool LazyRecordStore::tableExists() const {
    return std::holds_alternative<std::unique_ptr<RecordStore>>(_tableOrIdent);
}

RecordStore& LazyRecordStore::getTableOrThrow() const {
    auto table = std::get_if<std::unique_ptr<RecordStore>>(&_tableOrIdent);
    tassert(12129700, "LazyRecordStore table has not been created", table);
    return **table;
}

void LazyRecordStore::drop(OperationContext* opCtx, StorageEngine::DropTime dropTime) {
    if (auto table = std::get_if<std::unique_ptr<RecordStore>>(&_tableOrIdent)) {
        auto identStr = std::string{(*table)->getIdent()};
        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        storageEngine->addDropPendingIdent(dropTime, std::make_shared<Ident>(identStr));
        _tableOrIdent = std::move(identStr);
    }
}

}  // namespace mongo
