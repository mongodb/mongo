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
#include "mongo/db/import_collection_oplog_entry_gen.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/container_oplog_entry_gen.h"
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
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/unittest/unittest.h"

namespace mongo::replicated_fast_count_test_helpers {

namespace {
bool findPersistedDocInCollection(OperationContext* opCtx, const UUID& uuid, BSONObj& outDoc) {
    auto fastCountColl = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx,
            NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore),
            AcquisitionPrerequisites::kRead),
        MODE_IS);

    return Helpers::findById(
        opCtx, fastCountColl.getCollectionPtr()->ns(), BSON("_id" << uuid), outDoc);
}

}  // namespace

bool findPersistedDocInContainer(OperationContext* opCtx, const UUID& uuid, BSONObj& outDoc) {
    Lock::GlobalLock globalLock(opCtx, MODE_IS);
    auto* engine = opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto rs = engine->getRecordStore(opCtx,
                                     NamespaceString::kAdminCommandNamespace,
                                     ident::kFastCountMetadataStore,
                                     RecordStore::Options{.keyFormat = KeyFormat::String},
                                     /*uuid=*/boost::none);
    auto containerVariant = rs->getContainer();
    invariant(
        std::holds_alternative<std::reference_wrapper<StringKeyedContainer>>(containerVariant),
        "Fast count metadata store is expected to be a StringKeyedContainer in container "
        "mode");
    auto& container =
        std::get<std::reference_wrapper<StringKeyedContainer>>(containerVariant).get();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto cursor = container.getCursor(ru);

    auto cdr = uuid.toCDR();
    const std::span<const char> keySpan{reinterpret_cast<const char*>(cdr.data()), cdr.length()};
    auto result = cursor->find(keySpan);
    if (!result) {
        return false;
    }
    // Copy the BSON out of the cursor-owned span so 'outDoc' outlives the cursor.
    outDoc = BSONObj(result->data()).getOwned();
    return true;
}

void checkFastCountMetadataInInternalStore(
    OperationContext* opCtx,
    replicated_fast_count::ReplicatedFastCountManager* fastCountManager,
    const UUID& uuid,
    bool expectPersisted,
    int64_t expectedCount,
    int64_t expectedSize) {
    BSONObj persisted;
    const bool found = fastCountManager->usesContainers_ForTest()
        ? findPersistedDocInContainer(opCtx, uuid, persisted)
        : findPersistedDocInCollection(opCtx, uuid, persisted);

    EXPECT_EQ(found, expectPersisted);
    if (!expectPersisted) {
        return;
    }
    const int64_t persistedCount = persisted.getField(replicated_fast_count::kMetadataKey)
                                       .Obj()
                                       .getField(replicated_fast_count::kCountKey)
                                       .Long();
    const int64_t persistedSize = persisted.getField(replicated_fast_count::kMetadataKey)
                                      .Obj()
                                      .getField(replicated_fast_count::kSizeKey)
                                      .Long();
    EXPECT_EQ(persistedCount, expectedCount);
    EXPECT_EQ(persistedSize, expectedSize);

    ASSERT_TRUE(persisted.hasField(replicated_fast_count::kValidAsOfKey));
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

void checkCommittedFastCountChanges(
    const UUID& uuid,
    replicated_fast_count::ReplicatedFastCountManager* fastCountManager,
    int64_t expectedCount,
    int64_t expectedSize) {
    auto committedSizeAndCount = fastCountManager->find(uuid);

    EXPECT_EQ(committedSizeAndCount.count, expectedCount);
    EXPECT_EQ(committedSizeAndCount.size, expectedSize);
}

void insertDocs(OperationContext* opCtx,
                replicated_fast_count::ReplicatedFastCountManager* fastCountManager,
                const NamespaceString& nss,
                int numDocs,
                int64_t startingCount,
                int64_t startingSize,
                const std::function<BSONObj(int)>& makeDoc,
                const BSONObj& sampleDoc,
                bool abortWithoutCommit) {

    auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);

    {
        WriteUnitOfWork wuow{opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        for (int i = startingCount; i < startingCount + numDocs; ++i) {
            BSONObj doc = makeDoc(i);
            ASSERT_OK(Helpers::insert(opCtx, coll.getCollectionPtr(), doc));
        }
        checkUncommittedFastCountChanges(
            opCtx, coll.uuid(), numDocs, numDocs * sampleDoc.objsize());
        checkCommittedFastCountChanges(coll.uuid(), fastCountManager, startingCount, startingSize);
        if (!abortWithoutCommit) {
            wuow.commit();
        }
    }

    if (abortWithoutCommit) {
        checkCommittedFastCountChanges(coll.uuid(), fastCountManager, startingCount, startingSize);
    } else {
        checkCommittedFastCountChanges(coll.uuid(),
                                       fastCountManager,
                                       startingCount + numDocs,
                                       startingSize + numDocs * sampleDoc.objsize());
    }

    checkUncommittedFastCountChanges(opCtx, coll.uuid(), 0, 0);
}

void updateDocs(OperationContext* opCtx,
                replicated_fast_count::ReplicatedFastCountManager* fastCountManager,
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
                         replicated_fast_count::ReplicatedFastCountManager* fastCountManager,
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
    ASSERT_FALSE(entries.empty()) << "Expected at least one applyOps entry for "
                                  << innerNss.toStringForErrorMsg();
    return entries.back();
}

std::vector<repl::OplogEntry> getApplyOpsForFastCountStore(OperationContext* opCtx) {
    const auto fastCountStoreNss =
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);

    auto predicate = [&](const repl::OplogEntry& entry) {
        if (entry.getOpType() != repl::OpTypeEnum::kCommand ||
            entry.getCommandType() != repl::OplogEntry::CommandType::kApplyOps) {
            return false;
        }

        std::vector<repl::OplogEntry> inner;
        repl::ApplyOps::extractOperationsTo(entry, entry.getEntry().toBSON(), &inner);

        for (const auto& innerEntry : inner) {
            // Collection-backed path: inner op on the fast count metadata collection.
            if (innerEntry.getNss() == fastCountStoreNss) {
                return true;
            }
            // Container-backed path: inner op is a container op on the fast count metadata ident.
            if (auto container = innerEntry.getContainer();
                container && ident::isReplicatedFastCountIdent(*container)) {
                return true;
            }
        }
        return false;
    };

    return getOplogEntriesMatching(opCtx, predicate);
}

