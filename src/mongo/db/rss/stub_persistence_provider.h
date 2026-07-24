// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/rss/persistence_provider.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo::rss {

/**
 * Base class for mocking a 'PersistenceProvider' implementation for testing purposes.
 * All methods from the base 'PersistenceProvider' class are implemented here so that they throw a
 * 'NotImplemented' error. Tests can use this implementation as a base and only override the methods
 * that they need and want to specialize.
 */
class [[MONGO_MOD_OPEN]] StubPersistenceProvider : public PersistenceProvider {
public:
    std::string name() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::name() not implemented");
    }

    boost::optional<Timestamp> getSentinelDataTimestamp() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::getSentinelDataTimestamp() not implemented");
    }

    std::string getWiredTigerConfig(bool wtInMemory,
                                    bool wtLogEnabled,
                                    const std::string& wtLogCompressor) const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::getWiredTigerConfig() method not implemented");
    }

    std::string getMainWiredTigerTableSettings() const override {
        uasserted(
            mongo::ErrorCodes::NotImplemented,
            "StubPersistenceProvider::getMainWiredTigerTableSettings() method not implemented");
    }

    bool shouldUseReplicatedCatalogIdentifiers() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::shouldUseReplicatedCatalogIdentifiers() method not "
                  "implemented");
    }

    bool mustUsePrimaryDrivenIndexBuilds() const override {
        return false;
    }

    bool mustUseContainerWrites() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::mustUseContainerWrites() method not implemented");
    }

    bool shouldUseReplicatedRecordIds() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::shouldUseReplicatedRecordIds() method not implemented");
    }

    bool shouldUseClusteredCollectionOplogFastPath() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::shouldUseClusteredCollectionOplogFastPath() method not "
                  "implemented");
    }

    bool shouldUseReplicatedTruncates() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::shouldUseReplicatedTruncates() method not implemented");
    }

    bool shouldUseReplicatedFastCount() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::shouldUseReplicatedFastCount() method not implemented");
    }

    bool shouldUseOplogWritesForFlowControlSampling() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::shouldUseOplogWritesForFlowControlSampling method not "
                  "implemented");
    }

    bool shouldStepDownForShutdown() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::shouldStepDownForShutdown() method not implemented");
    }

    bool shouldDelayDataAccessDuringStartup() const override {
        uasserted(
            mongo::ErrorCodes::NotImplemented,
            "StubPersistenceProvider::shouldDelayDataAccessDuringStartup() method not implemented");
    }

    bool shouldAvoidDuplicateCheckpoints() const override {
        uasserted(
            mongo::ErrorCodes::NotImplemented,
            "StubPersistenceProvider::shouldAvoidDuplicateCheckpoints() method not implemented");
    }

    bool supportsCursorReuseForExpressPathQueries() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsCursorReuseForExpressPathQueries() method not "
                  "implemented");
    }

    bool supportsLocalCollections() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsLocalCollections() method not implemented");
    }

    bool supportsUnstableCheckpoints() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsUnstableCheckpoints() method not implemented");
    }

    bool supportsPreservingPreparedTxnInPreciseCheckpoints() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsPreservingPreparedTxnInPreciseCheckpoints() "
                  "method not implemented");
    }

    bool supportsTableLogging() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsTableLogging() method not implemented");
    }

    bool supportsCrossShardTransactions() const override {
        uasserted(
            mongo::ErrorCodes::NotImplemented,
            "StubPersistenceProvider::supportsCrossShardTransactions() method not implemented");
    }

    bool supportsFindAndModifyImageCollection() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsFindAndModifyImageCollection() method not "
                  "implemented");
    }

    bool supportsPersistentOplogCapMaintainerThread() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsPersistentOplogCapMaintainerThread() method "
                  "not implemented");
    }

    bool supportsAsyncOplogMarkerGeneration() const override {
        uasserted(
            mongo::ErrorCodes::NotImplemented,
            "StubPersistenceProvider::supportsAsyncOplogMarkerGeneration() method not implemented");
    }

    bool supportsOplogSampling() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsOplogSampling() method not implemented");
    }

    bool supportsWriteConcernOptions(
        const WriteConcernOptions& writeConcernOptions) const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsWriteConcernOptions() method not implemented");
    }

    bool supportsReadConcernLevel(const repl::ReadConcernLevel& readConcernLevel) const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsReadConcernLevel method not implemented");
    }

    bool shouldDisableTransactionUpdateCoalescing() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::shouldDisableTransactionUpdateCoalescing() method not "
                  "implemented");
    }

    multiversion::FeatureCompatibilityVersion getMinimumRequiredFCV() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::getMinimumRequiredFCV() method not implemented");
    }

    const char* getWTMemoryPageMaxForOplogStrValue() const override {
        uasserted(
            mongo::ErrorCodes::NotImplemented,
            "StubPersistenceProvider::getWTMemoryPageMaxForOplogStrValue() method not implemented");
    }

    bool supportsCompaction() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsCompaction() method not implemented");
    }

    bool supportsClassicMagicRestore() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsClassicMagicRestore() method not implemented");
    }

    bool usesSchemaEpochs() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::usesSchemaEpochs() method not implemented");
    }

    /**
     * Unlike the rest of this stub, this method returns false instead of uasserting so existing
     * Stub-based test providers continue to work without each one having to add an override.
     * false is the safe default -- it matches attached storage and makes chooseBlindWriteOverwrite
     * short-circuit to the caller's default.
     */
    bool shouldUseBlindWriteWhenSafe(OperationContext*) const override {
        return false;
    }

    uint64_t getSchemaEpochForTimestamp(Timestamp ts) const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::getSchemaEpochForTimestamp() not implemented");
    }

    int getMinSnapshotHistoryWindowInSeconds() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::getMinSnapshotHistoryWindowInSeconds() method not "
                  "implemented");
    }

    void setMinSnapshotHistoryWindowInSeconds(int seconds) override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::setMinSnapshotHistoryWindowInSeconds() method not "
                  "implemented");
    }

    bool settingsProvideMajorityWriteJournalDurability(
        bool writeConcernMajorityShouldJournal) const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::settingsProvideMajorityWriteJournalDurability() method "
                  "not implemented");
    }

    bool supportsProfilingLevel(int profilingLevel) const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsProfilingLevel() method not implemented");
    }

    bool oplogHasBeenTruncated(const BSONObj& firstOplogEntry) const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::oplogHasBeenTruncated() method not implemented");
    }

    bool supportsColdCollections() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::supportsColdCollections() method not implemented");
    }

    bool supportsLegacyReplSetCommands() const override {
        uasserted(
            mongo::ErrorCodes::NotImplemented,
            "StubPersistenceProvider::supportsLegacyReplSetCommands() method not implemented");
    }

    /**
     * Unlike the rest of this stub, this method returns a FixedIntervalPolicy rather than
     * uasserting, so that existing Stub-based test providers work without each needing an override.
     */
    std::unique_ptr<CheckpointSchedulePolicy> makeCheckpointSchedulePolicy() const override {
        return createFixedIntervalPolicy();
    }
};

}  // namespace mongo::rss
