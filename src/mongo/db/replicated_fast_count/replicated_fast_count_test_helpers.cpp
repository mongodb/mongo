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

#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_uncommitted_changes.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/unittest/unittest.h"

namespace mongo::replicated_fast_count_test_helpers {

void checkFastCountMetadataInFastCountStoreCollection(OperationContext* opCtx,
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

        // TODO SERVER-121625: Introduce validation for valid-as-of beyond its basic presence.
        ASSERT_TRUE(persisted.hasField(replicated_fast_count::kValidAsOfKey));
    }
}

void checkFastCountMetadataInTimestampsCollection(OperationContext* opCtx,
                                                  int32_t stripe,
                                                  bool expectedPersisted,
                                                  const Timestamp& expectedTimestamp) {
    AutoGetCollection timestampsColl(opCtx,
                                     NamespaceString::makeGlobalConfigCollection(
                                         NamespaceString::kReplicatedFastCountStoreTimestamps),
                                     LockMode::MODE_IS);

    BSONObj persisted;
    bool found = Helpers::findById(opCtx, timestampsColl->ns(), BSON("_id" << stripe), persisted);

    EXPECT_EQ(found, expectedPersisted);
    if (!expectedPersisted) {
        return;
    }
    Timestamp persistedTimestamp =
        persisted.getField(replicated_fast_count::kValidAsOfKey).timestamp();
    EXPECT_EQ(persistedTimestamp, expectedTimestamp);
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

bool isApplyOpsEntryStructureValid(const repl::OplogEntry& applyOpsEntry) {
    if (applyOpsEntry.getOpType() != repl::OpTypeEnum::kCommand) {
        return false;
    }
    if (applyOpsEntry.getCommandType() != repl::OplogEntry::CommandType::kApplyOps) {
        return false;
    }
    if (applyOpsEntry.getNss().ns_forTest() != "admin.$cmd"_sd) {
        return false;
    }

    std::vector<repl::OplogEntry> innerOperations;
    repl::ApplyOps::extractOperationsTo(
        applyOpsEntry, applyOpsEntry.getEntry().toBSON(), &innerOperations);

    for (const auto& innerEntry : innerOperations) {
        if (innerEntry.getCommandType() != repl::OplogEntry::CommandType::kNotCommand) {
            return false;
        }
    }

    return true;
}

void assertFastCountApplyOpsMatches(const repl::OplogEntry& applyOpsEntry,
                                    const std::vector<ExpectedFastCountOp>& expectedOps) {

    EXPECT_EQ(isApplyOpsEntryStructureValid(applyOpsEntry), true);

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

    for (const auto& innerEntry : innerOperations) {
        if (innerEntry.getNss() != replicatedFastCountStoreNss &&
            innerEntry.getNss() != replicatedFastCountStoreTimestampsNss) {
            FAIL("Found unexpected non-fast-count operation in applyOps payload");
        }

        if (innerEntry.getNss() == replicatedFastCountStoreTimestampsNss) {
            continue;
        }

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
                     idl::serialize(innerEntry.getOpType()));
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

                std::string kSubDiffSectionFieldPrefix = "s";
                auto sizeElem =
                    obj["diff"][kSubDiffSectionFieldPrefix + replicated_fast_count::kMetadataKey]
                       ["u"][replicated_fast_count::kSizeKey];
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
                     idl::serialize(innerEntry.getOpType()));
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

void assertFastCountTimestampsApplyOpsMatches(const repl::OplogEntry& applyOpsEntry,
                                              const ExpectedFastCountTimestampsOp& expectedOp) {

    EXPECT_EQ(isApplyOpsEntryStructureValid(applyOpsEntry), true);

    std::vector<repl::OplogEntry> innerOperations;
    repl::ApplyOps::extractOperationsTo(
        applyOpsEntry, applyOpsEntry.getEntry().toBSON(), &innerOperations);

    for (const auto& innerEntry : innerOperations) {
        if (innerEntry.getNss() != replicatedFastCountStoreNss &&
            innerEntry.getNss() != replicatedFastCountStoreTimestampsNss) {
            FAIL("Found unexpected non-fast-count operation in applyOps payload");
        }

        if (innerEntry.getNss() == replicatedFastCountStoreNss) {
            continue;
        }

        const BSONObj& obj = innerEntry.getObject();
        BSONElement idElem = obj["_id"];
        EXPECT_TRUE(idElem.isNumber());

        int32_t stripeNum = idElem.safeNumberInt();
        EXPECT_EQ(stripeNum, expectedOp.stripe) << "Mismatched stripe number for timestamp update";

        Timestamp actualTimestamp = obj[replicated_fast_count::kValidAsOfKey].timestamp();
        EXPECT_EQ(expectedOp.expectedTimestamp, actualTimestamp)
            << "Mismatched timestamp for stripe " << stripeNum;
    }
};

void assertReplicatedSizeCountMeta(const repl::OplogEntry& oplogEntry, int32_t expectedSizeDelta) {
    const auto entrySizeMeta = oplogEntry.getSizeMetadata();
    ASSERT_TRUE(entrySizeMeta.has_value());
    ASSERT_EQ(entrySizeMeta->getSz(), expectedSizeDelta);
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
    absl::flat_hash_map<UUID, CollectionSizeCount> deltas;
    replicated_fast_count::extractSizeCountDeltasForApplyOps(applyOpsEntry, uuidFilter, deltas);
    return deltas;
}

}  // namespace mongo::replicated_fast_count_test_helpers
