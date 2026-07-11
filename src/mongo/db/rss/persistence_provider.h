// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/storage/checkpoint_schedule_policy.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/modules.h"
#include "mongo/util/version/releases.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/optional.hpp>


namespace mongo {
namespace rss {

/**
 * This class provides an abstraction around the persistence layer underlying the storage and
 * replication subsystems. Depending on the configuration, the implementation may be backed by a
 * local filesystem, a remote service, etc. The interface is built primarily around capabilities and
 * expected behaviors, allowing consumers to act based on these flags, rather than needing to reason
 * about how a particular provider would behave in a given context.
 */
class [[MONGO_MOD_OPEN]] PersistenceProvider {
public:
    virtual ~PersistenceProvider() = default;

    /**
     * The name of this provider, for use in e.g. logging and error messages.
     */
    virtual std::string name() const = 0;

    /**
     * If not none, the KVEngine will use the returned Timestamp during initialization as the
     * initial data timestamp.
     */
    virtual boost::optional<Timestamp> getSentinelDataTimestamp() const = 0;

    /**
     * Additional configuration that should be added to the WiredTiger config string for the
     * 'wiredtiger_open' call.
     */
    virtual std::string getWiredTigerConfig(bool wtInMemory,
                                            bool wtLogEnabled,
                                            const std::string& wtLogCompressor) const = 0;

    /**
     * Additional configuration that should be added to the WiredTiger config string for creating a
     * new table. Only applies to the 'main' WiredTiger instance - excluding 'spill' WiredTiger
     * instances.
     */
    virtual std::string getMainWiredTigerTableSettings() const = 0;

    /**
     * If true, the provider requires that index builds will be led by the primary node and
     * replicated through the oplog.
     */
    virtual bool mustUsePrimaryDrivenIndexBuilds() const = 0;

    /**
     * If true, the provider requires that table-level operations are replicated through the oplog.
     */
    virtual bool mustUseContainerWrites() const = 0;

    /**
     * If true, the provider expects that all catalog identifiers will be replicated and identical
     * between nodes.
     */
    virtual bool shouldUseReplicatedCatalogIdentifiers() const = 0;

    /**
     * If true, the provider expects that RecordIds will be replicated (either explicitly or
     * implicitly) and identical between nodes.
     */
    virtual bool shouldUseReplicatedRecordIds() const = 0;

    /**
     * If true, oplog application for clustered-on-_id collections may bypass the query system by
     * deriving the RecordId directly from the _id field. The RecordId of a clustered-on-_id
     * collection is deterministically derivable from _id, so this produces an identical result to
     * the query path while avoiding the executor.
     */
    virtual bool shouldUseClusteredCollectionOplogFastPath() const = 0;

    /**
     * If true, expired documents should be removed using replicated truncates.
     */
    virtual bool shouldUseReplicatedTruncates() const = 0;

    /**
     * If true, fastcounts (collection size and count) will be managed with replicated writes.
     */
    virtual bool shouldUseReplicatedFastCount() const = 0;

    /**
     * If true, writes to the oplog should be used as the unit of progress for flow control
     * sampling.
     */
    virtual bool shouldUseOplogWritesForFlowControlSampling() const = 0;

    /**
     * If true, the node should step down prior to shutdown in order to minimize unavailability.
     */
    virtual bool shouldStepDownForShutdown() const = 0;

    /**
     * If true, data may not be available immediately after starting the storage engine, so systems
     * like the catalog should not be initialized immediately.
     */
    virtual bool shouldDelayDataAccessDuringStartup() const = 0;

    /**
     * If true, the system should take precautions to avoid taking multiple checkpoints for the same
     * stable timestamp. The underlying key-value engine likely does not provide the necessary
     * coordination by default.
     */
    virtual bool shouldAvoidDuplicateCheckpoints() const = 0;

    /**
     * If true, the storage provider supports the reuse of cursors in express path queries. Used to
     * disable this optimization for disaggregated storage for now.
     *
     * TODO SERVER-116261: re-enable the optimization for disaggregated storage.
     */
    virtual bool supportsCursorReuseForExpressPathQueries() const = 0;

    /**
     * If true, the storage provider supports the use of local, unreplicated collections.
     */
    virtual bool supportsLocalCollections() const = 0;

    /**
     * If true, the provider can support unstable checkpoints.
     */
    virtual bool supportsUnstableCheckpoints() const = 0;

    /**
     * If true, the provider can support preserving prepared transactions in the precise
     * checkpoints.
     */
    virtual bool supportsPreservingPreparedTxnInPreciseCheckpoints() const = 0;

