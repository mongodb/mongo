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

#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"

#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/truncate_range_oplog_entry_gen.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_uncommitted_changes.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/unittest/unittest.h"

namespace mongo::replicated_fast_count_test_helpers {

void checkFastCountMetadataInInternalCollection(OperationContext* opCtx,
                                                const UUID& uuid,
                                                bool expectPersisted,
                                                int64_t expectedCount,
                                                int64_t expectedSize) {
    {
        AutoGetCollection fastCountColl(
            opCtx,
            NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore),
            LockMode::MODE_IS);

        BSONObj persisted;
        bool found = Helpers::findById(opCtx, fastCountColl->ns(), BSON("_id" << uuid), persisted);

        EXPECT_EQ(found, expectPersisted);
        if (!expectPersisted) {
            return;
        }
        int64_t persistedCount = persisted.getField(replicated_fast_count::kMetadataKey)
                                     .Obj()
                                     .getField(replicated_fast_count::kCountKey)
                                     .Long();
        int64_t persistedSize = persisted.getField(replicated_fast_count::kMetadataKey)
                                    .Obj()
                                    .getField(replicated_fast_count::kSizeKey)
                                    .Long();
        EXPECT_EQ(persistedCount, expectedCount);
        EXPECT_EQ(persistedSize, expectedSize);

        ASSERT_TRUE(persisted.hasField(replicated_fast_count::kValidAsOfKey));
    }
}

void checkUncommittedFastCountChanges(OperationContext* opCtx,
                                      const UUID& uuid,
                                      int64_t expectedCount,
                                      int64_t expectedSize) {
    auto uncommittedChanges = UncommittedFastCountChange::getForRead(opCtx);
    auto uncommittedSizeAndCount = uncommittedChanges.find(uuid);

    EXPECT_EQ(uncommittedSizeAndCount.count, expectedCount);
    EXPECT_EQ(uncommittedSizeAndCount.size, expectedSize);
}

void checkCommittedFastCountChanges(const UUID& uuid,
                                    ReplicatedFastCountManager* fastCountManager,
                                    int64_t expectedCount,
                                    int64_t expectedSize) {
    auto committedSizeAndCount = fastCountManager->find(uuid);

    EXPECT_EQ(committedSizeAndCount.count, expectedCount);
    EXPECT_EQ(committedSizeAndCount.size, expectedSize);
}

