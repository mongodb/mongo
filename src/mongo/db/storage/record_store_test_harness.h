// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/test_harness_helper.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <string>

namespace mongo {

class RecordStoreHarnessHelper : public HarnessHelper {
public:
    enum class Options { Standalone, ReplicationEnabled };

    virtual std::unique_ptr<RecordStore> newRecordStore(
        const RecordStore::Options& rsOptions = RecordStore::Options{}) = 0;

    std::unique_ptr<RecordStore> newRecordStore(const std::string& ns) {
        return newRecordStore(ns, RecordStore::Options{});
    }

    virtual std::unique_ptr<RecordStore> newRecordStore(const std::string& ns,
                                                        const RecordStore::Options& rsOptions) = 0;

    virtual RecordStore& oplogRecordStore() = 0;

    virtual KVEngine* getEngine() = 0;

    /**
     * For test convenience only - in general, the notion of a 'clustered' collection should exist
     * above the storage layer.
     *
     * Returns RecordStore::Options for a 'clustered' collection.
     */
    RecordStore::Options clusteredRecordStoreOptions() {
        RecordStore::Options clusteredRSOptions;
        clusteredRSOptions.keyFormat = KeyFormat::String;
        clusteredRSOptions.allowOverwrite = true;
        return clusteredRSOptions;
    }

    /**
     * Advances the stable timestamp of the engine.
     */
    void advanceStableTimestamp(Timestamp newTimestamp) {
        auto engine = getEngine();
        // Disable the callback for oldest active transaction as it blocks the timestamps from
        // advancing.
        engine->setOldestActiveTransactionTimestampCallback(
            StorageEngine::OldestActiveTransactionTimestampCallback{});
        engine->setInitialDataTimestamp(newTimestamp);
        engine->setStableTimestamp(newTimestamp, true);
        engine->checkpoint();
    }
};

void registerRecordStoreHarnessHelperFactory(
    std::function<std::unique_ptr<RecordStoreHarnessHelper>(RecordStoreHarnessHelper::Options)>
        factory);

std::unique_ptr<RecordStoreHarnessHelper> newRecordStoreHarnessHelper(
    RecordStoreHarnessHelper::Options options =
        RecordStoreHarnessHelper::Options::ReplicationEnabled);

}  // namespace mongo
