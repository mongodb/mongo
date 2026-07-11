// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/record_store_test_harness.h"

#include "mongo/db/storage/storage_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>
#include <unordered_map>

namespace mongo {
using namespace std::literals::string_view_literals;
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
                                               std::string_view failPointKind,
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

void registerWriteConflictForWritesFactory(std::string_view engineName,
                                           WriteConflictFailPointFn factory) {
    auto [it, inserted] =
        writeConflictForWritesHandlers().emplace(std::string{engineName}, std::move(factory));
    invariant(inserted,
              str::stream() << "write-conflict-for-writes factory already registered for engine "
                            << engineName);
}

void registerWriteConflictForReadsFactory(std::string_view engineName,
                                          WriteConflictFailPointFn factory) {
    auto [it, inserted] =
        writeConflictForReadsHandlers().emplace(std::string{engineName}, std::move(factory));
    invariant(inserted,
              str::stream() << "write-conflict-for-reads factory already registered for engine "
                            << engineName);
}

std::unique_ptr<FailPointEnableBlock> enableWriteConflictForWrites(FailPoint::ModeOptions mode) {
    return dispatch(
        writeConflictForWritesHandlers(), "write-conflict-for-writes"sv, std::move(mode));
}

std::unique_ptr<FailPointEnableBlock> enableWriteConflictForReads(FailPoint::ModeOptions mode) {
    return dispatch(writeConflictForReadsHandlers(), "write-conflict-for-reads"sv, std::move(mode));
}
}  // namespace mongo