void insertDocs(OperationContext* opCtx,
                ReplicatedFastCountManager* fastCountManager,
                const NamespaceString& nss,
                int numDocs,
                int64_t startingCount,
                int64_t startingSize,
                const std::function<BSONObj(int)>& makeDoc,
                const BSONObj& sampleDoc,
                bool abortWithoutCommit) {

    AutoGetCollection coll(opCtx, nss, LockMode::MODE_IX);

    {
        WriteUnitOfWork wuow{opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        for (int i = startingCount; i < startingCount + numDocs; ++i) {
            BSONObj doc = makeDoc(i);
            ASSERT_OK(Helpers::insert(opCtx, *coll, doc));
        }
        checkUncommittedFastCountChanges(
            opCtx, coll->uuid(), numDocs, numDocs * sampleDoc.objsize());
        checkCommittedFastCountChanges(coll->uuid(), fastCountManager, startingCount, startingSize);
        if (!abortWithoutCommit) {
            wuow.commit();
        }
    }

    if (abortWithoutCommit) {
        checkCommittedFastCountChanges(coll->uuid(), fastCountManager, startingCount, startingSize);
    } else {
        checkCommittedFastCountChanges(coll->uuid(),
                                       fastCountManager,
                                       startingCount + numDocs,
                                       startingSize + numDocs * sampleDoc.objsize());
    }

    checkUncommittedFastCountChanges(opCtx, coll->uuid(), 0, 0);
}

void updateDocs(OperationContext* opCtx,
                ReplicatedFastCountManager* fastCountManager,
                const NamespaceString& nss,
                int startIdx,
                int endIdx,
                int64_t startingCount,
                int64_t startingSize,
                const std::function<BSONObj(int)>& makeUpdatedDoc,
                const BSONObj& sampleDocBeforeUpdate,
                const BSONObj& sampleDocAfterUpdate) {

    ASSERT(endIdx >= startIdx);
    auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);

    const int64_t sizeDelta = sampleDocAfterUpdate.objsize() - sampleDocBeforeUpdate.objsize();
    const int numTotalUpdates = endIdx - startIdx + 1;

    {
        WriteUnitOfWork wuow{opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        for (int i = startIdx; i <= endIdx; ++i) {
            BSONObj updated = makeUpdatedDoc(i);
            Helpers::update(opCtx, coll, BSON("_id" << i), BSON("$set" << updated));
        }
        checkCommittedFastCountChanges(coll.uuid(), fastCountManager, startingCount, startingSize);
        checkUncommittedFastCountChanges(opCtx, coll.uuid(), 0, numTotalUpdates * sizeDelta);
        wuow.commit();
    }

    checkCommittedFastCountChanges(
        coll.uuid(), fastCountManager, startingCount, startingSize + numTotalUpdates * sizeDelta);
    checkUncommittedFastCountChanges(opCtx, coll.uuid(), 0, 0);
}

void deleteDocsByIDRange(OperationContext* opCtx,
                         ReplicatedFastCountManager* fastCountManager,
                         const NamespaceString& nss,
                         int startIdx,
                         int endIdx,
                         int64_t startingCount,
                         int64_t startingSize,
                         const BSONObj& sampleDoc) {
    ASSERT(endIdx >= startIdx);

    auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);

    const int numTotalDeletes = endIdx - startIdx + 1;
    ASSERT(numTotalDeletes <= startingCount);
    ASSERT(numTotalDeletes * sampleDoc.objsize() <= startingSize);
    {
        WriteUnitOfWork wuow{opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        for (int i = startIdx; i <= endIdx; ++i) {
            RecordId rid = Helpers::findOne(opCtx, coll, BSON("_id" << i));
            Helpers::deleteByRid(opCtx, coll, rid);
        }
        checkCommittedFastCountChanges(coll.uuid(), fastCountManager, startingCount, startingSize);
        checkUncommittedFastCountChanges(
            opCtx, coll.uuid(), -numTotalDeletes, -numTotalDeletes * sampleDoc.objsize());
        wuow.commit();
    }

    checkCommittedFastCountChanges(coll.uuid(),
                                   fastCountManager,
                                   startingCount - numTotalDeletes,
                                   startingSize - (numTotalDeletes)*sampleDoc.objsize());
    checkUncommittedFastCountChanges(opCtx, coll.uuid(), 0, 0);
}

std::vector<repl::OplogEntry> getOplogEntriesMatching(
    OperationContext* opCtx, std::function<bool(const repl::OplogEntry&)> predicate) {

    repl::OplogInterfaceLocal oplogInterface(opCtx);
    auto oplogIter = oplogInterface.makeIterator();

    std::vector<repl::OplogEntry> matchedEntries;
    while (true) {
        auto sw = oplogIter->next();
        if (sw.getStatus() == ErrorCodes::CollectionIsEmpty) {
            break;
        }
        ASSERT_OK(sw.getStatus());
        auto obj = sw.getValue().first.getOwned();

        auto swEntry = repl::OplogEntry::parse(obj);
        ASSERT_OK(swEntry.getStatus());
        const auto& entry = swEntry.getValue();
        if (predicate(entry)) {
            matchedEntries.push_back(entry);
        }
    }

    // Iterator returns newest-first; reverse to get ascending ts.
    std::reverse(matchedEntries.begin(), matchedEntries.end());

    return matchedEntries;
}

std::vector<repl::OplogEntry> getApplyOpsForNss(OperationContext* opCtx,
                                                const NamespaceString& innerNss) {

    auto predicate = [&](const repl::OplogEntry& entry) {
        if (entry.getOpType() != repl::OpTypeEnum::kCommand ||
            entry.getCommandType() != repl::OplogEntry::CommandType::kApplyOps) {
            return false;
        }

        std::vector<repl::OplogEntry> inner;
        repl::ApplyOps::extractOperationsTo(entry, entry.getEntry().toBSON(), &inner);

        for (const auto& innerEntry : inner) {
            if (innerEntry.getNss() == innerNss) {
                return true;
            }
        }
        return false;
    };

    return getOplogEntriesMatching(opCtx, predicate);
}

repl::OplogEntry getLatestApplyOpsForNss(OperationContext* opCtx, const NamespaceString& innerNss) {
    auto entries = getApplyOpsForNss(opCtx, innerNss);
    EXPECT_FALSE(entries.empty()) << "Expected at least one applyOps entry for "
                                  << innerNss.toStringForErrorMsg();
    return entries.back();
}

void assertFastCountApplyOpsMatches(const repl::OplogEntry& applyOpsEntry,
                                    const NamespaceString& internalNss,
                                    const std::vector<ExpectedFastCountOp>& expectedOps) {

    EXPECT_EQ(repl::OpTypeEnum::kCommand, applyOpsEntry.getOpType());
    EXPECT_EQ(repl::OplogEntry::CommandType::kApplyOps, applyOpsEntry.getCommandType());
    EXPECT_EQ("admin.$cmd"_sd, applyOpsEntry.getNss().ns_forTest());

    // Index expectations by UUID so we can match each inner op to one expected op.
    std::map<UUID, ExpectedFastCountOp> expectedByUuid;
    for (const auto& e : expectedOps) {
        auto [it, inserted] = expectedByUuid.emplace(e.uuid, e);
        EXPECT_TRUE(inserted) << "Duplicate expected UUID in test: " << e.uuid;
    }

    std::vector<repl::OplogEntry> innerOperations;
    repl::ApplyOps::extractOperationsTo(
        applyOpsEntry, applyOpsEntry.getEntry().toBSON(), &innerOperations);
    int seenFastCountOps = 0;

    const auto timestampStoreNss = NamespaceString::makeGlobalConfigCollection(
        NamespaceString::kReplicatedFastCountStoreTimestamps);

    for (const auto& innerEntry : innerOperations) {
        if (innerEntry.getNss() == timestampStoreNss) {
            // TODO SERVER-123384: Add explicit validation for the timestamp store writes.
            //
            // Timestamp-store ops are written alongside metadata ops in the same applyOps; skip
            // them here since they aren't part of the per-collection metadata being validated.
            continue;
        }

        EXPECT_EQ(internalNss, innerEntry.getNss())
            << "Found unexpected non-fast-count operation in applyOps payload. "
            << applyOpsEntry.toStringForLogging() << " Inner operation "
            << innerEntry.toStringForLogging();

        EXPECT_EQ(repl::OplogEntry::CommandType::kNotCommand, innerEntry.getCommandType());

        FastCountOpType observedType = FastCountOpType::kInsert;
        UUID uuid = UUID::gen();

        // Get the UUID from the entry.
        switch (innerEntry.getOpType()) {
            case repl::OpTypeEnum::kInsert: {
                observedType = FastCountOpType::kInsert;
                const auto& obj = innerEntry.getObject();
                auto idElem = obj["_id"];
                uuid = UUID::parse(idElem).getValue();
                break;
            }
            case repl::OpTypeEnum::kUpdate: {
                observedType = FastCountOpType::kUpdate;
                const auto& o2 = innerEntry.getObject2();
                EXPECT_TRUE(o2);
                auto idElem = o2->getField("_id");
                uuid = UUID::parse(idElem).getValue();
                break;
            }
            default: {
                FAIL(std::string("Unexpected opType for observed fast-count applyOps entry: ") +
                     std::string{idl::serialize(innerEntry.getOpType())});
                break;
            }
        }

        auto it = expectedByUuid.find(uuid);
        if (it == expectedByUuid.end()) {
            continue;
        }

        const auto& expected = it->second;
        EXPECT_EQ(expected.opType, observedType) << "Mismatched op type for UUID " << uuid;

        switch (observedType) {
            case FastCountOpType::kInsert: {
                const auto& obj = innerEntry.getObject();

                auto metaElem = obj[replicated_fast_count::kMetadataKey];
                EXPECT_TRUE(metaElem.isABSONObj())
                    << "Meta field not numeric for UUID " << uuid << ": " << metaElem;

                auto metaObj = metaElem.Obj();
                auto countElem = metaObj[replicated_fast_count::kCountKey];
                auto sizeElem = metaObj[replicated_fast_count::kSizeKey];

                EXPECT_TRUE(countElem.isNumber())
                    << "Count field not numeric for UUID " << uuid << ": " << countElem;
                EXPECT_TRUE(sizeElem.isNumber())
                    << "Size field not numeric for UUID " << uuid << ": " << sizeElem;

                auto actualCount = countElem.safeNumberLong();
                auto actualSize = sizeElem.safeNumberLong();

                if (expected.expectedCount) {
                    EXPECT_EQ(*expected.expectedCount, actualCount)
                        << "Mismatched fast-count 'count' for UUID " << uuid;
                }
                if (expected.expectedSize) {
                    EXPECT_EQ(*expected.expectedSize, actualSize)
                        << "Mismatched fast-count 'size' for UUID " << uuid;
                }
                break;
            }
            case FastCountOpType::kUpdate: {
                const auto& obj = innerEntry.getObject();

                // Extract the size from the update diff. The diff algorithm may either use a
                // subdiff for the meta object (smeta: {u: {sz: N}}) or replace the whole meta
                // field as part of the top-level update (u: {meta: {sz: N}}), depending on which
                // representation is more compact. In practice, the former will be used when we are
                // updating only one field (sz) and the latter will be used when we replace both
                // fields.
                BSONElement sizeElem;
                {
                    auto diffField = obj.getField("diff");
                    ASSERT_TRUE(diffField.isABSONObj())
                        << "Expected 'diff' object in kUpdate op for UUID " << uuid << ": "
                        << obj.toString();
                    const BSONObj diffBson = diffField.Obj();

                    const std::string smetaKey =
                        "s" + std::string(replicated_fast_count::kMetadataKey);
                    auto smetaField = diffBson.getField(smetaKey);
                    if (smetaField.isABSONObj()) {
                        // Subdiff format: {diff: {smeta: {u: {sz: N}}}}
                        const BSONObj smetaBson = smetaField.Obj();
                        auto uField = smetaBson.getField("u");
                        if (uField.isABSONObj()) {
                            const BSONObj uBson = uField.Obj();
                            sizeElem = uBson.getField(replicated_fast_count::kSizeKey);
                        }
                    } else {
                        // Full-replacement format: {diff: {u: {meta: {sz: N}}}}
                        auto uField = diffBson.getField("u");
                        if (uField.isABSONObj()) {
                            const BSONObj uBson = uField.Obj();
                            auto metaField = uBson.getField(replicated_fast_count::kMetadataKey);
                            if (metaField.isABSONObj()) {
                                const BSONObj metaBson = metaField.Obj();
                                sizeElem = metaBson.getField(replicated_fast_count::kSizeKey);
                            }
                        }
                    }
                }
                EXPECT_TRUE(sizeElem.isNumber())
                    << "Size field not numeric for UUID " << uuid << ": " << sizeElem;

                auto actualNewSize = sizeElem.safeNumberLong();

                if (expected.expectedSize) {
                    EXPECT_EQ(*expected.expectedSize, actualNewSize)
                        << "Mismatched fast-count 'size' for UUID " << uuid;
                }

                const auto& o2 = innerEntry.getObject2();
                ASSERT_BSONOBJ_EQ(o2.get(), BSON("_id" << uuid));
                break;
            }
            default: {
                FAIL(std::string("Unexpected opType for inputted fast-count applyOps entry: ") +
                     std::string{idl::serialize(innerEntry.getOpType())});
                break;
            }
        }

        ++seenFastCountOps;
    }

    // Ensure we saw every expected UUID exactly once.
    EXPECT_EQ(seenFastCountOps, expectedByUuid.size())
        << "Expected " << expectedByUuid.size() << " fast-count ops in applyOps, saw "
        << seenFastCountOps;
}

void assertReplicatedSizeCountMeta(const repl::OplogEntry& oplogEntry, int32_t expectedSizeDelta) {
    const auto& entrySizeMeta = oplogEntry.getSizeMetadata();
    ASSERT_TRUE(entrySizeMeta.has_value());
    const auto* perOpMeta = std::get_if<SingleOpSizeMetadata>(&entrySizeMeta.value());
    ASSERT_NE(perOpMeta, nullptr);
    EXPECT_EQ(perOpMeta->getSz(), expectedSizeDelta);
}

void assertOpMatchesSpec(const repl::OplogEntry& oplogEntry, const OpValidationSpec& entrySpec) {
    ASSERT_EQ(entrySpec.uuid, oplogEntry.getUuid());
    ASSERT_EQ(entrySpec.opType, oplogEntry.getOpType());
    assertReplicatedSizeCountMeta(oplogEntry, entrySpec.expectedSizeDelta);
}

void assertOpsMatchSpecs(const std::vector<repl::OplogEntry>& oplogEntries,
                         const std::vector<OpValidationSpec>& entrySpecs) {
    ASSERT_EQ(entrySpecs.size(), oplogEntries.size());
    for (size_t i = 0; i < oplogEntries.size(); i++) {
        const auto opSpec = entrySpecs[i];
        const auto opEntry = oplogEntries[i];
        replicated_fast_count_test_helpers::assertOpMatchesSpec(opEntry, opSpec);
    }
}

boost::optional<repl::OplogEntry> getMostRecentOplogEntry(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          const repl::OpTypeEnum& opType) {
    repl::OplogInterfaceLocal oplogInterface(opCtx);
    auto oplogIter = oplogInterface.makeIterator();

    while (true) {
        auto sw = oplogIter->next();
        if (sw.getStatus() == ErrorCodes::CollectionIsEmpty) {
            break;
        }
        ASSERT_OK(sw.getStatus());
        auto obj = sw.getValue().first.getOwned();

        auto swEntry = repl::OplogEntry::parse(obj);
        ASSERT_OK(swEntry.getStatus());
        const auto& entry = swEntry.getValue();
        if (entry.getNss() == nss && entry.getOpType() == opType) {
            return entry;
        }
    }
    return boost::none;  // No oplog entry found.
}

CollectionSizeCount scanForAccurateSizeCount(OperationContext* opCtx, const NamespaceString& nss) {
    auto collAcq = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);

    auto cursor = collAcq.getCollectionPtr()->getCursor(opCtx, /*forward*/ true);

    CollectionSizeCount sizeCount;
    for (auto record = cursor->next(); record; record = cursor->next()) {
        sizeCount.count++;
        sizeCount.size += record->data.size();
    }
    return sizeCount;
}

