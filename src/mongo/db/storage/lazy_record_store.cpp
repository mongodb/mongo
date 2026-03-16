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
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {
std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                               StringData ident,
                                                               KeyFormat keyFormat) {
    WriteUnitOfWork wuow(opCtx);
    auto rs = opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(
        opCtx, ident, keyFormat);
    wuow.commit();
    return rs;
}
}  // namespace

std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStoreFromExistingIdent(
    OperationContext* opCtx, StringData ident, KeyFormat keyFormat) {
    WriteUnitOfWork wuow(opCtx);
    auto rs =
        opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStoreFromExistingIdent(
            opCtx, ident, keyFormat);
    wuow.commit();
    return rs;
}

LazyRecordStore::LazyRecordStore(OperationContext* opCtx, StringData ident, CreateMode createMode)
    : _tableOrIdent([&]() -> decltype(_tableOrIdent) {
          switch (createMode) {
              case CreateMode::immediate:
                  return makeTemporaryRecordStore(opCtx, ident, KeyFormat::Long);
              case CreateMode::deferred:
                  return std::string{ident};
              case CreateMode::openExisting:
                  return makeTemporaryRecordStoreFromExistingIdent(opCtx, ident, KeyFormat::Long);
          }
          MONGO_UNREACHABLE;
      }()) {}

TemporaryRecordStore& LazyRecordStore::_getOrCreateTemporaryRecordStore(OperationContext* opCtx) {
    if (auto ident = std::get_if<std::string>(&_tableOrIdent)) {
        _tableOrIdent = makeTemporaryRecordStore(opCtx, *ident, KeyFormat::Long);
    }
    return *std::get<std::unique_ptr<TemporaryRecordStore>>(_tableOrIdent);
}

void LazyRecordStore::keepTemporaryTable(OperationContext* opCtx) {
    _getOrCreateTemporaryRecordStore(opCtx).keep();
}

RecordStore& LazyRecordStore::getOrCreateTable(OperationContext* opCtx) {
    return *_getOrCreateTemporaryRecordStore(opCtx).rs();
}

RecordStore* LazyRecordStore::getTableIfExists() const {
    if (auto table = std::get_if<std::unique_ptr<TemporaryRecordStore>>(&_tableOrIdent)) {
        return (*table)->rs();
    }
    return nullptr;
}

void LazyRecordStore::drop() {
    if (auto table = std::get_if<std::unique_ptr<TemporaryRecordStore>>(&_tableOrIdent)) {
        _tableOrIdent = std::string{table->get()->rs()->getIdent()};
    }
}

}  // namespace mongo
