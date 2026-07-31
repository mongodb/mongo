// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/record_store_test_harness.h"

namespace mongo {
namespace {
std::function<std::unique_ptr<RecordStoreHarnessHelper>(RecordStoreHarnessHelper::Options)>
    recordStoreHarnessFactory;
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
}  // namespace mongo