absl::flat_hash_map<UUID, CollectionSizeCount> extractSizeCountDeltasForApplyOps(
    const repl::OplogEntry& applyOpsEntry, const boost::optional<UUID>& uuidFilter) {
    replicated_fast_count::SizeCountDeltas entries;
    replicated_fast_count::extractSizeCountDeltasForApplyOps(applyOpsEntry, uuidFilter, entries);
    absl::flat_hash_map<UUID, CollectionSizeCount> deltas;
    for (const auto& [uuid, entry] : entries) {
        deltas[uuid] = entry.sizeCount;
    }
    return deltas;
}

}  // namespace mongo::replicated_fast_count_test_helpers

namespace mongo::replicated_fast_count::test_helpers {
namespace {
repl::OplogEntrySizeMetadata makeOperationSizeMetadata(int32_t replicatedSizeDelta) {
    SingleOpSizeMetadata m;
    m.setSz(replicatedSizeDelta);
    return m;
}
}  // namespace

repl::OplogEntry makeOplogEntry(Timestamp ts,
                                NsAndUUID userColl,
                                repl::OpTypeEnum opType,
                                int32_t sizeDelta) {
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(ts, 1),
        .opType = opType,
        .nss = userColl.nss,
        .uuid = userColl.uuid,
        .oField = BSONObj(),
        .sizeMetadata = makeOperationSizeMetadata(sizeDelta),
        .wallClockTime = Date_t::now(),
    }};
}