repl::OplogEntry getLatestApplyOpsForFastCountStore(OperationContext* opCtx) {
    auto entries = getApplyOpsForFastCountStore(opCtx);
    ASSERT_FALSE(entries.empty())
        << "Expected at least one applyOps entry for the replicated fast count store";
    return entries.back();
}

namespace {
struct ObservedApplyOp {
    FastCountOpType opType{FastCountOpType::kInsert};
    UUID uuid{UUID::gen()};
    boost::optional<int64_t> observedCount;
    boost::optional<int64_t> observedSize;
};

// Parses the meta subobject of a persisted Entry BSON into count/size observed values.
void readMetaFieldsInto(const BSONObj& entryBson, ObservedApplyOp& out) {
    const auto metaField = entryBson.getField(replicated_fast_count::kMetadataKey);
    ASSERT_TRUE(metaField.isABSONObj())
        << "Entry meta field missing or not an object for UUID " << out.uuid << ": " << entryBson;
    const BSONObj metaObj = metaField.Obj();
    const auto countElem = metaObj.getField(replicated_fast_count::kCountKey);
    const auto sizeElem = metaObj.getField(replicated_fast_count::kSizeKey);
    EXPECT_TRUE(countElem.isNumber())
        << "Count field not numeric for UUID " << out.uuid << ": " << countElem;
    EXPECT_TRUE(sizeElem.isNumber())
        << "Size field not numeric for UUID " << out.uuid << ": " << sizeElem;
    out.observedCount = countElem.safeNumberLong();
    out.observedSize = sizeElem.safeNumberLong();
}

ObservedApplyOp parseCollectionInnerOp(const repl::OplogEntry& innerEntry) {
    ObservedApplyOp out;
    EXPECT_EQ(repl::OplogEntry::CommandType::kNotCommand, innerEntry.getCommandType());
    switch (innerEntry.getOpType()) {
        case repl::OpTypeEnum::kInsert: {
            out.opType = FastCountOpType::kInsert;
            const auto& obj = innerEntry.getObject();
            out.uuid = UUID::parse(obj["_id"]).getValue();

            const auto metaElem = obj[replicated_fast_count::kMetadataKey];
            EXPECT_TRUE(metaElem.isABSONObj())
                << "Meta field not an object for UUID " << out.uuid << ": " << metaElem;
            readMetaFieldsInto(obj, out);
            break;
        }
        case repl::OpTypeEnum::kUpdate: {
            out.opType = FastCountOpType::kUpdate;
            const auto& o2 = innerEntry.getObject2();
            EXPECT_TRUE(o2);
            out.uuid = UUID::parse(o2->getField("_id")).getValue();

            // Extract the size from the update diff. The diff algorithm may either use a
            // subdiff for the meta object (smeta: {u: {sz: N}}) or replace the whole meta
            // field as part of the top-level update (u: {meta: {sz: N}}), depending on which
            // representation is more compact. In practice, the former is used when only `sz`
            // changes and the latter is used when count + size both change.
            const auto& obj = innerEntry.getObject();
            const auto diffField = obj.getField("diff");
            ASSERT_TRUE(diffField.isABSONObj()) << "Expected 'diff' object in kUpdate op for UUID "
                                                << out.uuid << ": " << obj.toString();
            const BSONObj diffBson = diffField.Obj();

            const std::string smetaKey = "s" + std::string(replicated_fast_count::kMetadataKey);
            const BSONElement smetaField = diffBson.getField(smetaKey);
            BSONElement sizeElem;
            if (smetaField.isABSONObj()) {
                // Subdiff format: {diff: {smeta: {u: {sz: N}}}}
                const BSONObj smetaBson = smetaField.Obj();
                const BSONElement uField = smetaBson.getField("u");
                if (uField.isABSONObj()) {
                    sizeElem = uField.Obj().getField(replicated_fast_count::kSizeKey);
                }
            } else {
                // Full-replacement format: {diff: {u: {meta: {sz: N}}}}
                const BSONElement uField = diffBson.getField("u");
                if (uField.isABSONObj()) {
                    const BSONObj uBson = uField.Obj();
                    const BSONElement metaField =
                        uBson.getField(replicated_fast_count::kMetadataKey);
                    if (metaField.isABSONObj()) {
                        sizeElem = metaField.Obj().getField(replicated_fast_count::kSizeKey);
                    }
                }
            }
            EXPECT_TRUE(sizeElem.isNumber())
                << "Size field not numeric for UUID " << out.uuid << ": " << sizeElem;
            out.observedSize = sizeElem.safeNumberLong();

            ASSERT_BSONOBJ_EQ(o2.get(), BSON("_id" << out.uuid));
            break;
        }
        case repl::OpTypeEnum::kDelete: {
            out.opType = FastCountOpType::kDelete;
            const auto& obj = innerEntry.getObject();
            out.uuid = UUID::parse(obj["_id"]).getValue();
            break;
        }
        default: {
            FAIL(std::string("Unexpected opType for collection fast-count applyOps inner op: ") +
                 std::string{idl::serialize(innerEntry.getOpType())});
        }
    }
    return out;
}

ObservedApplyOp parseContainerInnerOp(const repl::OplogEntry& innerEntry) {
    ObservedApplyOp out;
    // Container writes carry the full Entry BSON (no diff), so both kInsert and kUpdate surface
    // both count and size.
    repl::ContainerKey key;
    repl::ContainerVal val;
    switch (innerEntry.getOpType()) {
        case repl::OpTypeEnum::kContainerInsert: {
            out.opType = FastCountOpType::kInsert;
            const auto parsed = repl::ContainerInsertOplogEntryO::parse(
                innerEntry.getObject(), IDLParserContext("ContainerInsertOplogEntryO"));
            key = parsed.getKey();
            val = parsed.getValue();
            break;
        }
        case repl::OpTypeEnum::kContainerUpdate: {
            out.opType = FastCountOpType::kUpdate;
            const auto parsed = repl::ContainerUpdateOplogEntryO::parse(
                innerEntry.getObject(), IDLParserContext("ContainerUpdateOplogEntryO"));
            key = parsed.getKey();
            val = parsed.getValue();
            break;
        }
        case repl::OpTypeEnum::kContainerDelete: {
            out.opType = FastCountOpType::kDelete;
            const auto parsed = repl::ContainerDeleteOplogEntryO::parse(
                innerEntry.getObject(), IDLParserContext("ContainerDeleteOplogEntryO"));
            key = parsed.getKey();
            break;
        }
        default: {
            FAIL(std::string("Unexpected opType for container fast-count applyOps inner op: ") +
                 std::string{idl::serialize(innerEntry.getOpType())});
            return out;
        }
    }

    ASSERT_FALSE(key.isIntKey());
    const auto keyBytes = key.getBytesKey();
    out.uuid = UUID::fromCDR(ConstDataRange(keyBytes.data(), keyBytes.data() + keyBytes.size()));

    if (out.opType == FastCountOpType::kDelete) {
        return out;
    }

    const BSONObj entryBson = BSONObj(val.data().data()).getOwned();
    readMetaFieldsInto(entryBson, out);
    return out;
}
}  // namespace

