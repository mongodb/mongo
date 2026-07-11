// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/rss/attached_storage/attached_persistence_provider.h"

#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/rss/snapshot_window_options_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/checkpoint_schedule_policy.h"
#include "mongo/db/storage/oplog_truncate_marker_parameters_gen.h"

namespace mongo::rss {
namespace {
ServiceContext::ConstructorActionRegisterer registerAttachedPersistenceProvider{
    "AttachedPersistenceProvider", [](ServiceContext* service) {
        auto& rss = ReplicatedStorageService::get(service);
        rss.setPersistenceProvider(std::make_unique<AttachedPersistenceProvider>());
        rss.setSpillPersistenceProvider(std::make_unique<AttachedPersistenceProvider>());
    }};

}  // namespace

std::string AttachedPersistenceProvider::name() const {
    return "Attached Storage";
}

boost::optional<Timestamp> AttachedPersistenceProvider::getSentinelDataTimestamp() const {
    return boost::none;
}

std::string AttachedPersistenceProvider::getWiredTigerConfig(
    bool wtInMemory, bool wtLogEnabled, const std::string& wtLogCompressor) const {

    StringBuilder ss;

    if (wtInMemory) {
        invariant(!wtLogEnabled);
        // If we've requested an ephemeral instance we store everything into memory instead of
        // backing it onto disk. Logging is not supported in this instance, thus we also have to
        // disable it.
        ss << "in_memory=true,log=(enabled=false),";
    } else {
        if (wtLogEnabled) {
            ss << "log=(enabled=true,remove=true,path=journal,compressor=";
            ss << wtLogCompressor << "),";
        } else {
            ss << "log=(enabled=false),";
        }
    }

    return ss.str();
}

std::string AttachedPersistenceProvider::getMainWiredTigerTableSettings() const {
    return "";
}

bool AttachedPersistenceProvider::mustUsePrimaryDrivenIndexBuilds() const {
    return false;
}

bool AttachedPersistenceProvider::mustUseContainerWrites() const {
    return false;
}

bool AttachedPersistenceProvider::shouldUseReplicatedCatalogIdentifiers() const {
    return false;
}

bool AttachedPersistenceProvider::shouldUseReplicatedRecordIds() const {
    return false;
}

bool AttachedPersistenceProvider::shouldUseClusteredCollectionOplogFastPath() const {
    return false;
}

bool AttachedPersistenceProvider::shouldUseReplicatedTruncates() const {
    return false;
}

bool AttachedPersistenceProvider::shouldUseReplicatedFastCount() const {
    return false;
}

bool AttachedPersistenceProvider::shouldUseOplogWritesForFlowControlSampling() const {
    return true;
}

bool AttachedPersistenceProvider::shouldStepDownForShutdown() const {
    return true;
}

bool AttachedPersistenceProvider::shouldDelayDataAccessDuringStartup() const {
    return false;
}

bool AttachedPersistenceProvider::shouldAvoidDuplicateCheckpoints() const {
    return false;
}

bool AttachedPersistenceProvider::supportsCursorReuseForExpressPathQueries() const {
    return true;
}

bool AttachedPersistenceProvider::supportsLocalCollections() const {
    return true;
}

bool AttachedPersistenceProvider::supportsUnstableCheckpoints() const {
    return true;
}

bool AttachedPersistenceProvider::supportsPreservingPreparedTxnInPreciseCheckpoints() const {
    return false;
}

bool AttachedPersistenceProvider::supportsTableLogging() const {
    return true;
}

bool AttachedPersistenceProvider::supportsCrossShardTransactions() const {
    return true;
}

bool AttachedPersistenceProvider::supportsFindAndModifyImageCollection() const {
    // TODO (SERVER-117324): Remove this feature flag.
    return !gFeatureFlagDisallowFindAndModifyImageCollection.checkEnabled();
}

bool AttachedPersistenceProvider::supportsPersistentOplogCapMaintainerThread() const {
    return true;
}

bool AttachedPersistenceProvider::supportsAsyncOplogMarkerGeneration() const {
    return gOplogSamplingAsyncEnabled;
}

bool AttachedPersistenceProvider::supportsOplogSampling() const {
    return true;
}

bool AttachedPersistenceProvider::supportsWriteConcernOptions(
    const WriteConcernOptions& writeConcernOptions) const {
    return true;
}

bool AttachedPersistenceProvider::supportsReadConcernLevel(
    const repl::ReadConcernLevel& readConcernLevel) const {
    return true;
}

bool AttachedPersistenceProvider::shouldDisableTransactionUpdateCoalescing() const {
    // This is only used for testing purposes.
    return gFeatureFlagDisableTransactionUpdateCoalescing.checkEnabled();
}

multiversion::FeatureCompatibilityVersion AttachedPersistenceProvider::getMinimumRequiredFCV()
    const {
    // (Generic FCV reference): Attached storage can operate at any FCV.
    return multiversion::GenericFCV::kLastLTS;
}

const char* AttachedPersistenceProvider::getWTMemoryPageMaxForOplogStrValue() const {
    return "10m";  // 10MB
}

bool AttachedPersistenceProvider::supportsCompaction() const {
    return true;
}

bool AttachedPersistenceProvider::supportsClassicMagicRestore() const {
    return true;
}

bool AttachedPersistenceProvider::usesSchemaEpochs() const {
    return false;
}

bool AttachedPersistenceProvider::shouldUseBlindWriteWhenSafe(OperationContext*) const {
    return false;
}

uint64_t AttachedPersistenceProvider::getSchemaEpochForTimestamp(Timestamp) const {
    return 0;
}

int AttachedPersistenceProvider::getMinSnapshotHistoryWindowInSeconds() const {
    return minSnapshotHistoryWindowInSeconds.load();
}

void AttachedPersistenceProvider::setMinSnapshotHistoryWindowInSeconds(int seconds) {
    minSnapshotHistoryWindowInSeconds.store(seconds);
}

bool AttachedPersistenceProvider::settingsProvideMajorityWriteJournalDurability(
    bool writeConcernMajorityShouldJournal) const {
    return writeConcernMajorityShouldJournal;
}

bool AttachedPersistenceProvider::supportsProfilingLevel(int profilingLevel) const {
    return true;
}

bool AttachedPersistenceProvider::oplogHasBeenTruncated(const BSONObj& firstOplogEntry) const {
    repl::OplogEntryParserNonStrict parser{firstOplogEntry};
    return parser.getOpType() != repl::OpTypeEnum::kNoop ||
        parser.getObject().getStringField(repl::kNewPrimaryMsgField) != repl::kInitiatingSetMsg;
}

bool AttachedPersistenceProvider::supportsColdCollections() const {
    return false;
}

bool AttachedPersistenceProvider::supportsLegacyReplSetCommands() const {
    return true;
}

std::unique_ptr<CheckpointSchedulePolicy>
AttachedPersistenceProvider::makeCheckpointSchedulePolicy() const {
    return createFixedIntervalPolicy();
}

}  // namespace mongo::rss