repl::OplogEntry makeOplogEntry(const Timestamp ts, NsAndUUID userColl, repl::OpTypeEnum opType) {
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(ts, 1),
        .opType = opType,
        .nss = userColl.nss,
        .uuid = userColl.uuid,
        .oField = BSONObj(),
        .wallClockTime = Date_t::now(),
    }};
}

repl::OplogEntry makeTruncateRangeOplogEntry(Timestamp ts,
                                             NsAndUUID userColl,
                                             int64_t bytesDeleted,
                                             int64_t docsDeleted) {
    TruncateRangeOplogEntry objectEntry(
        userColl.nss, RecordId(), RecordId(), bytesDeleted, docsDeleted);
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(ts, 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = userColl.nss.getCommandNS(),
        .uuid = userColl.uuid,
        .oField = objectEntry.toBSON(),
        .wallClockTime = Date_t::now(),
    }};
}

repl::OplogEntry makeCreateOplogEntry(Timestamp ts, NsAndUUID userColl) {
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(ts, 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = userColl.nss.getCommandNS(),
        .uuid = userColl.uuid,
        .oField = BSON("create" << userColl.nss.coll()),
        .wallClockTime = Date_t::now(),
    }};
}

repl::OplogEntry makeDropOplogEntry(Timestamp ts, NsAndUUID userColl) {
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(ts, 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = userColl.nss.getCommandNS(),
        .uuid = userColl.uuid,
        .oField = BSON("drop" << userColl.nss.coll()),
        .wallClockTime = Date_t::now(),
    }};
}

void writeToOplog(OperationContext* opCtx, const repl::OplogEntry& oplogEntry) {
    InsertStatement insert(oplogEntry.getEntry().toBSON());
    insert.replicatedRecordId = massertStatusOK(
        record_id_helpers::keyForOptime(oplogEntry.getTimestamp(), KeyFormat::Long));

    AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);
    const auto& coll = oplogWrite.getCollection();
    WriteUnitOfWork wuow(opCtx);
    ASSERT_OK(collection_internal::insertDocument(opCtx, coll, insert, nullptr));
    wuow.commit();
}

void insertSizeCountEntry(OperationContext* opCtx,
                          SizeCountStore& store,
                          UUID uuid,
                          const SizeCountStore::Entry& entry) {
    WriteUnitOfWork wuow(opCtx);
    store.write(opCtx, uuid, entry);
    wuow.commit();
}

void insertSizeCountTimestamp(OperationContext* opCtx,
                              SizeCountTimestampStore& store,
                              Timestamp timestamp) {
    WriteUnitOfWork wuow(opCtx);
    store.write(opCtx, timestamp);
    wuow.commit();
}
}  // namespace mongo::replicated_fast_count::test_helpers
