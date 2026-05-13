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

#include "mongo/db/storage/record_store_test_harness.h"

#include "mongo/db/storage/storage_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <unordered_map>

namespace mongo {
namespace {
std::function<std::unique_ptr<RecordStoreHarnessHelper>(RecordStoreHarnessHelper::Options)>
    recordStoreHarnessFactory;

using WriteConflictHandlerMap = std::unordered_map<std::string, WriteConflictFailPointFn>;

WriteConflictHandlerMap& writeConflictForWritesHandlers() {
    static WriteConflictHandlerMap m;
    return m;
}

WriteConflictHandlerMap& writeConflictForReadsHandlers() {
    static WriteConflictHandlerMap m;
    return m;
}

std::unique_ptr<FailPointEnableBlock> dispatch(WriteConflictHandlerMap& handlers,
                                               StringData failPointKind,
                                               FailPoint::ModeOptions mode) {
    auto it = handlers.find(storageGlobalParams.engine);
    invariant(it != handlers.end(),
              str::stream() << "no " << failPointKind << " factory registered for storage engine "
                            << storageGlobalParams.engine);
    return it->second(std::move(mode));
}
}  // namespace

void registerRecordStoreHarnessHelperFactory(
    std::function<std::unique_ptr<RecordStoreHarnessHelper>(RecordStoreHarnessHelper::Options)>
        factory) {
    recordStoreHarnessFactory = std::move(factory);
}

auto newRecordStoreHarnessHelper(RecordStoreHarnessHelper::Options options)
    -> std::unique_ptr<RecordStoreHarnessHelper> {
    return recordStoreHarnessFactory(options);
}

void registerWriteConflictForWritesFactory(StringData engineName,
                                           WriteConflictFailPointFn factory) {
    auto [it, inserted] =
        writeConflictForWritesHandlers().emplace(std::string{engineName}, std::move(factory));
    invariant(inserted,
              str::stream() << "write-conflict-for-writes factory already registered for engine "
                            << engineName);
}

void registerWriteConflictForReadsFactory(StringData engineName, WriteConflictFailPointFn factory) {
    auto [it, inserted] =
        writeConflictForReadsHandlers().emplace(std::string{engineName}, std::move(factory));
    invariant(inserted,
              str::stream() << "write-conflict-for-reads factory already registered for engine "
                            << engineName);
}

std::unique_ptr<FailPointEnableBlock> enableWriteConflictForWrites(FailPoint::ModeOptions mode) {
    return dispatch(
        writeConflictForWritesHandlers(), "write-conflict-for-writes"_sd, std::move(mode));
}

std::unique_ptr<FailPointEnableBlock> enableWriteConflictForReads(FailPoint::ModeOptions mode) {
    return dispatch(
        writeConflictForReadsHandlers(), "write-conflict-for-reads"_sd, std::move(mode));
}
}  // namespace mongo
