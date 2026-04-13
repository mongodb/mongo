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

#include "mongo/bson/timestamp.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {
std::unique_ptr<RecordStore> createRecordStore(StorageEngine* storageEngine,
                                               OperationContext* opCtx,
                                               StringData ident,
                                               KeyFormat keyFormat) {
    auto rs = storageEngine->makeInternalRecordStore(opCtx, ident, keyFormat);

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    ru.onRollback([ident = std::string(ident)](OperationContext* rollbackOpCtx) {
        auto se = rollbackOpCtx->getServiceContext()->getStorageEngine();
        se->addDropPendingIdent(StorageEngine::Immediate{}, std::make_shared<Ident>(ident));
    });

    return rs;
}

std::unique_ptr<RecordStore> openExistingRecordStore(StorageEngine* storageEngine,
                                                     OperationContext* opCtx,
                                                     StringData ident,
                                                     KeyFormat keyFormat) {
    auto engine = storageEngine->getEngine();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    return engine->getInternalRecordStore(ru, ident, keyFormat);
}
}  // namespace

LazyRecordStore::LazyRecordStore(OperationContext* opCtx, StringData ident, CreateMode createMode)
    : _tableOrIdent([&]() -> decltype(_tableOrIdent) {
          auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
          switch (createMode) {
              case CreateMode::immediate: {
                  WriteUnitOfWork wuow(opCtx);
                  auto rs = createRecordStore(storageEngine, opCtx, ident, KeyFormat::Long);
                  wuow.commit();
                  return rs;
              }
              case CreateMode::deferred:
                  return std::string{ident};
              case CreateMode::openExisting: {
                  auto rs = openExistingRecordStore(storageEngine, opCtx, ident, KeyFormat::Long);
                  return rs;
              }
              default:
                  MONGO_UNREACHABLE;
          }
      }()) {}

RecordStore& LazyRecordStore::_getOrCreateRecordStore(OperationContext* opCtx) {
    if (auto ident = std::get_if<std::string>(&_tableOrIdent)) {
        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        WriteUnitOfWork wuow(opCtx);
        _tableOrIdent = createRecordStore(storageEngine, opCtx, *ident, KeyFormat::Long);
        wuow.commit();
    }
    return *std::get<std::unique_ptr<RecordStore>>(_tableOrIdent);
}

RecordStore& LazyRecordStore::getOrCreateTable(OperationContext* opCtx) {
    return _getOrCreateRecordStore(opCtx);
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
