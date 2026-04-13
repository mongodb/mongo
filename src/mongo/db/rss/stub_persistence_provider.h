/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
class MONGO_MOD_OPEN StubPersistenceProvider : public PersistenceProvider {
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
        uasserted(
            mongo::ErrorCodes::NotImplemented,
            "StubPersistenceProvider::mustUsePrimaryDrivenIndexBuilds() method not implemented");
    }

    bool shouldUseReplicatedRecordIds() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::shouldUseReplicatedRecordIds() method not implemented");
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

    bool shouldForceUpdateWithFullDocument() const override {
        uasserted(mongo::ErrorCodes::NotImplemented,
                  "StubPersistenceProvider::shouldForceUpdateWithFullDocument method not "
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
};

}  // namespace mongo::rss