void assertFastCountApplyOpsMatches(const repl::OplogEntry& applyOpsEntry,
                                    const std::vector<ExpectedFastCountOp>& expectedOps) {
    EXPECT_EQ(repl::OpTypeEnum::kCommand, applyOpsEntry.getOpType());
    EXPECT_EQ(repl::OplogEntry::CommandType::kApplyOps, applyOpsEntry.getCommandType());
    EXPECT_EQ("admin.$cmd"_sd, applyOpsEntry.getNss().ns_forTest());

    std::map<UUID, ExpectedFastCountOp> expectedByUuid;
    for (const auto& e : expectedOps) {
        auto [it, inserted] = expectedByUuid.emplace(e.uuid, e);
        EXPECT_TRUE(inserted) << "Duplicate expected UUID in test: " << e.uuid;
    }

    std::vector<repl::OplogEntry> innerOperations;
    repl::ApplyOps::extractOperationsTo(
        applyOpsEntry, applyOpsEntry.getEntry().toBSON(), &innerOperations);

    const auto fastCountStoreNss =
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);
    const auto timestampStoreNss = NamespaceString::makeGlobalConfigCollection(
        NamespaceString::kReplicatedFastCountStoreTimestamps);

    int seenFastCountOps = 0;
    for (const auto& innerEntry : innerOperations) {
        // TODO SERVER-123384: Add explicit validation for the timestamp store writes.
        //
        // Timestamp-store ops are written alongside metadata ops in the same applyOps; skip them
        // here since they aren't part of the per-collection metadata being validated.
        ObservedApplyOp observed;
        if (auto container = innerEntry.getContainer(); container) {
            if (*container == ident::kFastCountMetadataStoreTimestamps) {
                continue;
            }
            EXPECT_EQ(ident::kFastCountMetadataStore, *container)
                << "Found unexpected non-fast-count container op in applyOps payload. "
                << applyOpsEntry.toStringForLogging() << " Inner operation "
                << innerEntry.toStringForLogging();
            observed = parseContainerInnerOp(innerEntry);
        } else {
            if (innerEntry.getNss() == timestampStoreNss) {
                continue;
            }
            EXPECT_EQ(fastCountStoreNss, innerEntry.getNss())
                << "Found unexpected non-fast-count operation in applyOps payload. "
                << applyOpsEntry.toStringForLogging() << " Inner operation "
                << innerEntry.toStringForLogging();
            observed = parseCollectionInnerOp(innerEntry);
        }

        auto it = expectedByUuid.find(observed.uuid);
        if (it == expectedByUuid.end()) {
            continue;
        }
        const auto& expected = it->second;
        EXPECT_EQ(expected.opType, observed.opType)
            << "Mismatched op type for UUID " << observed.uuid;

        if (expected.expectedCount && observed.observedCount) {
            EXPECT_EQ(*expected.expectedCount, *observed.observedCount)
                << "Mismatched fast-count 'count' for UUID " << observed.uuid;
        }
        if (expected.expectedSize && observed.observedSize) {
            EXPECT_EQ(*expected.expectedSize, *observed.observedSize)
                << "Mismatched fast-count 'size' for UUID " << observed.uuid;
        }

        ++seenFastCountOps;
    }

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