    /**
     * If true, the provider can support logging (i.e. journaling) on individual tables.
     */
    virtual bool supportsTableLogging() const = 0;

    /**
     * If true, the provider supports cross-shard transactions.
     */
    virtual bool supportsCrossShardTransactions() const = 0;

    /**
     * If true, the provider supports storing findAndModify pre/post-images in the image collection.
     */
    virtual bool supportsFindAndModifyImageCollection() const = 0;

    /**
     * If true, the provider supports a persistent oplog cap maintainer thread that runs for the
     * entirety of a node's runtime.
     */
    virtual bool supportsPersistentOplogCapMaintainerThread() const = 0;

    /**
     * If true, the provider supports generating initial oplog markers asynchronously on the cap
     * maintainer thread. If false, initial oplog markers must be generated before starting the cap
     * maintainer thread.
     */
    virtual bool supportsAsyncOplogMarkerGeneration() const = 0;

    /**
     * If true, the provider supports random sampling over the oplog collection.
     */
    virtual bool supportsOplogSampling() const = 0;

    virtual bool supportsWriteConcernOptions(
        const WriteConcernOptions& writeConcernOptions) const = 0;

    virtual bool supportsReadConcernLevel(const repl::ReadConcernLevel& readConcernLevel) const = 0;

    /**
     * If true, we disable transaction update coalescing on secondaries.
     */
    virtual bool shouldDisableTransactionUpdateCoalescing() const = 0;

    /**
     * The default feature compatibility version to be used on a new cluster. Some persistence
     * providers depend on features only available on the latest FCV.
     */
    virtual multiversion::FeatureCompatibilityVersion getMinimumRequiredFCV() const = 0;

    /**
     * The default memory_page_max value to set on WT for the oplog in string format.
     */
    virtual const char* getWTMemoryPageMaxForOplogStrValue() const = 0;

    /**
     * If true, the provider supports compaction.
     */
    virtual bool supportsCompaction() const = 0;

    /**
     * If true, the provider supports using magic restore in classic mode.
     */
    virtual bool supportsClassicMagicRestore() const = 0;

    /**
     * If true, this persistence provider uses schema epochs for table creation/drop.
     */
    virtual bool usesSchemaEpochs() const = 0;

    /**
     * Returns true if the current operation may skip the read-before-write existence check that
     * the storage layer would otherwise perform on a write (a "blind write"). Skipping the check
     * avoids redundant work on non-primaries applying oplog, but is only safe when the primary
     * has already validated the write. A true return signals that the context is safe for a
     * blind write; whether one actually happens is a separate decision made at the call site.
     * Providers whose storage engines do not benefit from blind writes return false
     * unconditionally. Providers that do must also verify that the node is currently a standby.
     */
    virtual bool shouldUseBlindWriteWhenSafe(OperationContext* opCtx) const = 0;

    /**
     * Returns the schema epoch to use when passing a schema-related operation at the given
     * timestamp to WiredTiger. Returns 0 for backends that do not use schema epochs.
     */
    virtual uint64_t getSchemaEpochForTimestamp(Timestamp ts) const = 0;

    /**
     * The minimum number of seconds of snapshot history to maintain.
     */
    virtual int getMinSnapshotHistoryWindowInSeconds() const = 0;

    /**
     * Set minimum number of seconds of snapshot history to maintain.
     */
    virtual void setMinSnapshotHistoryWindowInSeconds(int seconds) = 0;

    /**
     * Whether the current settings provide guarantees of a journal / write ahead log even if not
     * explicitly asked for.
     */
    virtual bool settingsProvideMajorityWriteJournalDurability(
        bool writeConcernMajorityShouldJournal) const = 0;

    /**
     * If true, the provider supports this level on the profile command.
     */
    virtual bool supportsProfilingLevel(int profilingLevel) const = 0;

    /**
     * Returns whether the oplog has been truncated, based on inspection of the first oplog entry.
     * The check is provider-specific because the format of the initialization entry differs between
     * persistence providers.
     */
    virtual bool oplogHasBeenTruncated(const BSONObj& firstOplogEntry) const = 0;

    /**
     * If true, the provider supports cold collections.
     */
    virtual bool supportsColdCollections() const = 0;

    /**
     * If true, the provider supports replSetTestEgress and replSetGetRBID commands.
     */
    virtual bool supportsLegacyReplSetCommands() const = 0;

    /**
     * Creates and returns a new policy that governs checkpoint scheduling for this provider.
     */
    virtual std::unique_ptr<CheckpointSchedulePolicy> makeCheckpointSchedulePolicy() const = 0;
};

}  // namespace rss
}  // namespace mongo
