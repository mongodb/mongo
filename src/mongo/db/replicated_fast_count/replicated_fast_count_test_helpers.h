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

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/rss/stub_persistence_provider.h"
#include "mongo/util/uuid.h"

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo::replicated_fast_count_test_helpers {
/**
 * Stub persistence provider for enabling the replicated fast count collection.
 */
class ReplicatedFastCountTestPersistenceProvider : public rss::StubPersistenceProvider {
    boost::optional<Timestamp> getSentinelDataTimestamp() const override {
        return boost::none;
    }

    std::string getWiredTigerConfig(bool, bool wtLogEnabled, const std::string&) const override {
        invariant(!wtLogEnabled);
        return "in_memory=true,log=(enabled=false),";
    }

    std::string getMainWiredTigerTableSettings() const override {
        return "";
    }

    bool shouldUseReplicatedCatalogIdentifiers() const override {
        return false;
    }

    // Enables replicated fast count collection for tests.
    bool shouldUseReplicatedFastCount() const override {
        return true;
    }

    bool shouldUseOplogWritesForFlowControlSampling() const override {
        return false;
    }

    bool shouldForceUpdateWithFullDocument() const override {
        return true;
    }

    bool shouldUseReplicatedRecordIds() const override {
        return false;
    }

    bool shouldDelayDataAccessDuringStartup() const override {
        return false;
    }

    bool shouldAvoidDuplicateCheckpoints() const override {
        return false;
    }

    bool supportsCursorReuseForExpressPathQueries() const override {
        return false;
    }

    bool supportsLocalCollections() const override {
        return true;
    }

    bool supportsUnstableCheckpoints() const override {
        return false;
    }

    bool supportsTableLogging() const override {
        return true;
    }

    bool supportsCrossShardTransactions() const override {
        return false;
    }

    bool supportsFindAndModifyImageCollection() const override {
        return false;
    }

    bool supportsWriteConcernOptions(const WriteConcernOptions&) const override {
        return true;
    }

    multiversion::FeatureCompatibilityVersion getMinimumRequiredFCV() const override {
        // (Generic FCV reference): Mock storage can operate at any FCV.
        return multiversion::GenericFCV::kLastLTS;
    }

    const char* getWTMemoryPageMaxForOplogStrValue() const override {
        return "10m";  // 10MB
    }

    bool settingsProvideMajorityWriteJournalDurability(bool) const override {
        return false;
    }

    bool shouldDeferUntimestampedDrops() const override {
        return false;
    }

    bool supportsColdCollections() const override {
        return false;
    }

    bool usesSchemaEpochs() const override {
        return false;
    }

    bool supportsPreservingPreparedTxnInPreciseCheckpoints() const override {
        return false;
    }
};

/**
 * Checks the persisted values of count and size for the given UUID in the internal
 * replicated fast count collection.
 */
void checkFastCountMetadataInInternalCollection(OperationContext* opCtx,
                                                const UUID& uuid,
                                                bool expectPersisted,
                                                int64_t expectedCount,
                                                int64_t expectedSize);

/**
 * Checks the uncommitted fast count changes for the given UUID.
 */
void checkUncommittedFastCountChanges(OperationContext* opCtx,
                                      const UUID& uuid,
                                      int64_t expectedCount,
                                      int64_t expectedSize);

/**
 * Checks the committed fast count changes for the given UUID.
 */
void checkCommittedFastCountChanges(const UUID& uuid,
                                    ReplicatedFastCountManager* fastCountManager,
                                    int64_t expectedCount,
                                    int64_t expectedSize);
/**
 * Inserts the specified number of documents into the given collection, using the provided function
 * 'makeDoc' to generate each document. Checks whether uncommitted and committed changes are updated
 * as expected. Expected size is calculated as numDocs * size of sampleDoc, where sampleDoc is a
 * representative document for the type of documents being inserted.
 *
 * If abortWithoutCommit is set to true, the WriteUnitOfWork will not be committed, simulating a
 * rollback.
 */
void insertDocs(OperationContext* opCtx,
                ReplicatedFastCountManager* fastCountManager,
                const NamespaceString& nss,
                int numDocs,
                int64_t startingCount,
                int64_t startingSize,
                const std::function<BSONObj(int)>& makeDoc,
                const BSONObj& sampleDoc,
                bool abortWithoutCommit = false);

/**
 * Updates the documents in the given collection in the id range [start, end] to the new documents
 * generated by the 'makeUpdatedDoc' function, checking expected size and count. The expected size
 * is calculated as sampleDoc.objsize() * numDocs, where sampleDoc is a document that is
 * representative of what the documents we are updating look like after the update is applied.
 */
void updateDocs(OperationContext* opCtx,
                ReplicatedFastCountManager* fastCountManager,
                const NamespaceString& nss,
                int startIdx,
                int endIdx,
                int64_t startingCount,
                int64_t startingSize,
                const std::function<BSONObj(int)>& makeUpdatedDoc,
                const BSONObj& sampleDocBeforeUpdate,
                const BSONObj& sampleDocAfterUpdate);

/**
 * Deletes documents with ids in the range [startIdx, endIdx] from the given collection.
 */
void deleteDocsByIDRange(OperationContext* opCtx,
                         ReplicatedFastCountManager* fastCountManager,
                         const NamespaceString& nss,
                         int startIdx,
                         int endIdx,
                         int64_t startingCount,
                         int64_t startingSize,
                         const BSONObj& sampleDoc);

/**
 * Return all oplog entries matching 'predicate'.
 */
std::vector<repl::OplogEntry> getOplogEntriesMatching(
    OperationContext* opCtx, std::function<bool(const repl::OplogEntry&)> predicate);

/**
 * Get applyOps oplog entries on the given namespace. Returns a vector of oplog entries sorted in
 * ascending timestamp order.
 */
std::vector<repl::OplogEntry> getApplyOpsForNss(OperationContext* opCtx,
                                                const NamespaceString& innerNss);

/**
 * Returns the most recent applyOps entry that contains an inner op on 'innerNss'.
 * Assumes getApplyOpsForNss() returns entries in ascending timestamp order.
 */
repl::OplogEntry getLatestApplyOpsForNss(OperationContext* opCtx, const NamespaceString& innerNss);

enum class FastCountOpType {
    kInsert,
    kUpdate,
    // TODO SERVER-118821: Add test cases for delete operations once we delete entries for dropped
    // collections.
};

struct ExpectedFastCountOp {
    UUID uuid;
    FastCountOpType opType;

    boost::optional<int64_t> expectedCount;
    boost::optional<int64_t> expectedSize;
};

/**
 * Asserts that the given applyOps oplog entry contains fast-count operations matching the expected
 * operations. Also performs structural checks on the applyOps entry.
 */
void assertFastCountApplyOpsMatches(const repl::OplogEntry& applyOpsEntry,
                                    const NamespaceString& internalNss,
                                    const std::vector<ExpectedFastCountOp>& expectedOps);

/**
 * Expected values for validating individual operations in an oplog entry or applyOps inner ops with
 * respect to the replicated fast count information.
 */
struct OpValidationSpec {
    /**
     * The collection UUID for the operation.
     */
    UUID uuid;

    /**
     * The operation type (e.g., insert, update, delete).
     */
    repl::OpTypeEnum opType;

    /**
     * The expected size delta for 'o2.m.sz' in the oplog entry.
     */
    int32_t expectedSizeDelta;
};

/**
 * Asserts the replicated size count information in 'oplogEntry' is 'expectedSizeDelta'.
 */
void assertReplicatedSizeCountMeta(const repl::OplogEntry& oplogEntry, int32_t expectedSizeDelta);

/**
 * Asserts the information encoded in 'oplogEntry' aligns with that of 'entrySpecs'.
 */
void assertOpMatchesSpec(const repl::OplogEntry& oplogEntry, const OpValidationSpec& entrySpecs);
void assertOpsMatchSpecs(const std::vector<repl::OplogEntry>& oplogEntries,
                         const std::vector<OpValidationSpec>& entrySpecs);

/**
 * Gets the most recent oplog entry matching 'nss' and 'opType'.
 */
boost::optional<repl::OplogEntry> getMostRecentOplogEntry(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          const repl::OpTypeEnum& opType);

/**
 * Performs a scan over all the documents in 'nss' to get an accurate total size and count for the
 * collection.
 */
CollectionSizeCount scanForAccurateSizeCount(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Convenience wrapper around extractSizeCountDeltasForApplyOps that constructs and returns the
 * result map.
 */
absl::flat_hash_map<UUID, CollectionSizeCount> extractSizeCountDeltasForApplyOps(
    const repl::OplogEntry& applyOpsEntry, const boost::optional<UUID>& uuidFilter = boost::none);

}  // namespace mongo::replicated_fast_count_test_helpers

namespace mongo::replicated_fast_count::test_helpers {
/**
 * Simple wrapper to ease creation and testing of replicated fast count and size.
 */
struct NsAndUUID {
    NamespaceString nss;
    UUID uuid;
};

/**
 * Generates an oplog entry with the provided inputs and placeholders for all other required fields.
 */
repl::OplogEntry makeOplogEntry(Timestamp ts,
                                NsAndUUID userColl,
                                repl::OpTypeEnum opType,
                                int32_t sizeDelta);
repl::OplogEntry makeOplogEntry(Timestamp ts, NsAndUUID userColl, repl::OpTypeEnum opType);

/**
 * Generates a truncateRange command oplog entry for the given collection UUID with the specified
 * bytesDeleted and docsDeleted values.
 */
repl::OplogEntry makeTruncateRangeOplogEntry(Timestamp ts,
                                             NsAndUUID userColl,
                                             int64_t bytesDeleted,
                                             int64_t docsDeleted);

/**
 * Inserts `oplogEntry` into the oplog collection.
 */
void writeToOplog(OperationContext* opCtx, const repl::OplogEntry& oplogEntry);

/**
 * Inserts an entry into `store` for the provided `uuid`.
 */
// TODO(SERVER-122992): Assert return value is false.
void insertSizeCountEntry(OperationContext* opCtx,
                          SizeCountStore& store,
                          UUID uuid,
                          const SizeCountStore::Entry& entry);

/**
 * Inserts a timestamp into `store`.
 */
// TODO(SERVER-122992): Assert return value is false.
void insertSizeCountTimestamp(OperationContext* opCtx,
                              SizeCountTimestampStore& store,
                              Timestamp timestamp);
}  // namespace mongo::replicated_fast_count::test_helpers