repl::OplogEntry makeImportCollectionOplogEntry(
    Timestamp ts, NsAndUUID userColl, int64_t numRecords, int64_t dataSize, bool dryRun) {

    const BSONObj catalogEntry = BSON("md" << BSON("options" << BSON("uuid" << userColl.uuid)));

    ImportCollectionOplogEntry importEntry(
        userColl.nss, UUID::gen(), numRecords, dataSize, catalogEntry, BSONObj{}, dryRun);

    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(ts, 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = userColl.nss.getCommandNS(),
        .oField = importEntry.toBSON(),
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

BSONObj makeEntryBson(int64_t count, int64_t size) {
    return BSON(replicated_fast_count::kValidAsOfKey
                << Timestamp(1, 1) << replicated_fast_count::kMetadataKey
                << BSON(replicated_fast_count::kCountKey << count << replicated_fast_count::kSizeKey
                                                         << size));
}

std::span<const char> uuidSpan(const UUID& u) {
    auto cdr = u.toCDR();
    return std::span<const char>{reinterpret_cast<const char*>(cdr.data()), cdr.length()};
}

std::span<const char> bsonSpan(const BSONObj& obj) {
    return std::span<const char>{obj.objdata(), static_cast<size_t>(obj.objsize())};
};
}  // namespace mongo::replicated_fast_count::test_helpers
