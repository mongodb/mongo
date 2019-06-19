/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <cstdint>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/s/op_observer_sharding_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/storage/kv/storage_engine_impl.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/stacktrace.h"

namespace mongo {
namespace {
/**
 * RAII type for operating at a timestamp. Will remove any timestamping when the object destructs.
 */
class OneOffRead {
public:
    OneOffRead(OperationContext* opCtx, const Timestamp& ts) : _opCtx(opCtx) {
        _opCtx->recoveryUnit()->abandonSnapshot();
        if (ts.isNull()) {
            _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kUnset);
        } else {
            _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, ts);
        }
    }

    ~OneOffRead() {
        _opCtx->recoveryUnit()->abandonSnapshot();
        _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kUnset);
    }

private:
    OperationContext* _opCtx;
};

class IgnorePrepareBlock {
public:
    IgnorePrepareBlock(OperationContext* opCtx) : _opCtx(opCtx) {
        _opCtx->recoveryUnit()->abandonSnapshot();
        _opCtx->recoveryUnit()->setPrepareConflictBehavior(
            PrepareConflictBehavior::kIgnoreConflicts);
    }

    ~IgnorePrepareBlock() {
        _opCtx->recoveryUnit()->abandonSnapshot();
        _opCtx->recoveryUnit()->setPrepareConflictBehavior(PrepareConflictBehavior::kEnforce);
    }

private:
    OperationContext* _opCtx;
};
}

const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

void assertIndexMetaDataMissing(const BSONCollectionCatalogEntry::MetaData& collMetaData,
                                StringData indexName) {
    const auto idxOffset = collMetaData.findIndexOffset(indexName);
    ASSERT_EQUALS(-1, idxOffset) << indexName << ". Collection Metdata: " << collMetaData.toBSON();
}

BSONCollectionCatalogEntry::IndexMetaData getIndexMetaData(
    const BSONCollectionCatalogEntry::MetaData& collMetaData, StringData indexName) {
    const auto idxOffset = collMetaData.findIndexOffset(indexName);
    ASSERT_GT(idxOffset, -1) << indexName;
    return collMetaData.indexes[idxOffset];
}

class DoNothingOplogApplierObserver : public repl::OplogApplier::Observer {
public:
    void onBatchBegin(const repl::OplogApplier::Operations&) final {}
    void onBatchEnd(const StatusWith<repl::OpTime>&, const repl::OplogApplier::Operations&) final {}
    void onMissingDocumentsFetchedAndInserted(const std::vector<FetchInfo>&) final {}
};

class StorageTimestampTest {
public:
    ServiceContext::UniqueOperationContext _opCtxRaii = cc().makeOperationContext();
    OperationContext* _opCtx = _opCtxRaii.get();
    LogicalClock* _clock = LogicalClock::get(_opCtx);

    // Set up Timestamps in the past, present, and future.
    const LogicalTime pastLt = _clock->reserveTicks(1);
    const Timestamp pastTs = pastLt.asTimestamp();
    const LogicalTime presentLt = _clock->reserveTicks(1);
    const Timestamp presentTs = presentLt.asTimestamp();
    const LogicalTime futureLt = presentLt.addTicks(1);
    const Timestamp futureTs = futureLt.asTimestamp();
    const Timestamp nullTs = Timestamp();
    const int presentTerm = 1;
    repl::ReplicationCoordinatorMock* _coordinatorMock;
    repl::ReplicationConsistencyMarkers* _consistencyMarkers;

    StorageTimestampTest() {
        repl::ReplSettings replSettings;
        replSettings.setOplogSizeBytes(10 * 1024 * 1024);
        replSettings.setReplSetString("rs0");
        setGlobalReplSettings(replSettings);
        auto coordinatorMock =
            new repl::ReplicationCoordinatorMock(_opCtx->getServiceContext(), replSettings);
        _coordinatorMock = coordinatorMock;
        coordinatorMock->alwaysAllowWrites(true);
        repl::ReplicationCoordinator::set(
            _opCtx->getServiceContext(),
            std::unique_ptr<repl::ReplicationCoordinator>(coordinatorMock));
        repl::StorageInterface::set(_opCtx->getServiceContext(),
                                    stdx::make_unique<repl::StorageInterfaceImpl>());

        auto replicationProcess = new repl::ReplicationProcess(
            repl::StorageInterface::get(_opCtx->getServiceContext()),
            stdx::make_unique<repl::ReplicationConsistencyMarkersMock>(),
            stdx::make_unique<repl::ReplicationRecoveryMock>());
        repl::ReplicationProcess::set(
            cc().getServiceContext(),
            std::unique_ptr<repl::ReplicationProcess>(replicationProcess));

        _consistencyMarkers =
            repl::ReplicationProcess::get(cc().getServiceContext())->getConsistencyMarkers();

        // Since the Client object persists across tests, even though the global
        // ReplicationCoordinator does not, we need to clear the last op associated with the client
        // to avoid the invariant in ReplClientInfo::setLastOp that the optime only goes forward.
        repl::ReplClientInfo::forClient(_opCtx->getClient()).clearLastOp_forTest();

        auto registry = stdx::make_unique<OpObserverRegistry>();
        registry->addObserver(stdx::make_unique<OpObserverShardingImpl>());
        _opCtx->getServiceContext()->setOpObserver(std::move(registry));

        repl::setOplogCollectionName(getGlobalServiceContext());
        repl::createOplog(_opCtx);

        ASSERT_OK(_clock->advanceClusterTime(LogicalTime(Timestamp(1, 0))));

        ASSERT_EQUALS(presentTs, pastLt.addTicks(1).asTimestamp());
        setReplCoordAppliedOpTime(repl::OpTime(presentTs, presentTerm));
    }

    ~StorageTimestampTest() {
        try {
            reset(NamespaceString("local.oplog.rs"));
        } catch (...) {
            FAIL("Exception while cleaning up test");
        }
    }

    /**
     * Walking on ice: resetting the ReplicationCoordinator destroys the underlying
     * `DropPendingCollectionReaper`. Use a truncate/dropAllIndexes to clean out a collection
     * without actually dropping it.
     */
    void reset(NamespaceString nss) const {
        ::mongo::writeConflictRetry(_opCtx, "deleteAll", nss.ns(), [&] {
            _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kUnset);
            AutoGetCollection collRaii(_opCtx, nss, LockMode::MODE_X);

            if (collRaii.getCollection()) {
                WriteUnitOfWork wunit(_opCtx);
                invariant(collRaii.getCollection()->truncate(_opCtx).isOK());
                collRaii.getCollection()->getIndexCatalog()->dropAllIndexes(_opCtx, false);
                wunit.commit();
                return;
            }

            AutoGetOrCreateDb dbRaii(_opCtx, nss.db(), LockMode::MODE_X);
            WriteUnitOfWork wunit(_opCtx);
            invariant(dbRaii.getDb()->createCollection(_opCtx, nss));
            wunit.commit();
        });
    }

    void insertDocument(Collection* coll, const InsertStatement& stmt) {
        // Insert some documents.
        OpDebug* const nullOpDebug = nullptr;
        const bool fromMigrate = false;
        ASSERT_OK(coll->insertDocument(_opCtx, stmt, nullOpDebug, fromMigrate));
    }

    void createIndex(Collection* coll, std::string indexName, const BSONObj& indexKey) {

        // Build an index.
        MultiIndexBlock indexer;
        ON_BLOCK_EXIT([&] { indexer.cleanUpAfterBuild(_opCtx, coll); });

        BSONObj indexInfoObj;
        {
            auto swIndexInfoObj = indexer.init(
                _opCtx,
                coll,
                {BSON("v" << 2 << "name" << indexName << "ns" << coll->ns().ns() << "key"
                          << indexKey)},
                MultiIndexBlock::makeTimestampedIndexOnInitFn(_opCtx, coll));
            ASSERT_OK(swIndexInfoObj.getStatus());
            indexInfoObj = std::move(swIndexInfoObj.getValue()[0]);
        }

        ASSERT_OK(indexer.insertAllDocumentsInCollection(_opCtx, coll));

        {
            WriteUnitOfWork wuow(_opCtx);
            // Timestamping index completion. Primaries write an oplog entry.
            ASSERT_OK(
                indexer.commit(_opCtx,
                               coll,
                               [&](const BSONObj& indexSpec) {
                                   _opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                                       _opCtx, coll->ns(), *(coll->uuid()), indexSpec, false);
                               },
                               MultiIndexBlock::kNoopOnCommitFn));
            // The timestamping repsponsibility is placed on the caller rather than the
            // MultiIndexBlock.
            wuow.commit();
        }
    }

    std::int32_t itCount(Collection* coll) {
        std::uint64_t ret = 0;
        auto cursor = coll->getRecordStore()->getCursor(_opCtx);
        while (cursor->next() != boost::none) {
            ++ret;
        }

        return ret;
    }

    BSONObj findOne(Collection* coll) {
        auto optRecord = coll->getRecordStore()->getCursor(_opCtx)->next();
        if (optRecord == boost::none) {
            // Print a stack trace to help disambiguate which `findOne` failed.
            printStackTrace();
            FAIL("Did not find any documents.");
        }
        return optRecord.get().data.toBson();
    }

    BSONCollectionCatalogEntry::MetaData getMetaDataAtTime(KVCatalog* kvCatalog,
                                                           NamespaceString ns,
                                                           const Timestamp& ts) {
        OneOffRead oor(_opCtx, ts);
        return kvCatalog->getMetaData(_opCtx, ns);
    }

    StatusWith<BSONObj> doAtomicApplyOps(const std::string& dbName,
                                         const std::list<BSONObj>& applyOpsList) {
        OneOffRead oor(_opCtx, Timestamp::min());

        BSONObjBuilder result;
        Status status = applyOps(_opCtx,
                                 dbName,
                                 BSON("applyOps" << applyOpsList),
                                 repl::OplogApplication::Mode::kApplyOpsCmd,
                                 &result);
        if (!status.isOK()) {
            return status;
        }

        return {result.obj()};
    }

    // Creates a dummy command operation to persuade `applyOps` to be non-atomic.
    StatusWith<BSONObj> doNonAtomicApplyOps(const std::string& dbName,
                                            const std::list<BSONObj>& applyOpsList) {
        OneOffRead oor(_opCtx, Timestamp::min());

        BSONObjBuilder result;
        Status status = applyOps(_opCtx,
                                 dbName,
                                 BSON("applyOps" << applyOpsList << "allowAtomic" << false),
                                 repl::OplogApplication::Mode::kApplyOpsCmd,
                                 &result);
        if (!status.isOK()) {
            return status;
        }

        return {result.obj()};
    }

    BSONObj queryCollection(NamespaceString nss, const BSONObj& query) {
        BSONObj ret;
        ASSERT_TRUE(Helpers::findOne(
            _opCtx, AutoGetCollectionForRead(_opCtx, nss).getCollection(), query, ret))
            << "Query: " << query;
        return ret;
    }

    BSONObj queryOplog(const BSONObj& query) {
        OneOffRead oor(_opCtx, Timestamp::min());
        return queryCollection(NamespaceString::kRsOplogNamespace, query);
    }

    void assertMinValidDocumentAtTimestamp(Collection* coll,
                                           const Timestamp& ts,
                                           const repl::MinValidDocument& expectedDoc) {
        OneOffRead oor(_opCtx, ts);

        auto doc =
            repl::MinValidDocument::parse(IDLParserErrorContext("MinValidDocument"), findOne(coll));
        ASSERT_EQ(expectedDoc.getMinValidTimestamp(), doc.getMinValidTimestamp())
            << "minValid timestamps weren't equal at " << ts.toString()
            << ". Expected: " << expectedDoc.toBSON() << ". Found: " << doc.toBSON();
        ASSERT_EQ(expectedDoc.getMinValidTerm(), doc.getMinValidTerm())
            << "minValid terms weren't equal at " << ts.toString()
            << ". Expected: " << expectedDoc.toBSON() << ". Found: " << doc.toBSON();
        ASSERT_EQ(expectedDoc.getAppliedThrough(), doc.getAppliedThrough())
            << "appliedThrough OpTimes weren't equal at " << ts.toString()
            << ". Expected: " << expectedDoc.toBSON() << ". Found: " << doc.toBSON();
        ASSERT_EQ(expectedDoc.getInitialSyncFlag(), doc.getInitialSyncFlag())
            << "Initial sync flags weren't equal at " << ts.toString()
            << ". Expected: " << expectedDoc.toBSON() << ". Found: " << doc.toBSON();
    }

    void assertDocumentAtTimestamp(Collection* coll,
                                   const Timestamp& ts,
                                   const BSONObj& expectedDoc) {
        OneOffRead oor(_opCtx, ts);
        if (expectedDoc.isEmpty()) {
            ASSERT_EQ(0, itCount(coll)) << "Should not find any documents in " << coll->ns()
                                        << " at ts: " << ts;
        } else {
            ASSERT_EQ(1, itCount(coll)) << "Should find one document in " << coll->ns()
                                        << " at ts: " << ts;
            auto doc = findOne(coll);
            ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, expectedDoc))
                << "Doc: " << doc.toString() << " Expected: " << expectedDoc.toString();
        }
    }

    void assertFilteredDocumentAtTimestamp(Collection* coll,
                                           const BSONObj& query,
                                           const Timestamp& ts,
                                           boost::optional<const BSONObj&> expectedDoc) {
        OneOffRead oor(_opCtx, ts);
        BSONObj doc;
        bool found = Helpers::findOne(_opCtx, coll, query, doc);
        if (!expectedDoc) {
            ASSERT_FALSE(found) << "Should not find any documents in " << coll->ns() << " matching "
                                << query << " at ts: " << ts;
        } else {
            ASSERT(found) << "Should find document in " << coll->ns() << " matching " << query
                          << " at ts: " << ts;
            ASSERT_BSONOBJ_EQ(doc, *expectedDoc);
        }
    }

    void assertOplogDocumentExistsAtTimestamp(const BSONObj& query,
                                              const Timestamp& ts,
                                              bool exists) {
        OneOffRead oor(_opCtx, ts);
        BSONObj ret;
        bool found = Helpers::findOne(
            _opCtx,
            AutoGetCollectionForRead(_opCtx, NamespaceString::kRsOplogNamespace).getCollection(),
            query,
            ret);
        ASSERT_EQ(found, exists) << "Found " << ret << " at " << ts.toBSON();
        ASSERT_EQ(!ret.isEmpty(), exists) << "Found " << ret << " at " << ts.toBSON();
    }

    void assertOldestActiveTxnTimestampEquals(const boost::optional<Timestamp>& ts,
                                              const Timestamp& atTs) {
        auto oldest = TransactionParticipant::getOldestActiveTimestamp(atTs);
        ASSERT_EQ(oldest, ts);
    }

    void assertHasStartOpTime() {
        auto txnDoc = _getTxnDoc();
        ASSERT_TRUE(txnDoc.hasField(SessionTxnRecord::kStartOpTimeFieldName));
    }

    void assertNoStartOpTime() {
        auto txnDoc = _getTxnDoc();
        ASSERT_FALSE(txnDoc.hasField(SessionTxnRecord::kStartOpTimeFieldName));
    }

    void setReplCoordAppliedOpTime(const repl::OpTime& opTime, Date_t wallTime = Date_t()) {
        repl::ReplicationCoordinator::get(getGlobalServiceContext())
            ->setMyLastAppliedOpTimeAndWallTime({opTime, wallTime});
        ASSERT_OK(repl::ReplicationCoordinator::get(getGlobalServiceContext())
                      ->updateTerm(_opCtx, opTime.getTerm()));
    }

    /**
     * Asserts that the given collection is in (or not in) the KVCatalog's list of idents at the
     * provided timestamp.
     */
    void assertNamespaceInIdents(NamespaceString nss, Timestamp ts, bool shouldExpect) {
        OneOffRead oor(_opCtx, ts);
        auto kvCatalog = _opCtx->getServiceContext()->getStorageEngine()->getCatalog();

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IS, LockMode::MODE_IS);

        // getCollectionIdent() returns the ident for the given namespace in the KVCatalog.
        // getAllIdents() actually looks in the RecordStore for a list of all idents, and is thus
        // versioned by timestamp. We can expect a namespace to have a consistent ident across
        // timestamps, provided the collection does not get renamed.
        auto expectedIdent = kvCatalog->getCollectionIdent(nss);
        auto idents = kvCatalog->getAllIdents(_opCtx);
        auto found = std::find(idents.begin(), idents.end(), expectedIdent);

        if (shouldExpect) {
            ASSERT(found != idents.end()) << nss.ns() << " was not found at " << ts.toString();
        } else {
            ASSERT(found == idents.end()) << nss.ns() << " was found at " << ts.toString()
                                          << " when it should not have been.";
        }
    }

    /**
     * Use `ts` = Timestamp::min to observe all indexes.
     */
    std::string getNewIndexIdentAtTime(KVCatalog* kvCatalog,
                                       std::vector<std::string>& origIdents,
                                       Timestamp ts) {
        auto ret = getNewIndexIdentsAtTime(kvCatalog, origIdents, ts);
        ASSERT_EQ(static_cast<std::size_t>(1), ret.size()) << " Num idents: " << ret.size();
        return ret[0];
    }

    /**
     * Use `ts` = Timestamp::min to observe all indexes.
     */
    std::vector<std::string> getNewIndexIdentsAtTime(KVCatalog* kvCatalog,
                                                     std::vector<std::string>& origIdents,
                                                     Timestamp ts) {
        OneOffRead oor(_opCtx, ts);

        // Find the collection and index ident by performing a set difference on the original
        // idents and the current idents.
        std::vector<std::string> identsWithColl = kvCatalog->getAllIdents(_opCtx);
        std::sort(origIdents.begin(), origIdents.end());
        std::sort(identsWithColl.begin(), identsWithColl.end());
        std::vector<std::string> idxIdents;
        std::set_difference(identsWithColl.begin(),
                            identsWithColl.end(),
                            origIdents.begin(),
                            origIdents.end(),
                            std::back_inserter(idxIdents));

        for (const auto& ident : idxIdents) {
            ASSERT(ident.find("index-") == 0) << "Ident is not an index: " << ident;
        }
        return idxIdents;
    }

    std::string getDroppedIndexIdent(KVCatalog* kvCatalog, std::vector<std::string>& origIdents) {
        // Find the collection and index ident by performing a set difference on the original
        // idents and the current idents.
        std::vector<std::string> identsWithColl = kvCatalog->getAllIdents(_opCtx);
        std::sort(origIdents.begin(), origIdents.end());
        std::sort(identsWithColl.begin(), identsWithColl.end());
        std::vector<std::string> collAndIdxIdents;
        std::set_difference(origIdents.begin(),
                            origIdents.end(),
                            identsWithColl.begin(),
                            identsWithColl.end(),
                            std::back_inserter(collAndIdxIdents));

        ASSERT(collAndIdxIdents.size() == 1) << "Num idents: " << collAndIdxIdents.size();
        return collAndIdxIdents[0];
    }

    std::vector<std::string> _getIdentDifference(KVCatalog* kvCatalog,
                                                 std::vector<std::string>& origIdents) {
        // Find the ident difference by performing a set difference on the original idents and the
        // current idents.
        std::vector<std::string> identsWithColl = kvCatalog->getAllIdents(_opCtx);
        std::sort(origIdents.begin(), origIdents.end());
        std::sort(identsWithColl.begin(), identsWithColl.end());
        std::vector<std::string> collAndIdxIdents;
        std::set_difference(identsWithColl.begin(),
                            identsWithColl.end(),
                            origIdents.begin(),
                            origIdents.end(),
                            std::back_inserter(collAndIdxIdents));
        return collAndIdxIdents;
    }
    std::tuple<std::string, std::string> getNewCollectionIndexIdent(
        KVCatalog* kvCatalog, std::vector<std::string>& origIdents) {
        // Find the collection and index ident difference.
        auto collAndIdxIdents = _getIdentDifference(kvCatalog, origIdents);

        ASSERT(collAndIdxIdents.size() == 1 || collAndIdxIdents.size() == 2);
        if (collAndIdxIdents.size() == 1) {
            // `system.profile` collections do not have an `_id` index.
            return std::tie(collAndIdxIdents[0], "");
        }
        if (collAndIdxIdents.size() == 2) {
            // The idents are sorted, so the `collection-...` comes before `index-...`
            return std::tie(collAndIdxIdents[0], collAndIdxIdents[1]);
        }

        MONGO_UNREACHABLE;
    }

    /**
     * Note: expectedNewIndexIdents should include the _id index.
     */
    void assertRenamedCollectionIdentsAtTimestamp(KVCatalog* kvCatalog,
                                                  std::vector<std::string>& origIdents,
                                                  size_t expectedNewIndexIdents,
                                                  Timestamp timestamp) {
        OneOffRead oor(_opCtx, timestamp);
        // Find the collection and index ident difference.
        auto collAndIdxIdents = _getIdentDifference(kvCatalog, origIdents);
        size_t newNssIdents, newIdxIdents;
        newNssIdents = newIdxIdents = 0;
        for (const auto& ident : collAndIdxIdents) {
            ASSERT(ident.find("index-") == 0 || ident.find("collection-") == 0)
                << "Ident is not an index or collection: " << ident;
            if (ident.find("collection-") == 0) {
                ASSERT(++newNssIdents == 1) << "Expected new collection idents (1) differ from "
                                               "actual new collection idents ("
                                            << newNssIdents << ")";
            } else {
                newIdxIdents++;
            }
        }
        ASSERT(expectedNewIndexIdents == newIdxIdents)
            << "Expected new index idents (" << expectedNewIndexIdents
            << ") differ from actual new index idents (" << newIdxIdents << ")";
    }

    void assertIdentsExistAtTimestamp(KVCatalog* kvCatalog,
                                      const std::string& collIdent,
                                      const std::string& indexIdent,
                                      Timestamp timestamp) {
        OneOffRead oor(_opCtx, timestamp);

        auto allIdents = kvCatalog->getAllIdents(_opCtx);
        if (collIdent.size() > 0) {
            // Index build test does not pass in a collection ident.
            ASSERT(std::find(allIdents.begin(), allIdents.end(), collIdent) != allIdents.end());
        }

        if (indexIdent.size() > 0) {
            // `system.profile` does not have an `_id` index.
            ASSERT(std::find(allIdents.begin(), allIdents.end(), indexIdent) != allIdents.end());
        }
    }

    void assertIdentsMissingAtTimestamp(KVCatalog* kvCatalog,
                                        const std::string& collIdent,
                                        const std::string& indexIdent,
                                        Timestamp timestamp) {
        OneOffRead oor(_opCtx, timestamp);
        auto allIdents = kvCatalog->getAllIdents(_opCtx);
        if (collIdent.size() > 0) {
            // Index build test does not pass in a collection ident.
            ASSERT(std::find(allIdents.begin(), allIdents.end(), collIdent) == allIdents.end());
        }

        ASSERT(std::find(allIdents.begin(), allIdents.end(), indexIdent) == allIdents.end())
            << "Ident: " << indexIdent << " Timestamp: " << timestamp;
    }

    std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths) {
        std::stringstream ss;

        ss << "[ ";
        for (const auto multikeyComponents : multikeyPaths) {
            ss << "[ ";
            for (const auto multikeyComponent : multikeyComponents) {
                ss << multikeyComponent << " ";
            }
            ss << "] ";
        }
        ss << "]";

        return ss.str();
    }

    void assertMultikeyPaths(OperationContext* opCtx,
                             Collection* collection,
                             StringData indexName,
                             Timestamp ts,
                             bool shouldBeMultikey,
                             const MultikeyPaths& expectedMultikeyPaths) {
        auto catalog = collection->getCatalogEntry();

        OneOffRead oor(_opCtx, ts);

        MultikeyPaths actualMultikeyPaths;
        if (!shouldBeMultikey) {
            ASSERT_FALSE(catalog->isIndexMultikey(opCtx, indexName, &actualMultikeyPaths))
                << "index " << indexName << " should not be multikey at timestamp " << ts;
        } else {
            ASSERT(catalog->isIndexMultikey(opCtx, indexName, &actualMultikeyPaths))
                << "index " << indexName << " should be multikey at timestamp " << ts;
        }

        const bool match = (expectedMultikeyPaths == actualMultikeyPaths);
        if (!match) {
            FAIL(str::stream() << "Expected: " << dumpMultikeyPaths(expectedMultikeyPaths)
                               << ", Actual: "
                               << dumpMultikeyPaths(actualMultikeyPaths));
        }
        ASSERT_TRUE(match);
    }

private:
    BSONObj _getTxnDoc() {
        auto txnParticipant = TransactionParticipant::get(_opCtx);
        auto txnsFilter = BSON("_id" << _opCtx->getLogicalSessionId()->toBSON() << "txnNum"
                                     << txnParticipant.getActiveTxnNumber());
        return queryCollection(NamespaceString::kSessionTransactionsTableNamespace, txnsFilter);
    }
};

class SecondaryInsertTimes : public StorageTimestampTest {
public:
    void run() {
        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        // Create a new collection.
        NamespaceString nss("unittests.timestampedUpdates");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        const std::uint32_t docsToInsert = 10;
        const LogicalTime firstInsertTime = _clock->reserveTicks(docsToInsert);
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            BSONObjBuilder result;
            ASSERT_OK(applyOps(
                _opCtx,
                nss.db().toString(),
                BSON("applyOps" << BSON_ARRAY(
                         BSON("ts" << firstInsertTime.addTicks(idx).asTimestamp() << "t" << 1LL
                                   << "v"
                                   << 2
                                   << "op"
                                   << "i"
                                   << "ns"
                                   << nss.ns()
                                   << "ui"
                                   << autoColl.getCollection()->uuid().get()
                                   << "o"
                                   << BSON("_id" << idx))
                         << BSON("ts" << firstInsertTime.addTicks(idx).asTimestamp() << "t" << 1LL
                                      << "op"
                                      << "c"
                                      << "ns"
                                      << "test.$cmd"
                                      << "o"
                                      << BSON("applyOps" << BSONArrayBuilder().obj())))),
                repl::OplogApplication::Mode::kApplyOpsCmd,
                &result));
        }

        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            OneOffRead oor(_opCtx, firstInsertTime.addTicks(idx).asTimestamp());

            BSONObj result;
            ASSERT(Helpers::getLast(_opCtx, nss.ns().c_str(), result)) << " idx is " << idx;
            ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(result, BSON("_id" << idx)))
                << "Doc: " << result.toString() << " Expected: " << BSON("_id" << idx);
        }
    }
};

class SecondaryArrayInsertTimes : public StorageTimestampTest {
public:
    void run() {
        // In order for oplog application to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        // Create a new collection.
        NamespaceString nss("unittests.timestampedUpdates");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        const std::uint32_t docsToInsert = 10;
        const LogicalTime firstInsertTime = _clock->reserveTicks(docsToInsert);

        BSONObjBuilder oplogEntryBuilder;

        // Populate the "ts" field with an array of all the grouped inserts' timestamps.
        BSONArrayBuilder tsArrayBuilder(oplogEntryBuilder.subarrayStart("ts"));
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            tsArrayBuilder.append(firstInsertTime.addTicks(idx).asTimestamp());
        }
        tsArrayBuilder.done();

        // Populate the "t" (term) field with an array of all the grouped inserts' terms.
        BSONArrayBuilder tArrayBuilder(oplogEntryBuilder.subarrayStart("t"));
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            tArrayBuilder.append(1LL);
        }
        tArrayBuilder.done();

        // Populate the "o" field with an array of all the grouped inserts.
        BSONArrayBuilder oArrayBuilder(oplogEntryBuilder.subarrayStart("o"));
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            oArrayBuilder.append(BSON("_id" << idx));
        }
        oArrayBuilder.done();

        oplogEntryBuilder << "v" << 2 << "op"
                          << "i"
                          << "ns" << nss.ns() << "ui" << autoColl.getCollection()->uuid().get();

        auto oplogEntry = oplogEntryBuilder.done();
        ASSERT_OK(repl::SyncTail::syncApply(
            _opCtx, oplogEntry, repl::OplogApplication::Mode::kSecondary, boost::none));

        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            OneOffRead oor(_opCtx, firstInsertTime.addTicks(idx).asTimestamp());

            BSONObj result;
            ASSERT(Helpers::getLast(_opCtx, nss.ns().c_str(), result)) << " idx is " << idx;
            ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(result, BSON("_id" << idx)))
                << "Doc: " << result.toString() << " Expected: " << BSON("_id" << idx);
        }
    }
};

class SecondaryDeleteTimes : public StorageTimestampTest {
public:
    void run() {
        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        // Create a new collection.
        NamespaceString nss("unittests.timestampedDeletes");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        // Insert some documents.
        const std::int32_t docsToInsert = 10;
        const LogicalTime firstInsertTime = _clock->reserveTicks(docsToInsert);
        const LogicalTime lastInsertTime = firstInsertTime.addTicks(docsToInsert - 1);
        WriteUnitOfWork wunit(_opCtx);
        for (std::int32_t num = 0; num < docsToInsert; ++num) {
            insertDocument(autoColl.getCollection(),
                           InsertStatement(BSON("_id" << num << "a" << num),
                                           firstInsertTime.addTicks(num).asTimestamp(),
                                           0LL));
        }
        wunit.commit();
        ASSERT_EQ(docsToInsert, itCount(autoColl.getCollection()));

        // Delete all documents one at a time.
        const LogicalTime startDeleteTime = _clock->reserveTicks(docsToInsert);
        for (std::int32_t num = 0; num < docsToInsert; ++num) {
            ASSERT_OK(
                doNonAtomicApplyOps(
                    nss.db().toString(),
                    {BSON("ts" << startDeleteTime.addTicks(num).asTimestamp() << "t" << 0LL << "v"
                               << 2
                               << "op"
                               << "d"
                               << "ns"
                               << nss.ns()
                               << "ui"
                               << autoColl.getCollection()->uuid().get()
                               << "o"
                               << BSON("_id" << num))})
                    .getStatus());
        }

        for (std::int32_t num = 0; num <= docsToInsert; ++num) {
            // The first loop queries at `lastInsertTime` and should count all documents. Querying
            // at each successive tick counts one less document.
            OneOffRead oor(_opCtx, lastInsertTime.addTicks(num).asTimestamp());
            ASSERT_EQ(docsToInsert - num, itCount(autoColl.getCollection()));
        }
    }
};

class SecondaryUpdateTimes : public StorageTimestampTest {
public:
    void run() {
        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        // Create a new collection.
        NamespaceString nss("unittests.timestampedUpdates");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        // Insert one document that will go through a series of updates.
        const LogicalTime insertTime = _clock->reserveTicks(1);
        WriteUnitOfWork wunit(_opCtx);
        insertDocument(autoColl.getCollection(),
                       InsertStatement(BSON("_id" << 0), insertTime.asTimestamp(), 0LL));
        wunit.commit();
        ASSERT_EQ(1, itCount(autoColl.getCollection()));

        // Each pair in the vector represents the update to perform at the next tick of the
        // clock. `pair.first` is the update to perform and `pair.second` is the full value of the
        // document after the transformation.
        const std::vector<std::pair<BSONObj, BSONObj>> updates = {
            {BSON("$set" << BSON("val" << 1)), BSON("_id" << 0 << "val" << 1)},
            {BSON("$unset" << BSON("val" << 1)), BSON("_id" << 0)},
            {BSON("$addToSet" << BSON("theSet" << 1)),
             BSON("_id" << 0 << "theSet" << BSON_ARRAY(1))},
            {BSON("$addToSet" << BSON("theSet" << 2)),
             BSON("_id" << 0 << "theSet" << BSON_ARRAY(1 << 2))},
            {BSON("$pull" << BSON("theSet" << 1)), BSON("_id" << 0 << "theSet" << BSON_ARRAY(2))},
            {BSON("$pull" << BSON("theSet" << 2)), BSON("_id" << 0 << "theSet" << BSONArray())},
            {BSON("$set" << BSON("theMap.val" << 1)),
             BSON("_id" << 0 << "theSet" << BSONArray() << "theMap" << BSON("val" << 1))},
            {BSON("$rename" << BSON("theSet"
                                    << "theOtherSet")),
             BSON("_id" << 0 << "theMap" << BSON("val" << 1) << "theOtherSet" << BSONArray())}};

        const LogicalTime firstUpdateTime = _clock->reserveTicks(updates.size());
        for (std::size_t idx = 0; idx < updates.size(); ++idx) {
            ASSERT_OK(
                doNonAtomicApplyOps(
                    nss.db().toString(),
                    {BSON("ts" << firstUpdateTime.addTicks(idx).asTimestamp() << "t" << 0LL << "v"
                               << 2
                               << "op"
                               << "u"
                               << "ns"
                               << nss.ns()
                               << "ui"
                               << autoColl.getCollection()->uuid().get()
                               << "o2"
                               << BSON("_id" << 0)
                               << "o"
                               << updates[idx].first)})
                    .getStatus());
        }

        for (std::size_t idx = 0; idx < updates.size(); ++idx) {
            // Querying at each successive ticks after `insertTime` sees the document transform in
            // the series.
            OneOffRead oor(_opCtx, insertTime.addTicks(idx + 1).asTimestamp());

            auto doc = findOne(autoColl.getCollection());
            ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, updates[idx].second))
                << "Doc: " << doc.toString() << " Expected: " << updates[idx].second.toString();
        }
    }
};

class SecondaryInsertToUpsert : public StorageTimestampTest {
public:
    void run() {
        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        // Create a new collection.
        NamespaceString nss("unittests.insertToUpsert");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        const LogicalTime insertTime = _clock->reserveTicks(2);

        // This applyOps runs into an insert of `{_id: 0, field: 0}` followed by a second insert
        // on the same collection with `{_id: 0}`. It's expected for this second insert to be
        // turned into an upsert. The goal document does not contain `field: 0`.
        BSONObjBuilder resultBuilder;
        auto result = unittest::assertGet(doNonAtomicApplyOps(
            nss.db().toString(),
            {BSON("ts" << insertTime.asTimestamp() << "t" << 1LL << "op"
                       << "i"
                       << "ns"
                       << nss.ns()
                       << "ui"
                       << autoColl.getCollection()->uuid().get()
                       << "o"
                       << BSON("_id" << 0 << "field" << 0)),
             BSON("ts" << insertTime.addTicks(1).asTimestamp() << "t" << 1LL << "op"
                       << "i"
                       << "ns"
                       << nss.ns()
                       << "ui"
                       << autoColl.getCollection()->uuid().get()
                       << "o"
                       << BSON("_id" << 0))}));

        ASSERT_EQ(2, result.getIntField("applied"));
        ASSERT(result["results"].Array()[0].Bool());
        ASSERT(result["results"].Array()[1].Bool());

        // Reading at `insertTime` should show the original document, `{_id: 0, field: 0}`.
        auto recoveryUnit = _opCtx->recoveryUnit();
        recoveryUnit->abandonSnapshot();
        recoveryUnit->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                             insertTime.asTimestamp());
        auto doc = findOne(autoColl.getCollection());
        ASSERT_EQ(0,
                  SimpleBSONObjComparator::kInstance.compare(doc, BSON("_id" << 0 << "field" << 0)))
            << "Doc: " << doc.toString() << " Expected: {_id: 0, field: 0}";

        // Reading at `insertTime + 1` should show the second insert that got converted to an
        // upsert, `{_id: 0}`.
        recoveryUnit->abandonSnapshot();
        recoveryUnit->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                             insertTime.addTicks(1).asTimestamp());
        doc = findOne(autoColl.getCollection());
        ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, BSON("_id" << 0)))
            << "Doc: " << doc.toString() << " Expected: {_id: 0}";
    }
};

class SecondaryAtomicApplyOps : public StorageTimestampTest {
public:
    void run() {
        // Create a new collection.
        NamespaceString nss("unittests.insertToUpsert");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        // Reserve a timestamp before the inserts should happen.
        const LogicalTime preInsertTimestamp = _clock->reserveTicks(1);
        auto swResult = doAtomicApplyOps(nss.db().toString(),
                                         {BSON("op"
                                               << "i"
                                               << "ns"
                                               << nss.ns()
                                               << "ui"
                                               << autoColl.getCollection()->uuid().get()
                                               << "o"
                                               << BSON("_id" << 0)),
                                          BSON("op"
                                               << "i"
                                               << "ns"
                                               << nss.ns()
                                               << "ui"
                                               << autoColl.getCollection()->uuid().get()
                                               << "o"
                                               << BSON("_id" << 1))});
        ASSERT_OK(swResult);

        ASSERT_EQ(2, swResult.getValue().getIntField("applied"));
        ASSERT(swResult.getValue()["results"].Array()[0].Bool());
        ASSERT(swResult.getValue()["results"].Array()[1].Bool());

        // Reading at `preInsertTimestamp` should not find anything.
        auto recoveryUnit = _opCtx->recoveryUnit();
        recoveryUnit->abandonSnapshot();
        recoveryUnit->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                             preInsertTimestamp.asTimestamp());
        ASSERT_EQ(0, itCount(autoColl.getCollection()))
            << "Should not observe a write at `preInsertTimestamp`. TS: "
            << preInsertTimestamp.asTimestamp();

        // Reading at `preInsertTimestamp + 1` should observe both inserts.
        recoveryUnit = _opCtx->recoveryUnit();
        recoveryUnit->abandonSnapshot();
        recoveryUnit->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                             preInsertTimestamp.addTicks(1).asTimestamp());
        ASSERT_EQ(2, itCount(autoColl.getCollection()))
            << "Should observe both writes at `preInsertTimestamp + 1`. TS: "
            << preInsertTimestamp.addTicks(1).asTimestamp();
    }
};


// This should have the same result as `SecondaryInsertToUpsert` except it gets there a different
// way. Doing an atomic `applyOps` should result in a WriteConflictException because the same
// transaction is trying to write modify the same document twice. The `applyOps` command should
// catch that failure and retry in non-atomic mode, preserving the timestamps supplied by the
// user.
class SecondaryAtomicApplyOpsWCEToNonAtomic : public StorageTimestampTest {
public:
    void run() {
        // Create a new collectiont.
        NamespaceString nss("unitteTsts.insertToUpsert");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        const LogicalTime preInsertTimestamp = _clock->reserveTicks(1);
        auto swResult = doAtomicApplyOps(nss.db().toString(),
                                         {BSON("op"
                                               << "i"
                                               << "ns"
                                               << nss.ns()
                                               << "ui"
                                               << autoColl.getCollection()->uuid().get()
                                               << "o"
                                               << BSON("_id" << 0 << "field" << 0)),
                                          BSON("op"
                                               << "i"
                                               << "ns"
                                               << nss.ns()
                                               << "ui"
                                               << autoColl.getCollection()->uuid().get()
                                               << "o"
                                               << BSON("_id" << 0))});
        ASSERT_OK(swResult);

        ASSERT_EQ(2, swResult.getValue().getIntField("applied"));
        ASSERT(swResult.getValue()["results"].Array()[0].Bool());
        ASSERT(swResult.getValue()["results"].Array()[1].Bool());

        // Reading at `insertTime` should not see any documents.
        auto recoveryUnit = _opCtx->recoveryUnit();
        recoveryUnit->abandonSnapshot();
        recoveryUnit->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                             preInsertTimestamp.asTimestamp());
        ASSERT_EQ(0, itCount(autoColl.getCollection()))
            << "Should not find any documents at `preInsertTimestamp`. TS: "
            << preInsertTimestamp.asTimestamp();

        // Reading at `preInsertTimestamp + 1` should show the final state of the document.
        recoveryUnit->abandonSnapshot();
        recoveryUnit->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                             preInsertTimestamp.addTicks(1).asTimestamp());
        auto doc = findOne(autoColl.getCollection());
        ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, BSON("_id" << 0)))
            << "Doc: " << doc.toString() << " Expected: {_id: 0}";
    }
};

class SecondaryCreateCollection : public StorageTimestampTest {
public:
    void run() {
        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        NamespaceString nss("unittests.secondaryCreateCollection");
        ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss));

        { ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx, nss).getCollection()); }

        BSONObjBuilder resultBuilder;
        auto swResult = doNonAtomicApplyOps(nss.db().toString(),
                                            {
                                                BSON("ts" << presentTs << "t" << 1LL << "op"
                                                          << "c"
                                                          << "ui"
                                                          << UUID::gen()
                                                          << "ns"
                                                          << nss.getCommandNS().ns()
                                                          << "o"
                                                          << BSON("create" << nss.coll())),
                                            });
        ASSERT_OK(swResult);

        { ASSERT(AutoGetCollectionForReadCommand(_opCtx, nss).getCollection()); }

        assertNamespaceInIdents(nss, pastTs, false);
        assertNamespaceInIdents(nss, presentTs, true);
        assertNamespaceInIdents(nss, futureTs, true);
        assertNamespaceInIdents(nss, nullTs, true);
    }
};

class SecondaryCreateTwoCollections : public StorageTimestampTest {
public:
    void run() {
        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        std::string dbName = "unittest";
        NamespaceString nss1(dbName, "secondaryCreateTwoCollections1");
        NamespaceString nss2(dbName, "secondaryCreateTwoCollections2");
        ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss1));
        ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss2));

        { ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx, nss1).getCollection()); }
        { ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx, nss2).getCollection()); }

        const LogicalTime dummyLt = futureLt.addTicks(1);
        const Timestamp dummyTs = dummyLt.asTimestamp();

        BSONObjBuilder resultBuilder;
        auto swResult = doNonAtomicApplyOps(dbName,
                                            {
                                                BSON("ts" << presentTs << "t" << 1LL << "op"
                                                          << "c"
                                                          << "ui"
                                                          << UUID::gen()
                                                          << "ns"
                                                          << nss1.getCommandNS().ns()
                                                          << "o"
                                                          << BSON("create" << nss1.coll())),
                                                BSON("ts" << futureTs << "t" << 1LL << "op"
                                                          << "c"
                                                          << "ui"
                                                          << UUID::gen()
                                                          << "ns"
                                                          << nss2.getCommandNS().ns()
                                                          << "o"
                                                          << BSON("create" << nss2.coll())),
                                            });
        ASSERT_OK(swResult);

        { ASSERT(AutoGetCollectionForReadCommand(_opCtx, nss1).getCollection()); }
        { ASSERT(AutoGetCollectionForReadCommand(_opCtx, nss2).getCollection()); }

        assertNamespaceInIdents(nss1, pastTs, false);
        assertNamespaceInIdents(nss1, presentTs, true);
        assertNamespaceInIdents(nss1, futureTs, true);
        assertNamespaceInIdents(nss1, dummyTs, true);
        assertNamespaceInIdents(nss1, nullTs, true);

        assertNamespaceInIdents(nss2, pastTs, false);
        assertNamespaceInIdents(nss2, presentTs, false);
        assertNamespaceInIdents(nss2, futureTs, true);
        assertNamespaceInIdents(nss2, dummyTs, true);
        assertNamespaceInIdents(nss2, nullTs, true);
    }
};

class SecondaryCreateCollectionBetweenInserts : public StorageTimestampTest {
public:
    void run() {
        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        std::string dbName = "unittest";
        NamespaceString nss1(dbName, "secondaryCreateCollectionBetweenInserts1");
        NamespaceString nss2(dbName, "secondaryCreateCollectionBetweenInserts2");
        BSONObj doc1 = BSON("_id" << 1 << "field" << 1);
        BSONObj doc2 = BSON("_id" << 2 << "field" << 2);

        const UUID uuid2 = UUID::gen();

        const LogicalTime insert2Lt = futureLt.addTicks(1);
        const Timestamp insert2Ts = insert2Lt.asTimestamp();

        const LogicalTime dummyLt = insert2Lt.addTicks(1);
        const Timestamp dummyTs = dummyLt.asTimestamp();

        {
            reset(nss1);
            AutoGetCollection autoColl(_opCtx, nss1, LockMode::MODE_X, LockMode::MODE_IX);

            ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss2));
            { ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx, nss2).getCollection()); }

            BSONObjBuilder resultBuilder;
            auto swResult =
                doNonAtomicApplyOps(dbName,
                                    {
                                        BSON("ts" << presentTs << "t" << 1LL << "op"
                                                  << "i"
                                                  << "ns"
                                                  << nss1.ns()
                                                  << "ui"
                                                  << autoColl.getCollection()->uuid().get()
                                                  << "o"
                                                  << doc1),
                                        BSON("ts" << futureTs << "t" << 1LL << "op"
                                                  << "c"
                                                  << "ui"
                                                  << uuid2
                                                  << "ns"
                                                  << nss2.getCommandNS().ns()
                                                  << "o"
                                                  << BSON("create" << nss2.coll())),
                                        BSON("ts" << insert2Ts << "t" << 1LL << "op"
                                                  << "i"
                                                  << "ns"
                                                  << nss2.ns()
                                                  << "ui"
                                                  << uuid2
                                                  << "o"
                                                  << doc2),
                                    });
            ASSERT_OK(swResult);
        }

        {
            AutoGetCollectionForReadCommand autoColl1(_opCtx, nss1);
            auto coll1 = autoColl1.getCollection();
            ASSERT(coll1);
            AutoGetCollectionForReadCommand autoColl2(_opCtx, nss2);
            auto coll2 = autoColl2.getCollection();
            ASSERT(coll2);

            assertDocumentAtTimestamp(coll1, pastTs, BSONObj());
            assertDocumentAtTimestamp(coll1, presentTs, doc1);
            assertDocumentAtTimestamp(coll1, futureTs, doc1);
            assertDocumentAtTimestamp(coll1, insert2Ts, doc1);
            assertDocumentAtTimestamp(coll1, dummyTs, doc1);
            assertDocumentAtTimestamp(coll1, nullTs, doc1);

            assertNamespaceInIdents(nss2, pastTs, false);
            assertNamespaceInIdents(nss2, presentTs, false);
            assertNamespaceInIdents(nss2, futureTs, true);
            assertNamespaceInIdents(nss2, insert2Ts, true);
            assertNamespaceInIdents(nss2, dummyTs, true);
            assertNamespaceInIdents(nss2, nullTs, true);

            assertDocumentAtTimestamp(coll2, pastTs, BSONObj());
            assertDocumentAtTimestamp(coll2, presentTs, BSONObj());
            assertDocumentAtTimestamp(coll2, futureTs, BSONObj());
            assertDocumentAtTimestamp(coll2, insert2Ts, doc2);
            assertDocumentAtTimestamp(coll2, dummyTs, doc2);
            assertDocumentAtTimestamp(coll2, nullTs, doc2);
        }
    }
};

class PrimaryCreateCollectionInApplyOps : public StorageTimestampTest {
public:
    void run() {
        NamespaceString nss("unittests.primaryCreateCollectionInApplyOps");
        ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss));

        { ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx, nss).getCollection()); }

        BSONObjBuilder resultBuilder;
        auto swResult = doNonAtomicApplyOps(nss.db().toString(),
                                            {
                                                BSON("ts" << presentTs << "t" << 1LL << "op"
                                                          << "c"
                                                          << "ui"
                                                          << UUID::gen()
                                                          << "ns"
                                                          << nss.getCommandNS().ns()
                                                          << "o"
                                                          << BSON("create" << nss.coll())),
                                            });
        ASSERT_OK(swResult);

        { ASSERT(AutoGetCollectionForReadCommand(_opCtx, nss).getCollection()); }

        BSONObj result;
        ASSERT(Helpers::getLast(
            _opCtx, NamespaceString::kRsOplogNamespace.toString().c_str(), result));
        repl::OplogEntry op(result);
        ASSERT(op.getOpType() == repl::OpTypeEnum::kCommand) << op.toBSON();
        // The next logOp() call will get 'futureTs', which will be the timestamp at which we do
        // the write. Thus we expect the write to appear at 'futureTs' and not before.
        ASSERT_EQ(op.getTimestamp(), futureTs) << op.toBSON();
        ASSERT_EQ(op.getNss().ns(), nss.getCommandNS().ns()) << op.toBSON();
        ASSERT_BSONOBJ_EQ(op.getObject(), BSON("create" << nss.coll()));

        assertNamespaceInIdents(nss, pastTs, false);
        assertNamespaceInIdents(nss, presentTs, false);
        assertNamespaceInIdents(nss, futureTs, true);
        assertNamespaceInIdents(nss, nullTs, true);
    }
};

class SecondarySetIndexMultikeyOnInsert : public StorageTimestampTest {

public:
    void run() {
        // Pretend to be a secondary.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        NamespaceString nss("unittests.SecondarySetIndexMultikeyOnInsert");
        reset(nss);
        UUID uuid = UUID::gen();
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
            uuid = autoColl.getCollection()->uuid().get();
        }
        auto indexName = "a_1";
        auto indexSpec =
            BSON("name" << indexName << "ns" << nss.ns() << "key" << BSON("a" << 1) << "v"
                        << static_cast<int>(kIndexVersion));
        ASSERT_OK(dbtests::createIndexFromSpec(_opCtx, nss.ns(), indexSpec));

        _coordinatorMock->alwaysAllowWrites(false);

        const LogicalTime pastTime = _clock->reserveTicks(1);
        const LogicalTime insertTime0 = _clock->reserveTicks(1);
        const LogicalTime insertTime1 = _clock->reserveTicks(1);
        const LogicalTime insertTime2 = _clock->reserveTicks(1);

        BSONObj doc0 = BSON("_id" << 0 << "a" << 3);
        BSONObj doc1 = BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2));
        BSONObj doc2 = BSON("_id" << 2 << "a" << BSON_ARRAY(1 << 2));
        auto op0 = repl::OplogEntry(BSON("ts" << insertTime0.asTimestamp() << "t" << 1LL << "v" << 2
                                              << "op"
                                              << "i"
                                              << "ns"
                                              << nss.ns()
                                              << "ui"
                                              << uuid
                                              << "o"
                                              << doc0));
        auto op1 = repl::OplogEntry(BSON("ts" << insertTime1.asTimestamp() << "t" << 1LL << "v" << 2
                                              << "op"
                                              << "i"
                                              << "ns"
                                              << nss.ns()
                                              << "ui"
                                              << uuid
                                              << "o"
                                              << doc1));
        auto op2 = repl::OplogEntry(BSON("ts" << insertTime2.asTimestamp() << "t" << 1LL << "v" << 2
                                              << "op"
                                              << "i"
                                              << "ns"
                                              << nss.ns()
                                              << "ui"
                                              << uuid
                                              << "o"
                                              << doc2));
        std::vector<repl::OplogEntry> ops = {op0, op1, op2};

        DoNothingOplogApplierObserver observer;
        auto storageInterface = repl::StorageInterface::get(_opCtx);
        auto writerPool = repl::OplogApplier::makeWriterPool();
        repl::OplogApplierImpl oplogApplier(
            nullptr,  // task executor. not required for multiApply().
            nullptr,  // oplog buffer. not required for multiApply().
            &observer,
            nullptr,  // replication coordinator. not required for multiApply().
            _consistencyMarkers,
            storageInterface,
            {},
            writerPool.get());
        ASSERT_EQUALS(op2.getOpTime(),
                      unittest::assertGet(oplogApplier.multiApply(_opCtx, ops, boost::none)));

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
        assertMultikeyPaths(
            _opCtx, autoColl.getCollection(), indexName, pastTime.asTimestamp(), false, {{}});
        assertMultikeyPaths(
            _opCtx, autoColl.getCollection(), indexName, insertTime0.asTimestamp(), true, {{0}});
        assertMultikeyPaths(
            _opCtx, autoColl.getCollection(), indexName, insertTime1.asTimestamp(), true, {{0}});
        assertMultikeyPaths(
            _opCtx, autoColl.getCollection(), indexName, insertTime2.asTimestamp(), true, {{0}});
    }
};

class InitialSyncSetIndexMultikeyOnInsert : public StorageTimestampTest {

public:
    void run() {
        // Pretend to be a secondary.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        NamespaceString nss("unittests.InitialSyncSetIndexMultikeyOnInsert");
        reset(nss);
        UUID uuid = UUID::gen();
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
            uuid = autoColl.getCollection()->uuid().get();
        }
        auto indexName = "a_1";
        auto indexSpec =
            BSON("name" << indexName << "ns" << nss.ns() << "key" << BSON("a" << 1) << "v"
                        << static_cast<int>(kIndexVersion));
        ASSERT_OK(dbtests::createIndexFromSpec(_opCtx, nss.ns(), indexSpec));

        _coordinatorMock->alwaysAllowWrites(false);
        ASSERT_OK(_coordinatorMock->setFollowerMode({repl::MemberState::MS::RS_STARTUP2}));

        const LogicalTime pastTime = _clock->reserveTicks(1);
        const LogicalTime insertTime0 = _clock->reserveTicks(1);
        const LogicalTime indexBuildTime = _clock->reserveTicks(1);
        const LogicalTime insertTime1 = _clock->reserveTicks(1);
        const LogicalTime insertTime2 = _clock->reserveTicks(1);

        BSONObj doc0 = BSON("_id" << 0 << "a" << 3);
        BSONObj doc1 = BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2));
        BSONObj doc2 = BSON("_id" << 2 << "a" << BSON_ARRAY(1 << 2));
        auto op0 = repl::OplogEntry(BSON("ts" << insertTime0.asTimestamp() << "t" << 1LL << "v" << 2
                                              << "op"
                                              << "i"
                                              << "ns"
                                              << nss.ns()
                                              << "ui"
                                              << uuid
                                              << "o"
                                              << doc0));
        auto op1 = repl::OplogEntry(BSON("ts" << insertTime1.asTimestamp() << "t" << 1LL << "v" << 2
                                              << "op"
                                              << "i"
                                              << "ns"
                                              << nss.ns()
                                              << "ui"
                                              << uuid
                                              << "o"
                                              << doc1));
        auto op2 = repl::OplogEntry(BSON("ts" << insertTime2.asTimestamp() << "t" << 1LL << "v" << 2
                                              << "op"
                                              << "i"
                                              << "ns"
                                              << nss.ns()
                                              << "ui"
                                              << uuid
                                              << "o"
                                              << doc2));
        auto indexSpec2 = BSON("createIndexes" << nss.coll() << "ns" << nss.ns() << "v"
                                               << static_cast<int>(kIndexVersion)
                                               << "key"
                                               << BSON("b" << 1)
                                               << "name"
                                               << "b_1");
        auto createIndexOp = repl::OplogEntry(
            BSON("ts" << indexBuildTime.asTimestamp() << "t" << 1LL << "v" << 2 << "op"
                      << "c"
                      << "ns"
                      << nss.getCommandNS().ns()
                      << "ui"
                      << uuid
                      << "o"
                      << indexSpec2));

        // We add in an index creation op to test that we restart tracking multikey path info
        // after bulk index builds.
        std::vector<repl::OplogEntry> ops = {op0, createIndexOp, op1, op2};

        DoNothingOplogApplierObserver observer;
        auto storageInterface = repl::StorageInterface::get(_opCtx);
        auto writerPool = repl::OplogApplier::makeWriterPool();
        repl::OplogApplier::Options options;
        options.allowNamespaceNotFoundErrorsOnCrudOps = true;
        options.missingDocumentSourceForInitialSync = HostAndPort("localhost", 123);

        repl::OplogApplierImpl oplogApplier(
            nullptr,  // task executor. not required for multiApply().
            nullptr,  // oplog buffer. not required for multiApply().
            &observer,
            nullptr,  // replication coordinator. not required for multiApply().
            _consistencyMarkers,
            storageInterface,
            options,
            writerPool.get());
        auto lastTime = unittest::assertGet(oplogApplier.multiApply(_opCtx, ops, boost::none));
        ASSERT_EQ(lastTime.getTimestamp(), insertTime2.asTimestamp());

        // Wait for the index build to finish before making any assertions.
        IndexBuildsCoordinator::get(_opCtx)->awaitNoIndexBuildInProgressForCollection(uuid);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        // It is not valid to read the multikey state earlier than the 'minimumVisibleTimestamp',
        // so at least assert that it has been updated due to the index creation.
        ASSERT_GT(autoColl.getCollection()->getMinimumVisibleSnapshot().get(),
                  pastTime.asTimestamp());

        // Reading the multikey state before 'insertTime0' is not valid or reliable to test. If the
        // background index build intercepts and drains writes during inital sync, the index write
        // and the write to the multikey path state will not be timestamped. This write is not
        // timestamped because the lastApplied timestamp, which would normally be used on a primary
        // or secondary, is not always available during initial sync.
        // Additionally, it is not valid to read at a timestamp before inital sync completes, so
        // these assertions below only make sense in the context of this unit test, but would
        // otherwise not be exercised in any normal scenario.
        assertMultikeyPaths(
            _opCtx, autoColl.getCollection(), indexName, insertTime0.asTimestamp(), true, {{0}});
        assertMultikeyPaths(
            _opCtx, autoColl.getCollection(), indexName, insertTime1.asTimestamp(), true, {{0}});
        assertMultikeyPaths(
            _opCtx, autoColl.getCollection(), indexName, insertTime2.asTimestamp(), true, {{0}});
    }
};

class PrimarySetIndexMultikeyOnInsert : public StorageTimestampTest {

public:
    void run() {
        NamespaceString nss("unittests.PrimarySetIndexMultikeyOnInsert");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
        auto indexName = "a_1";
        auto indexSpec =
            BSON("name" << indexName << "ns" << nss.ns() << "key" << BSON("a" << 1) << "v"
                        << static_cast<int>(kIndexVersion));
        ASSERT_OK(dbtests::createIndexFromSpec(_opCtx, nss.ns(), indexSpec));

        const LogicalTime pastTime = _clock->reserveTicks(1);
        const LogicalTime insertTime = pastTime.addTicks(1);

        BSONObj doc = BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2));
        WriteUnitOfWork wunit(_opCtx);
        insertDocument(autoColl.getCollection(), InsertStatement(doc));
        wunit.commit();

        assertMultikeyPaths(
            _opCtx, autoColl.getCollection(), indexName, pastTime.asTimestamp(), false, {{}});
        assertMultikeyPaths(
            _opCtx, autoColl.getCollection(), indexName, insertTime.asTimestamp(), true, {{0}});
    }
};

class PrimarySetIndexMultikeyOnInsertUnreplicated : public StorageTimestampTest {

public:
    void run() {
        // Use an unreplicated collection.
        NamespaceString nss("unittests.system.profile");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
        auto indexName = "a_1";
        auto indexSpec =
            BSON("name" << indexName << "ns" << nss.ns() << "key" << BSON("a" << 1) << "v"
                        << static_cast<int>(kIndexVersion));
        ASSERT_OK(dbtests::createIndexFromSpec(_opCtx, nss.ns(), indexSpec));

        const LogicalTime pastTime = _clock->reserveTicks(1);
        const LogicalTime insertTime = pastTime.addTicks(1);

        BSONObj doc = BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2));
        WriteUnitOfWork wunit(_opCtx);
        insertDocument(autoColl.getCollection(), InsertStatement(doc));
        wunit.commit();

        assertMultikeyPaths(
            _opCtx, autoColl.getCollection(), indexName, pastTime.asTimestamp(), true, {{0}});
        assertMultikeyPaths(
            _opCtx, autoColl.getCollection(), indexName, insertTime.asTimestamp(), true, {{0}});
    }
};

class InitializeMinValid : public StorageTimestampTest {
public:
    void run() {
        NamespaceString nss(repl::ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace);
        reset(nss);
        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
        auto minValidColl = autoColl.getCollection();

        repl::ReplicationConsistencyMarkersImpl consistencyMarkers(
            repl::StorageInterface::get(_opCtx));
        consistencyMarkers.initializeMinValidDocument(_opCtx);

        repl::MinValidDocument expectedMinValid;
        expectedMinValid.setMinValidTerm(repl::OpTime::kUninitializedTerm);
        expectedMinValid.setMinValidTimestamp(nullTs);

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValid);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValid);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValid);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValid);
    }
};

class SetMinValidInitialSyncFlag : public StorageTimestampTest {
public:
    void run() {
        NamespaceString nss(repl::ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace);
        reset(nss);
        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
        auto minValidColl = autoColl.getCollection();

        repl::ReplicationConsistencyMarkersImpl consistencyMarkers(
            repl::StorageInterface::get(_opCtx));
        ASSERT(consistencyMarkers.createInternalCollections(_opCtx).isOK());
        consistencyMarkers.initializeMinValidDocument(_opCtx);
        consistencyMarkers.setInitialSyncFlag(_opCtx);

        repl::MinValidDocument expectedMinValidWithSetFlag;
        expectedMinValidWithSetFlag.setMinValidTerm(repl::OpTime::kUninitializedTerm);
        expectedMinValidWithSetFlag.setMinValidTimestamp(nullTs);
        expectedMinValidWithSetFlag.setInitialSyncFlag(true);

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidWithSetFlag);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidWithSetFlag);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidWithSetFlag);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidWithSetFlag);

        consistencyMarkers.clearInitialSyncFlag(_opCtx);

        repl::MinValidDocument expectedMinValidWithUnsetFlag;
        expectedMinValidWithUnsetFlag.setMinValidTerm(presentTerm);
        expectedMinValidWithUnsetFlag.setMinValidTimestamp(presentTs);
        expectedMinValidWithUnsetFlag.setAppliedThrough(repl::OpTime(presentTs, presentTerm));

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidWithUnsetFlag);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidWithSetFlag);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidWithUnsetFlag);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidWithUnsetFlag);
    }
};

class SetMinValidToAtLeast : public StorageTimestampTest {
public:
    void run() {
        NamespaceString nss(repl::ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace);
        reset(nss);
        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
        auto minValidColl = autoColl.getCollection();

        repl::ReplicationConsistencyMarkersImpl consistencyMarkers(
            repl::StorageInterface::get(_opCtx));
        consistencyMarkers.initializeMinValidDocument(_opCtx);

        // Setting minValid sets it at the provided OpTime.
        consistencyMarkers.setMinValidToAtLeast(_opCtx, repl::OpTime(presentTs, presentTerm));

        repl::MinValidDocument expectedMinValidInit;
        expectedMinValidInit.setMinValidTerm(repl::OpTime::kUninitializedTerm);
        expectedMinValidInit.setMinValidTimestamp(nullTs);

        repl::MinValidDocument expectedMinValidPresent;
        expectedMinValidPresent.setMinValidTerm(presentTerm);
        expectedMinValidPresent.setMinValidTimestamp(presentTs);

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidInit);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidPresent);

        consistencyMarkers.setMinValidToAtLeast(_opCtx, repl::OpTime(futureTs, presentTerm));

        repl::MinValidDocument expectedMinValidFuture;
        expectedMinValidFuture.setMinValidTerm(presentTerm);
        expectedMinValidFuture.setMinValidTimestamp(futureTs);

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidFuture);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidInit);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidFuture);

        // Setting the timestamp to the past should be a noop.
        consistencyMarkers.setMinValidToAtLeast(_opCtx, repl::OpTime(pastTs, presentTerm));

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidFuture);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidInit);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidFuture);
    }
};

class SetMinValidAppliedThrough : public StorageTimestampTest {
public:
    void run() {
        NamespaceString nss(repl::ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace);
        reset(nss);
        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
        auto minValidColl = autoColl.getCollection();

        repl::ReplicationConsistencyMarkersImpl consistencyMarkers(
            repl::StorageInterface::get(_opCtx));
        consistencyMarkers.initializeMinValidDocument(_opCtx);

        consistencyMarkers.setAppliedThrough(_opCtx, repl::OpTime(presentTs, presentTerm));

        repl::MinValidDocument expectedMinValidInit;
        expectedMinValidInit.setMinValidTerm(repl::OpTime::kUninitializedTerm);
        expectedMinValidInit.setMinValidTimestamp(nullTs);

        repl::MinValidDocument expectedMinValidPresent;
        expectedMinValidPresent.setMinValidTerm(repl::OpTime::kUninitializedTerm);
        expectedMinValidPresent.setMinValidTimestamp(nullTs);
        expectedMinValidPresent.setAppliedThrough(repl::OpTime(presentTs, presentTerm));

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidInit);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidPresent);

        // appliedThrough opTime can be unset.
        consistencyMarkers.clearAppliedThrough(_opCtx, futureTs);

        assertMinValidDocumentAtTimestamp(minValidColl, nullTs, expectedMinValidInit);
        assertMinValidDocumentAtTimestamp(minValidColl, pastTs, expectedMinValidInit);
        assertMinValidDocumentAtTimestamp(minValidColl, presentTs, expectedMinValidPresent);
        assertMinValidDocumentAtTimestamp(minValidColl, futureTs, expectedMinValidInit);
    }
};

/**
 * This KVDropDatabase test only exists in this file for historical reasons, the final phase of
 * timestamping `dropDatabase` side-effects no longer applies. The purpose of this test is to
 * exercise the `StorageEngine::dropDatabase` method.
 */
template <bool SimulatePrimary>
class KVDropDatabase : public StorageTimestampTest {
public:
    void run() {
        auto storageInterface = repl::StorageInterface::get(_opCtx);
        repl::DropPendingCollectionReaper::set(
            _opCtx->getServiceContext(),
            stdx::make_unique<repl::DropPendingCollectionReaper>(storageInterface));

        auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
        auto kvCatalog = storageEngine->getCatalog();

        // Declare the database to be in a "synced" state, i.e: in steady-state replication.
        Timestamp syncTime = _clock->reserveTicks(1).asTimestamp();
        invariant(!syncTime.isNull());
        storageEngine->setInitialDataTimestamp(syncTime);

        // This test drops collections piece-wise instead of having the "drop database" algorithm
        // perform this walk. Defensively operate on a separate DB from the other tests to ensure
        // no leftover collections carry-over.
        const NamespaceString nss("unittestsDropDB.kvDropDatabase");
        const NamespaceString sysProfile("unittestsDropDB.system.profile");

        std::string collIdent;
        std::string indexIdent;
        std::string sysProfileIdent;
        // `*.system.profile` does not have an `_id` index. Just create it to abide by the API. This
        // value will be the empty string. Helper methods accommodate this.
        std::string sysProfileIndexIdent;
        for (auto& tuple : {std::tie(nss, collIdent, indexIdent),
                            std::tie(sysProfile, sysProfileIdent, sysProfileIndexIdent)}) {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_X);

            // Save the pre-state idents so we can capture the specific idents related to collection
            // creation.
            std::vector<std::string> origIdents = kvCatalog->getAllIdents(_opCtx);
            const auto& nss = std::get<0>(tuple);

            // Non-replicated namespaces are wrapped in an unreplicated writes block. This has the
            // side-effect of not timestamping the collection creation.
            repl::UnreplicatedWritesBlock notReplicated(_opCtx);
            if (nss.isReplicated()) {
                TimestampBlock tsBlock(_opCtx, _clock->reserveTicks(1).asTimestamp());
                reset(nss);
            } else {
                reset(nss);
            }

            // Bind the local values to the variables in the parent scope.
            auto& collIdent = std::get<1>(tuple);
            auto& indexIdent = std::get<2>(tuple);
            std::tie(collIdent, indexIdent) = getNewCollectionIndexIdent(kvCatalog, origIdents);
        }

        AutoGetCollection coll(_opCtx, nss, LockMode::MODE_X);
        {
            // Drop/rename `kvDropDatabase`. `system.profile` does not get dropped/renamed.
            WriteUnitOfWork wuow(_opCtx);
            Database* db = coll.getDb();
            ASSERT_OK(db->dropCollection(_opCtx, nss));
            wuow.commit();
        }

        // Reserve a tick, this represents a time after the rename in which the `kvDropDatabase`
        // ident for `kvDropDatabase` still exists.
        const Timestamp postRenameTime = _clock->reserveTicks(1).asTimestamp();

        // If the storage engine is managing drops internally, the ident should not be visible after
        // a drop.
        if (storageEngine->supportsPendingDrops()) {
            assertIdentsMissingAtTimestamp(kvCatalog, collIdent, indexIdent, postRenameTime);
        } else {
            // The namespace has changed, but the ident still exists as-is after the rename.
            assertIdentsExistAtTimestamp(kvCatalog, collIdent, indexIdent, postRenameTime);
        }

        const Timestamp dropTime = _clock->reserveTicks(1).asTimestamp();
        if (SimulatePrimary) {
            ASSERT_OK(dropDatabase(_opCtx, nss.db().toString()));
        } else {
            repl::UnreplicatedWritesBlock uwb(_opCtx);
            TimestampBlock ts(_opCtx, dropTime);
            ASSERT_OK(dropDatabase(_opCtx, nss.db().toString()));
        }

        // Assert that the idents do not exist.
        assertIdentsMissingAtTimestamp(
            kvCatalog, sysProfileIdent, sysProfileIndexIdent, Timestamp::max());
        assertIdentsMissingAtTimestamp(kvCatalog, collIdent, indexIdent, Timestamp::max());

        // dropDatabase must not timestamp the final write. The collection and index should seem
        // to have never existed.
        assertIdentsMissingAtTimestamp(kvCatalog, collIdent, indexIdent, syncTime);
    }
};

/**
 * This test asserts that the catalog updates that represent the beginning and end of an index
 * build are timestamped. Additionally, the index will be `multikey` and that catalog update that
 * finishes the index build will also observe the index is multikey.
 *
 * Primaries log no-ops when starting an index build to acquire a timestamp. A primary committing
 * an index build gets timestamped when the `createIndexes` command creates an oplog entry. That
 * step is mimiced here.
 *
 * Secondaries timestamp starting their index build by being in a `TimestampBlock` when the oplog
 * entry is processed. Secondaries will look at the logical clock when completing the index
 * build. This is safe so long as completion is not racing with secondary oplog application (i.e:
 * enforced via the parallel batch writer mode lock).
 */
template <bool SimulatePrimary>
class TimestampIndexBuilds : public StorageTimestampTest {
public:
    void run() {
        const bool SimulateSecondary = !SimulatePrimary;
        if (SimulateSecondary) {
            // The MemberState is inspected during index builds to use a "ghost" write to timestamp
            // index completion.
            ASSERT_OK(_coordinatorMock->setFollowerMode({repl::MemberState::MS::RS_SECONDARY}));
        }

        auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
        auto kvCatalog = storageEngine->getCatalog();

        NamespaceString nss("unittests.timestampIndexBuilds");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_X);

        const LogicalTime insertTimestamp = _clock->reserveTicks(1);
        {
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(autoColl.getCollection(),
                           InsertStatement(BSON("_id" << 0 << "a" << BSON_ARRAY(1 << 2)),
                                           insertTimestamp.asTimestamp(),
                                           presentTerm));
            wuow.commit();
            ASSERT_EQ(1, itCount(autoColl.getCollection()));
        }

        // Save the pre-state idents so we can capture the specific ident related to index
        // creation.
        std::vector<std::string> origIdents = kvCatalog->getAllIdents(_opCtx);

        // Build an index on `{a: 1}`. This index will be multikey.
        MultiIndexBlock indexer;
        ON_BLOCK_EXIT([&] { indexer.cleanUpAfterBuild(_opCtx, autoColl.getCollection()); });
        const LogicalTime beforeIndexBuild = _clock->reserveTicks(2);
        BSONObj indexInfoObj;
        {
            // Primaries do not have a wrapping `TimestampBlock`; secondaries do.
            const Timestamp commitTimestamp =
                SimulatePrimary ? Timestamp::min() : beforeIndexBuild.addTicks(1).asTimestamp();
            TimestampBlock tsBlock(_opCtx, commitTimestamp);

            // Secondaries will also be in an `UnreplicatedWritesBlock` that prevents the `logOp`
            // from making creating an entry.
            boost::optional<repl::UnreplicatedWritesBlock> unreplicated;
            if (SimulateSecondary) {
                unreplicated.emplace(_opCtx);
            }

            auto swIndexInfoObj = indexer.init(
                _opCtx,
                autoColl.getCollection(),
                {BSON("v" << 2 << "unique" << true << "name"
                          << "a_1"
                          << "ns"
                          << nss.ns()
                          << "key"
                          << BSON("a" << 1))},
                MultiIndexBlock::makeTimestampedIndexOnInitFn(_opCtx, autoColl.getCollection()));
            ASSERT_OK(swIndexInfoObj.getStatus());
            indexInfoObj = std::move(swIndexInfoObj.getValue()[0]);
        }

        const LogicalTime afterIndexInit = _clock->reserveTicks(2);

        // Inserting all the documents has the side-effect of setting internal state on the index
        // builder that the index is multikey.
        ASSERT_OK(indexer.insertAllDocumentsInCollection(_opCtx, autoColl.getCollection()));

        {
            WriteUnitOfWork wuow(_opCtx);
            // All callers of `MultiIndexBlock::commit` are responsible for timestamping index
            // completion  Primaries write an oplog entry. Secondaries explicitly set a
            // timestamp.
            ASSERT_OK(indexer.commit(
                _opCtx,
                autoColl.getCollection(),
                [&](const BSONObj& indexSpec) {
                    if (SimulatePrimary) {
                        // The timestamping responsibility for each index is placed on the caller.
                        _opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                            _opCtx, nss, *(autoColl.getCollection()->uuid()), indexSpec, false);
                    } else {
                        ASSERT_OK(_opCtx->recoveryUnit()->setTimestamp(
                            _clock->getClusterTime().asTimestamp()));
                    }
                },
                MultiIndexBlock::kNoopOnCommitFn));
            wuow.commit();
        }

        const Timestamp afterIndexBuild = _clock->reserveTicks(1).asTimestamp();

        const std::string indexIdent =
            getNewIndexIdentAtTime(kvCatalog, origIdents, Timestamp::min());
        assertIdentsMissingAtTimestamp(kvCatalog, "", indexIdent, beforeIndexBuild.asTimestamp());

        // Assert that the index entry exists after init and `ready: false`.
        assertIdentsExistAtTimestamp(kvCatalog, "", indexIdent, afterIndexInit.asTimestamp());
        {
            ASSERT_FALSE(getIndexMetaData(
                             getMetaDataAtTime(kvCatalog, nss, afterIndexInit.asTimestamp()), "a_1")
                             .ready);
        }

        // After the build completes, assert that the index is `ready: true` and multikey.
        assertIdentsExistAtTimestamp(kvCatalog, "", indexIdent, afterIndexBuild);
        {
            auto indexMetaData =
                getIndexMetaData(getMetaDataAtTime(kvCatalog, nss, afterIndexBuild), "a_1");
            ASSERT(indexMetaData.ready);
            ASSERT(indexMetaData.multikey);

            ASSERT_EQ(std::size_t(1), indexMetaData.multikeyPaths.size());
            const bool match = indexMetaData.multikeyPaths[0] == std::set<std::size_t>({0});
            if (!match) {
                FAIL(str::stream() << "Expected: [ [ 0 ] ] Actual: "
                                   << dumpMultikeyPaths(indexMetaData.multikeyPaths));
            }
        }
    }
};

template <bool SimulatePrimary>
class TimestampIndexBuildDrain : public StorageTimestampTest {
public:
    void run() {
        const bool SimulateSecondary = !SimulatePrimary;
        if (SimulateSecondary) {
            // The MemberState is inspected during index builds to use a "ghost" write to timestamp
            // index completion.
            ASSERT_OK(_coordinatorMock->setFollowerMode({repl::MemberState::MS::RS_SECONDARY}));
        }

        NamespaceString nss("unittests.timestampIndexBuildDrain");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_X);

        // Build an index on `{a: 1}`.
        MultiIndexBlock indexer;
        ON_BLOCK_EXIT([&] { indexer.cleanUpAfterBuild(_opCtx, autoColl.getCollection()); });
        const LogicalTime beforeIndexBuild = _clock->reserveTicks(2);
        BSONObj indexInfoObj;
        {
            // Primaries do not have a wrapping `TimestampBlock`; secondaries do.
            const Timestamp commitTimestamp =
                SimulatePrimary ? Timestamp::min() : beforeIndexBuild.addTicks(1).asTimestamp();
            TimestampBlock tsBlock(_opCtx, commitTimestamp);

            // Secondaries will also be in an `UnreplicatedWritesBlock` that prevents the `logOp`
            // from making creating an entry.
            boost::optional<repl::UnreplicatedWritesBlock> unreplicated;
            if (SimulateSecondary) {
                unreplicated.emplace(_opCtx);
            }

            auto swIndexInfoObj = indexer.init(
                _opCtx,
                autoColl.getCollection(),
                {BSON("v" << 2 << "unique" << true << "name"
                          << "a_1"
                          << "ns"
                          << nss.ns()
                          << "key"
                          << BSON("a" << 1))},
                MultiIndexBlock::makeTimestampedIndexOnInitFn(_opCtx, autoColl.getCollection()));
            ASSERT_OK(swIndexInfoObj.getStatus());
            indexInfoObj = std::move(swIndexInfoObj.getValue()[0]);
        }

        const LogicalTime afterIndexInit = _clock->reserveTicks(1);

        // Insert a document that will be intercepted and need to be drained. This timestamp will
        // become the lastApplied time.
        const LogicalTime firstInsert = _clock->reserveTicks(1);
        {
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(autoColl.getCollection(),
                           InsertStatement(BSON("_id" << 0 << "a" << 1),
                                           firstInsert.asTimestamp(),
                                           presentTerm));
            wuow.commit();
            ASSERT_EQ(1, itCount(autoColl.getCollection()));
        }

        // Index build drain will timestamp writes from the side table into the index with the
        // lastApplied timestamp. This is because these writes are not associated with any specific
        // oplog entry.
        ASSERT_EQ(repl::ReplicationCoordinator::get(getGlobalServiceContext())
                      ->getMyLastAppliedOpTime()
                      .getTimestamp(),
                  firstInsert.asTimestamp());

        ASSERT_OK(indexer.drainBackgroundWrites(_opCtx));

        auto indexCatalog = autoColl.getCollection()->getIndexCatalog();
        const IndexCatalogEntry* buildingIndex = indexCatalog->getEntry(
            indexCatalog->findIndexByName(_opCtx, "a_1", /* includeUnfinished */ true));
        ASSERT(buildingIndex);

        {
            // Before the drain, there are no writes to apply.
            OneOffRead oor(_opCtx, afterIndexInit.asTimestamp());
            ASSERT_TRUE(buildingIndex->indexBuildInterceptor()->areAllWritesApplied(_opCtx));
        }

        // Note: In this case, we can't observe a state where all writes are not applied, because
        // the index build drain effectively rewrites history by retroactively committing the drain
        // at the same time as the first insert, meaning there is no point-in-time with undrained
        // writes. This is fine, as long as the drain does not commit at a time before this insert.

        {
            // At time of the first insert, all writes are applied.
            OneOffRead oor(_opCtx, firstInsert.asTimestamp());
            ASSERT_TRUE(buildingIndex->indexBuildInterceptor()->areAllWritesApplied(_opCtx));
        }

        // Insert a second document that will be intercepted and need to be drained.
        const LogicalTime secondInsert = _clock->reserveTicks(1);
        {
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(autoColl.getCollection(),
                           InsertStatement(BSON("_id" << 1 << "a" << 2),
                                           secondInsert.asTimestamp(),
                                           presentTerm));
            wuow.commit();
            ASSERT_EQ(2, itCount(autoColl.getCollection()));
        }

        // Advance the lastApplied optime to observe a point before the drain where there are
        // un-drained writes.
        const LogicalTime afterSecondInsert = _clock->reserveTicks(1);
        setReplCoordAppliedOpTime(repl::OpTime(afterSecondInsert.asTimestamp(), presentTerm));

        ASSERT_OK(indexer.drainBackgroundWrites(_opCtx));

        {
            // At time of the second insert, there are un-drained writes.
            OneOffRead oor(_opCtx, secondInsert.asTimestamp());
            ASSERT_FALSE(buildingIndex->indexBuildInterceptor()->areAllWritesApplied(_opCtx));
        }

        {
            // After the second insert, also the lastApplied time, all writes are applied.
            OneOffRead oor(_opCtx, afterSecondInsert.asTimestamp());
            ASSERT_TRUE(buildingIndex->indexBuildInterceptor()->areAllWritesApplied(_opCtx));
        }

        {
            WriteUnitOfWork wuow(_opCtx);
            ASSERT_OK(indexer.commit(
                _opCtx,
                autoColl.getCollection(),
                [&](const BSONObj& indexSpec) {
                    if (SimulatePrimary) {
                        // The timestamping responsibility for each index is placed on the caller.
                        _opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                            _opCtx, nss, *(autoColl.getCollection()->uuid()), indexSpec, false);
                    } else {
                        ASSERT_OK(_opCtx->recoveryUnit()->setTimestamp(
                            _clock->getClusterTime().asTimestamp()));
                    }
                },
                MultiIndexBlock::kNoopOnCommitFn));
            wuow.commit();
        }
    }
};

class TimestampMultiIndexBuilds : public StorageTimestampTest {
public:
    void run() {
        auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
        auto kvCatalog = storageEngine->getCatalog();

        NamespaceString nss("unittests.timestampMultiIndexBuilds");
        reset(nss);

        std::vector<std::string> origIdents;
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_X);

            const LogicalTime insertTimestamp = _clock->reserveTicks(1);

            WriteUnitOfWork wuow(_opCtx);
            insertDocument(autoColl.getCollection(),
                           InsertStatement(BSON("_id" << 0 << "a" << 1 << "b" << 2 << "c" << 3),
                                           insertTimestamp.asTimestamp(),
                                           presentTerm));
            wuow.commit();
            ASSERT_EQ(1, itCount(autoColl.getCollection()));

            // Save the pre-state idents so we can capture the specific ident related to index
            // creation.
            origIdents = kvCatalog->getAllIdents(_opCtx);
        }

        DBDirectClient client(_opCtx);
        {
            IndexSpec index1;
            // Name this index for easier querying.
            index1.addKeys(BSON("a" << 1)).name("a_1");
            IndexSpec index2;
            index2.addKeys(BSON("b" << 1)).name("b_1");

            std::vector<const IndexSpec*> indexes;
            indexes.push_back(&index1);
            indexes.push_back(&index2);
            client.createIndexes(nss.ns(), indexes);
        }

        const Timestamp indexCreateInitTs = queryOplog(BSON("op"
                                                            << "n"))["ts"]
                                                .timestamp();

        const Timestamp indexAComplete = queryOplog(BSON("op"
                                                         << "c"
                                                         << "o.createIndexes"
                                                         << nss.coll()
                                                         << "o.name"
                                                         << "a_1"))["ts"]
                                             .timestamp();

        const auto indexBComplete =
            Timestamp(indexAComplete.getSecs(), indexAComplete.getInc() + 1);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_S);

        // The idents are created and persisted with the "ready: false" write. There should be two
        // new index idents visible at this time.
        const std::vector<std::string> indexes =
            getNewIndexIdentsAtTime(kvCatalog, origIdents, indexCreateInitTs);
        ASSERT_EQ(static_cast<std::size_t>(2), indexes.size()) << " Num idents: " << indexes.size();

        ASSERT_FALSE(
            getIndexMetaData(getMetaDataAtTime(kvCatalog, nss, indexCreateInitTs), "a_1").ready);
        ASSERT_FALSE(
            getIndexMetaData(getMetaDataAtTime(kvCatalog, nss, indexCreateInitTs), "b_1").ready);

        // Assert the `a_1` index becomes ready at the next oplog entry time.
        ASSERT_TRUE(
            getIndexMetaData(getMetaDataAtTime(kvCatalog, nss, indexAComplete), "a_1").ready);
        ASSERT_FALSE(
            getIndexMetaData(getMetaDataAtTime(kvCatalog, nss, indexAComplete), "b_1").ready);

        // Assert the `b_1` index becomes ready at the last oplog entry time.
        ASSERT_TRUE(
            getIndexMetaData(getMetaDataAtTime(kvCatalog, nss, indexBComplete), "a_1").ready);
        ASSERT_TRUE(
            getIndexMetaData(getMetaDataAtTime(kvCatalog, nss, indexBComplete), "b_1").ready);
    }
};

class TimestampMultiIndexBuildsDuringRename : public StorageTimestampTest {
public:
    void run() {
        auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
        auto kvCatalog = storageEngine->getCatalog();

        NamespaceString nss("unittests.timestampMultiIndexBuildsDuringRename");
        reset(nss);

        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_X);

            const LogicalTime insertTimestamp = _clock->reserveTicks(1);

            WriteUnitOfWork wuow(_opCtx);
            insertDocument(autoColl.getCollection(),
                           InsertStatement(BSON("_id" << 0 << "a" << 1 << "b" << 2 << "c" << 3),
                                           insertTimestamp.asTimestamp(),
                                           presentTerm));
            wuow.commit();
            ASSERT_EQ(1, itCount(autoColl.getCollection()));
        }

        DBDirectClient client(_opCtx);
        {
            IndexSpec index1;
            // Name this index for easier querying.
            index1.addKeys(BSON("a" << 1)).name("a_1");
            IndexSpec index2;
            index2.addKeys(BSON("b" << 1)).name("b_1");

            std::vector<const IndexSpec*> indexes;
            indexes.push_back(&index1);
            indexes.push_back(&index2);
            client.createIndexes(nss.ns(), indexes);
        }

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_X);

        NamespaceString renamedNss("unittestsRename.timestampMultiIndexBuildsDuringRename");
        reset(renamedNss);

        // Save the pre-state idents so we can capture the specific ident related to index
        // creation.
        std::vector<std::string> origIdents = kvCatalog->getAllIdents(_opCtx);

        // Rename collection.
        BSONObj renameResult;
        client.runCommand(
            "admin",
            BSON("renameCollection" << nss.ns() << "to" << renamedNss.ns() << "dropTarget" << true),
            renameResult);

        const auto createIndexesDocument = queryOplog(BSON("ns" << renamedNss.db() + ".$cmd"
                                                                << "o.createIndexes"
                                                                << BSON("$exists" << true)));

        // Find index creation timestamps.
        const auto createIndexesString =
            createIndexesDocument["o"].embeddedObject()["createIndexes"].toString();
        const std::string filterString = "createIndexes: \"";
        const NamespaceString tmpName(
            renamedNss.db(),
            createIndexesString.substr(filterString.size(),
                                       createIndexesString.size() - filterString.size() - 1));

        const Timestamp indexCreateInitTs = queryOplog(BSON("op"
                                                            << "c"
                                                            << "o.create"
                                                            << tmpName.coll()))["ts"]
                                                .timestamp();

        const Timestamp indexAComplete = createIndexesDocument["ts"].timestamp();
        const Timestamp indexBComplete = queryOplog(BSON("op"
                                                         << "c"
                                                         << "o.createIndexes"
                                                         << tmpName.coll()
                                                         << "o.name"
                                                         << "b_1"))["ts"]
                                             .timestamp();

        // We expect one new collection ident and one new index ident (the _id index) during this
        // rename.
        assertRenamedCollectionIdentsAtTimestamp(
            kvCatalog, origIdents, /*expectedNewIndexIdents*/ 1, indexCreateInitTs);

        // We expect one new collection ident and three new index idents (including the _id index)
        // after this rename. The a_1 and b_1 index idents are created and persisted with the
        // "ready: true" write.
        assertRenamedCollectionIdentsAtTimestamp(
            kvCatalog, origIdents, /*expectedNewIndexIdents*/ 3, indexBComplete);

        // Assert the `a_1` index becomes ready at the next oplog entry time.
        ASSERT_TRUE(
            getIndexMetaData(getMetaDataAtTime(kvCatalog, renamedNss, indexAComplete), "a_1")
                .ready);
        assertIndexMetaDataMissing(getMetaDataAtTime(kvCatalog, renamedNss, indexAComplete), "b_1");

        // Assert the `b_1` index becomes ready at the last oplog entry time.
        ASSERT_TRUE(
            getIndexMetaData(getMetaDataAtTime(kvCatalog, renamedNss, indexBComplete), "a_1")
                .ready);
        ASSERT_TRUE(
            getIndexMetaData(getMetaDataAtTime(kvCatalog, renamedNss, indexBComplete), "b_1")
                .ready);
    }
};

class TimestampIndexDrops : public StorageTimestampTest {
public:
    void run() {
        auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
        auto kvCatalog = storageEngine->getCatalog();

        NamespaceString nss("unittests.timestampIndexDrops");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_X);

        const LogicalTime insertTimestamp = _clock->reserveTicks(1);
        {
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(autoColl.getCollection(),
                           InsertStatement(BSON("_id" << 0 << "a" << 1 << "b" << 2 << "c" << 3),
                                           insertTimestamp.asTimestamp(),
                                           presentTerm));
            wuow.commit();
            ASSERT_EQ(1, itCount(autoColl.getCollection()));
        }


        const Timestamp beforeIndexBuild = _clock->reserveTicks(1).asTimestamp();

        // Save the pre-state idents so we can capture the specific ident related to index
        // creation.
        std::vector<std::string> origIdents = kvCatalog->getAllIdents(_opCtx);

        std::vector<Timestamp> afterCreateTimestamps;
        std::vector<std::string> indexIdents;
        // Create an index and get the ident for each index.
        for (auto key : {"a", "b", "c"}) {
            createIndex(autoColl.getCollection(), str::stream() << key << "_1", BSON(key << 1));

            // Timestamps at the completion of each index build.
            afterCreateTimestamps.push_back(_clock->reserveTicks(1).asTimestamp());

            // Add the new ident to the vector and reset the current idents.
            indexIdents.push_back(getNewIndexIdentAtTime(kvCatalog, origIdents, Timestamp::min()));
            origIdents = kvCatalog->getAllIdents(_opCtx);
        }

        // Ensure each index is visible at the correct timestamp, and not before.
        for (size_t i = 0; i < indexIdents.size(); i++) {
            auto beforeTs = (i == 0) ? beforeIndexBuild : afterCreateTimestamps[i - 1];
            assertIdentsMissingAtTimestamp(kvCatalog, "", indexIdents[i], beforeTs);
            assertIdentsExistAtTimestamp(kvCatalog, "", indexIdents[i], afterCreateTimestamps[i]);
        }

        const LogicalTime beforeDropTs = _clock->getClusterTime();

        // Drop all of the indexes.
        BSONObjBuilder result;
        ASSERT_OK(dropIndexes(_opCtx,
                              nss,
                              BSON("index"
                                   << "*"),
                              &result));

        // Assert that each index is dropped individually and with its own timestamp. The order of
        // dropping and creating are not guaranteed to be the same, but assert all of the created
        // indexes were also dropped.
        size_t nIdents = indexIdents.size();
        for (size_t i = 0; i < nIdents; i++) {
            OneOffRead oor(_opCtx, beforeDropTs.addTicks(i + 1).asTimestamp());

            auto ident = getDroppedIndexIdent(kvCatalog, origIdents);
            indexIdents.erase(std::remove(indexIdents.begin(), indexIdents.end(), ident));

            origIdents = kvCatalog->getAllIdents(_opCtx);
        }
        ASSERT_EQ(indexIdents.size(), 0ul) << "Dropped idents should match created idents";
    }
};

class SecondaryReadsDuringBatchApplicationAreAllowed : public StorageTimestampTest {
public:
    void run() {
        ASSERT(_opCtx->getServiceContext()->getStorageEngine()->supportsReadConcernSnapshot());

        NamespaceString ns("unittest.secondaryReadsDuringBatchApplicationAreAllowed");
        reset(ns);
        UUID uuid = UUID::gen();
        {
            AutoGetCollectionForRead autoColl(_opCtx, ns);
            uuid = autoColl.getCollection()->uuid().get();
            ASSERT_EQ(itCount(autoColl.getCollection()), 0);
        }

        // Returns true when the batch has started, meaning the applier is holding the PBWM lock.
        // Will return false if the lock was not held.
        auto batchInProgress = makePromiseFuture<bool>();
        // Attempt to read when in the middle of a batch.
        stdx::packaged_task<bool()> task([&] {
            Client::initThread(getThreadName());
            auto readOp = cc().makeOperationContext();

            // Wait for the batch to start or fail.
            if (!batchInProgress.future.get()) {
                return false;
            }
            AutoGetCollectionForRead autoColl(readOp.get(), ns);
            return !readOp->lockState()->isLockHeldForMode(resourceIdParallelBatchWriterMode,
                                                           MODE_IS);
        });
        auto taskFuture = task.get_future();
        stdx::thread taskThread{std::move(task)};

        auto joinGuard = makeGuard([&] {
            batchInProgress.promise.emplaceValue(false);
            taskThread.join();
        });

        // This apply operation function will block until the reader has tried acquiring a
        // collection lock. This returns BadValue statuses instead of asserting so that the worker
        // threads can cleanly exit and this test case fails without crashing the entire suite.
        auto applyOperationFn = [&](OperationContext* opCtx,
                                    std::vector<const repl::OplogEntry*>* operationsToApply,
                                    repl::SyncTail* st,
                                    std::vector<MultikeyPathInfo>* pathInfo) -> Status {
            if (!_opCtx->lockState()->isLockHeldForMode(resourceIdParallelBatchWriterMode,
                                                        MODE_X)) {
                return {ErrorCodes::BadValue, "Batch applied was not holding PBWM lock in MODE_X"};
            }

            // Insert the document. A reader without a PBWM lock should not see it yet.
            auto status = repl::multiSyncApply(opCtx, operationsToApply, st, pathInfo);
            if (!status.isOK()) {
                return status;
            }

            // Signals the reader to acquire a collection read lock.
            batchInProgress.promise.emplaceValue(true);

            // Block while holding the PBWM lock until the reader is done.
            if (!taskFuture.get()) {
                return {ErrorCodes::BadValue, "Client was holding PBWM lock in MODE_IS"};
            }
            return Status::OK();
        };

        // Make a simple insert operation.
        BSONObj doc0 = BSON("_id" << 0 << "a" << 0);
        auto insertOp = repl::OplogEntry(BSON("ts" << futureTs << "t" << 1LL << "v" << 2 << "op"
                                                   << "i"
                                                   << "ns"
                                                   << ns.ns()
                                                   << "ui"
                                                   << uuid
                                                   << "o"
                                                   << doc0));

        // Apply the operation.
        auto storageInterface = repl::StorageInterface::get(_opCtx);
        auto writerPool = repl::OplogApplier::makeWriterPool(1);
        repl::SyncTail syncTail(
            nullptr, _consistencyMarkers, storageInterface, applyOperationFn, writerPool.get());
        auto lastOpTime = unittest::assertGet(syncTail.multiApply(_opCtx, {insertOp}, boost::none));
        ASSERT_EQ(insertOp.getOpTime(), lastOpTime);

        joinGuard.dismiss();
        taskThread.join();

        // Read on the local snapshot to verify the document was inserted.
        AutoGetCollectionForRead autoColl(_opCtx, ns);
        assertDocumentAtTimestamp(autoColl.getCollection(), futureTs, doc0);
    }
};

/**
 * There are a few scenarios where a primary will be using the IndexBuilder thread to build
 * indexes. Specifically, when a primary builds an index from an oplog entry which can happen on
 * primary catch-up, drain, a secondary step-up or `applyOps`.
 *
 * This test will exercise IndexBuilder code on primaries by performing an index build via an
 * `applyOps` command.
 */
class TimestampIndexBuilderOnPrimary : public StorageTimestampTest {
public:
    void run() {
        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        std::string dbName = "unittest";
        NamespaceString nss(dbName, "indexBuilderOnPrimary");
        BSONObj doc = BSON("_id" << 1 << "field" << 1);

        const LogicalTime setupStart = _clock->reserveTicks(1);

        UUID collUUID = UUID::gen();
        {
            // Create the collection and insert a document.
            reset(nss);
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
            collUUID = *(autoColl.getCollection()->uuid());
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(autoColl.getCollection(),
                           InsertStatement(doc, setupStart.asTimestamp(), presentTerm));
            wuow.commit();
        }


        {
            // Sanity check everything exists.
            AutoGetCollectionForReadCommand autoColl(_opCtx, nss);
            auto coll = autoColl.getCollection();
            ASSERT(coll);

            const auto presentTs = _clock->getClusterTime().asTimestamp();
            assertDocumentAtTimestamp(coll, presentTs, doc);
        }

        {
            // Create an index via `applyOps`. Because this is a primary, the index build is
            // timestamped with `startBuildTs`.
            const auto beforeBuildTime = _clock->reserveTicks(2);
            const auto startBuildTs = beforeBuildTime.addTicks(1).asTimestamp();

            // Grab the existing idents to identify the ident created by the index build.
            auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
            auto kvCatalog = storageEngine->getCatalog();
            std::vector<std::string> origIdents;
            {
                AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IS, LockMode::MODE_IS);
                origIdents = kvCatalog->getAllIdents(_opCtx);
            }

            auto indexSpec = BSON("createIndexes" << nss.coll() << "ns" << nss.ns() << "v"
                                                  << static_cast<int>(kIndexVersion)
                                                  << "key"
                                                  << BSON("field" << 1)
                                                  << "name"
                                                  << "field_1");

            auto createIndexOp = BSON("ts" << startBuildTs << "t" << 1LL << "v" << 2 << "op"
                                           << "c"
                                           << "ns"
                                           << nss.getCommandNS().ns()
                                           << "ui"
                                           << collUUID
                                           << "o"
                                           << indexSpec);

            ASSERT_OK(doAtomicApplyOps(nss.db().toString(), {createIndexOp}));

            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IS, LockMode::MODE_IS);
            const std::string indexIdent =
                getNewIndexIdentAtTime(kvCatalog, origIdents, Timestamp::min());
            assertIdentsMissingAtTimestamp(
                kvCatalog, "", indexIdent, beforeBuildTime.asTimestamp());
            assertIdentsExistAtTimestamp(kvCatalog, "", indexIdent, startBuildTs);

            // On a primary, the index build should start and finish at `startBuildTs` because it is
            // built in the foreground.
            ASSERT_TRUE(
                getIndexMetaData(getMetaDataAtTime(kvCatalog, nss, startBuildTs), "field_1").ready);
        }
    }
};

class ViewCreationSeparateTransaction : public StorageTimestampTest {
public:
    void run() {
        auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
        auto kvCatalog = storageEngine->getCatalog();

        const NamespaceString backingCollNss("unittests.backingColl");
        reset(backingCollNss);

        const NamespaceString viewNss("unittests.view");
        const NamespaceString systemViewsNss("unittests.system.views");

        ASSERT_OK(createCollection(_opCtx,
                                   viewNss.db().toString(),
                                   BSON("create" << viewNss.coll() << "pipeline" << BSONArray()
                                                 << "viewOn"
                                                 << backingCollNss.coll())));

        const Timestamp systemViewsCreateTs = queryOplog(BSON("op"
                                                              << "c"
                                                              << "ns"
                                                              << (viewNss.db() + ".$cmd")
                                                              << "o.create"
                                                              << "system.views"))["ts"]
                                                  .timestamp();
        const Timestamp viewCreateTs = queryOplog(BSON("op"
                                                       << "i"
                                                       << "ns"
                                                       << systemViewsNss.ns()
                                                       << "o._id"
                                                       << viewNss.ns()))["ts"]
                                           .timestamp();

        {
            Lock::GlobalRead read(_opCtx);
            auto systemViewsMd = getMetaDataAtTime(
                kvCatalog, systemViewsNss, Timestamp(systemViewsCreateTs.asULL() - 1));
            ASSERT_EQ("", systemViewsMd.ns)
                << systemViewsNss
                << " incorrectly exists before creation. CreateTs: " << systemViewsCreateTs;

            systemViewsMd = getMetaDataAtTime(kvCatalog, systemViewsNss, systemViewsCreateTs);
            ASSERT_EQ(systemViewsNss.ns(), systemViewsMd.ns);

            AutoGetCollection autoColl(_opCtx, systemViewsNss, LockMode::MODE_IS);
            assertDocumentAtTimestamp(autoColl.getCollection(), systemViewsCreateTs, BSONObj());
            assertDocumentAtTimestamp(
                autoColl.getCollection(),
                viewCreateTs,
                BSON("_id" << viewNss.ns() << "viewOn" << backingCollNss.coll() << "pipeline"
                           << BSONArray()));
        }
    }
};

class CreateCollectionWithSystemIndex : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (!(mongo::storageGlobalParams.engine == "wiredTiger" &&
              mongo::serverGlobalParams.enableMajorityReadConcern)) {
            return;
        }

        const LogicalTime indexCreateLt = futureLt.addTicks(1);
        const Timestamp indexCreateTs = indexCreateLt.asTimestamp();

        NamespaceString nss("admin.system.users");

        { ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx, nss).getCollection()); }

        ASSERT_OK(createCollection(_opCtx, nss.db().toString(), BSON("create" << nss.coll())));

        { ASSERT(AutoGetCollectionForReadCommand(_opCtx, nss).getCollection()); }

        BSONObj result = queryOplog(BSON("op"
                                         << "c"
                                         << "ns"
                                         << nss.getCommandNS().ns()
                                         << "o.create"
                                         << nss.coll()));
        repl::OplogEntry op(result);
        // The logOp() call for createCollection should have timestamp 'futureTs', which will also
        // be the timestamp at which we do the write which creates the collection. Thus we expect
        // the collection to appear at 'futureTs' and not before.
        ASSERT_EQ(op.getTimestamp(), futureTs) << op.toBSON();

        assertNamespaceInIdents(nss, pastTs, false);
        assertNamespaceInIdents(nss, presentTs, false);
        assertNamespaceInIdents(nss, futureTs, true);
        assertNamespaceInIdents(nss, indexCreateTs, true);
        assertNamespaceInIdents(nss, nullTs, true);

        result = queryOplog(BSON("op"
                                 << "c"
                                 << "ns"
                                 << nss.getCommandNS().ns()
                                 << "o.createIndexes"
                                 << nss.coll()));
        repl::OplogEntry indexOp(result);
        ASSERT_EQ(indexOp.getObject()["name"].str(), "user_1_db_1");
        ASSERT_GT(indexOp.getTimestamp(), futureTs) << op.toBSON();
        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IS, LockMode::MODE_IS);
        auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
        auto kvCatalog = storageEngine->getCatalog();
        auto indexIdent = kvCatalog->getIndexIdent(_opCtx, nss, "user_1_db_1");
        assertIdentsMissingAtTimestamp(kvCatalog, "", indexIdent, pastTs);
        assertIdentsMissingAtTimestamp(kvCatalog, "", indexIdent, presentTs);
        assertIdentsMissingAtTimestamp(kvCatalog, "", indexIdent, futureTs);
        assertIdentsExistAtTimestamp(kvCatalog, "", indexIdent, indexCreateTs);
        assertIdentsExistAtTimestamp(kvCatalog, "", indexIdent, nullTs);
    }
};

class MultiDocumentTransactionTest : public StorageTimestampTest {
public:
    const StringData dbName = "unittest"_sd;
    const BSONObj doc = BSON("_id" << 1 << "TestValue" << 1);

    MultiDocumentTransactionTest(const std::string& collName) : nss(dbName, collName) {
        auto service = _opCtx->getServiceContext();
        auto sessionCatalog = SessionCatalog::get(service);
        sessionCatalog->reset_forTest();
        MongoDSessionCatalog::onStepUp(_opCtx);

        reset(nss);
        UUID ui = UUID::gen();
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
            auto coll = autoColl.getCollection();
            ASSERT(coll);
            ui = coll->uuid().get();
        }

        presentTs = _clock->getClusterTime().asTimestamp();
        // This test does not run a real ReplicationCoordinator, so must advance the snapshot
        // manager manually.
        auto storageEngine = cc().getServiceContext()->getStorageEngine();
        storageEngine->getSnapshotManager()->setLocalSnapshot(presentTs);
        const auto beforeTxnTime = _clock->reserveTicks(1);
        beforeTxnTs = beforeTxnTime.asTimestamp();
        commitEntryTs = beforeTxnTime.addTicks(1).asTimestamp();

        const auto sessionId = makeLogicalSessionIdForTest();
        _opCtx->setLogicalSessionId(sessionId);
        _opCtx->setTxnNumber(26);

        ocs.emplace(_opCtx);

        auto txnParticipant = TransactionParticipant::get(_opCtx);
        ASSERT(txnParticipant);

        txnParticipant.beginOrContinue(
            _opCtx, *_opCtx->getTxnNumber(), false /* autocommit */, true /* startTransaction */);
        txnParticipant.unstashTransactionResources(_opCtx, "insert");
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IX, LockMode::MODE_IX);
            insertDocument(autoColl.getCollection(), InsertStatement(doc));
        }
        txnParticipant.stashTransactionResources(_opCtx);

        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IS, LockMode::MODE_IS);
            auto coll = autoColl.getCollection();
            assertDocumentAtTimestamp(coll, presentTs, BSONObj());
            assertDocumentAtTimestamp(coll, beforeTxnTs, BSONObj());
            assertDocumentAtTimestamp(coll, commitEntryTs, BSONObj());
            assertDocumentAtTimestamp(coll, nullTs, BSONObj());

            const auto commitFilter = BSON("ts" << commitEntryTs);
            assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, nullTs, false);
        }
    }

    void logTimestamps() const {
        unittest::log() << "Present TS: " << presentTs;
        unittest::log() << "Before transaction TS: " << beforeTxnTs;
        unittest::log() << "Commit entry TS: " << commitEntryTs;
    }

    BSONObj getSessionTxnInfoAtTimestamp(const Timestamp& ts, bool expected) {
        AutoGetCollection autoColl(
            _opCtx, NamespaceString::kSessionTransactionsTableNamespace, LockMode::MODE_IX);
        const auto sessionId = *_opCtx->getLogicalSessionId();
        const auto txnNum = *_opCtx->getTxnNumber();
        BSONObj doc;
        OneOffRead oor(_opCtx, ts);
        bool found = Helpers::findOne(_opCtx,
                                      autoColl.getCollection(),
                                      BSON("_id" << sessionId.toBSON() << "txnNum" << txnNum),
                                      doc);
        if (expected) {
            ASSERT(found) << "Missing session transaction info at " << ts;
        } else {
            ASSERT_FALSE(found) << "Session transaction info at " << ts
                                << " is unexpectedly present " << doc;
        }
        return doc;
    }

protected:
    NamespaceString nss;
    Timestamp presentTs;
    Timestamp beforeTxnTs;
    Timestamp commitEntryTs;

    boost::optional<MongoDOperationContextSession> ocs;
};

class MultiDocumentTransaction : public MultiDocumentTransactionTest {
public:
    MultiDocumentTransaction() : MultiDocumentTransactionTest("multiDocumentTransaction") {}

    void run() {
        auto txnParticipant = TransactionParticipant::get(_opCtx);
        ASSERT(txnParticipant);
        logTimestamps();

        txnParticipant.unstashTransactionResources(_opCtx, "insert");

        txnParticipant.commitUnpreparedTransaction(_opCtx);

        txnParticipant.stashTransactionResources(_opCtx);
        assertNoStartOpTime();
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
            auto coll = autoColl.getCollection();
            assertDocumentAtTimestamp(coll, presentTs, BSONObj());
            assertDocumentAtTimestamp(coll, beforeTxnTs, BSONObj());
            assertDocumentAtTimestamp(coll, commitEntryTs, doc);
            assertDocumentAtTimestamp(coll, nullTs, doc);

            const auto commitFilter = BSON("ts" << commitEntryTs);
            assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(commitFilter, nullTs, true);
        }
    }
};

// Including this class in a test fixture forces transactions to use one oplog entry per operation
// instead of packing them into as few oplog entries as fit.  This allows testing of the timestamps
// of multi-oplog-entry transactions.
class MultiOplogScopedSettings {
public:
    MultiOplogScopedSettings()
        : _prevPackingLimit(gMaxNumberOfTransactionOperationsInSingleOplogEntry) {
        gMaxNumberOfTransactionOperationsInSingleOplogEntry = 1;
    }
    ~MultiOplogScopedSettings() {
        gMaxNumberOfTransactionOperationsInSingleOplogEntry = _prevPackingLimit;
    }

private:
    int _prevPackingLimit;
};

class MultiOplogEntryTransaction : public MultiDocumentTransactionTest {
public:
    MultiOplogEntryTransaction() : MultiDocumentTransactionTest("multiOplogEntryTransaction") {
        const auto currentTime = _clock->getClusterTime();
        firstOplogEntryTs = currentTime.addTicks(1).asTimestamp();
        commitEntryTs = currentTime.addTicks(2).asTimestamp();
    }

    void run() {
        auto txnParticipant = TransactionParticipant::get(_opCtx);
        ASSERT(txnParticipant);
        logTimestamps();

        txnParticipant.unstashTransactionResources(_opCtx, "insert");

        const BSONObj doc2 = BSON("_id" << 2 << "TestValue" << 2);
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IX, LockMode::MODE_IX);
            insertDocument(autoColl.getCollection(), InsertStatement(doc2));
        }
        txnParticipant.commitUnpreparedTransaction(_opCtx);

        txnParticipant.stashTransactionResources(_opCtx);
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
            const BSONObj query1 = BSON("_id" << 1);
            const BSONObj query2 = BSON("_id" << 2);
            auto coll = autoColl.getCollection();

            // Collection should be empty until commit, at which point both documents
            // should show up.
            assertDocumentAtTimestamp(coll, presentTs, BSONObj());
            assertDocumentAtTimestamp(coll, beforeTxnTs, BSONObj());
            assertDocumentAtTimestamp(coll, firstOplogEntryTs, BSONObj());
            assertFilteredDocumentAtTimestamp(coll, query1, commitEntryTs, doc);
            assertFilteredDocumentAtTimestamp(coll, query2, commitEntryTs, doc2);
            assertFilteredDocumentAtTimestamp(coll, query1, nullTs, doc);
            assertFilteredDocumentAtTimestamp(coll, query2, nullTs, doc2);

            // Implicit commit oplog entry should exist at commitEntryTs.
            const auto commitFilter =
                BSON("ts" << commitEntryTs << "o"
                          << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                                << "i"
                                                                << "ns"
                                                                << nss.ns()
                                                                << "ui"
                                                                << coll->uuid().get()
                                                                << "o"
                                                                << doc2))
                                             << "count"
                                             << 2));
            assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, firstOplogEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(commitFilter, nullTs, true);

            // Check that the oldestActiveTxnTimestamp properly accounts for in-progress
            // transactions.
            assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
            assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
            assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, firstOplogEntryTs);
            assertOldestActiveTxnTimestampEquals(boost::none, commitEntryTs);
            assertOldestActiveTxnTimestampEquals(boost::none, nullTs);

            // first oplog entry should exist at firstOplogEntryTs and after it.
            const auto firstOplogEntryFilter =
                BSON("ts" << firstOplogEntryTs << "o"
                          << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                                << "i"
                                                                << "ns"
                                                                << nss.ns()
                                                                << "ui"
                                                                << coll->uuid().get()
                                                                << "o"
                                                                << doc))
                                             << "partialTxn"
                                             << true));
            assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, firstOplogEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, commitEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, nullTs, true);

            // Session state should go to inProgress at firstOplogEntryTs, then to committed
            // at commitEntryTs
            getSessionTxnInfoAtTimestamp(presentTs, false);
            getSessionTxnInfoAtTimestamp(beforeTxnTs, false);
            auto sessionInfo = getSessionTxnInfoAtTimestamp(firstOplogEntryTs, true);
            ASSERT_EQ(sessionInfo["state"].String(), "inProgress");
            ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), firstOplogEntryTs);
            ASSERT_EQ(sessionInfo["startOpTime"]["ts"].timestamp(), firstOplogEntryTs);

            sessionInfo = getSessionTxnInfoAtTimestamp(commitEntryTs, true);
            ASSERT_EQ(sessionInfo["state"].String(), "committed");
            ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), commitEntryTs);
            ASSERT_FALSE(sessionInfo.hasField("startOpTime"));

            sessionInfo = getSessionTxnInfoAtTimestamp(nullTs, true);
            ASSERT_EQ(sessionInfo["state"].String(), "committed");
            ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), commitEntryTs);
            ASSERT_FALSE(sessionInfo.hasField("startOpTime"));
        }
    }

protected:
    Timestamp firstOplogEntryTs;

private:
    MultiOplogScopedSettings multiOplogSettings;
};

class CommitPreparedMultiOplogEntryTransaction : public MultiDocumentTransactionTest {
public:
    CommitPreparedMultiOplogEntryTransaction()
        : MultiDocumentTransactionTest("preparedMultiOplogEntryTransaction") {
        const auto currentTime = _clock->getClusterTime();
        firstOplogEntryTs = currentTime.addTicks(1).asTimestamp();
        prepareEntryTs = currentTime.addTicks(2).asTimestamp();
        commitEntryTs = currentTime.addTicks(3).asTimestamp();
    }

    void run() {
        auto txnParticipant = TransactionParticipant::get(_opCtx);
        ASSERT(txnParticipant);
        unittest::log() << "PrepareTS: " << prepareEntryTs;
        logTimestamps();

        const auto prepareFilter = BSON("ts" << prepareEntryTs);
        const auto commitFilter = BSON("ts" << commitEntryTs);
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IS, LockMode::MODE_IS);
            auto coll = autoColl.getCollection();
            assertDocumentAtTimestamp(coll, presentTs, BSONObj());
            assertDocumentAtTimestamp(coll, beforeTxnTs, BSONObj());
            assertDocumentAtTimestamp(coll, firstOplogEntryTs, BSONObj());
            assertDocumentAtTimestamp(coll, prepareEntryTs, BSONObj());
            assertDocumentAtTimestamp(coll, commitEntryTs, BSONObj());

            assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, firstOplogEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, commitEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, nullTs, false);

            assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, prepareEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, nullTs, false);

            assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
            assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
            assertOldestActiveTxnTimestampEquals(boost::none, prepareEntryTs);
            assertOldestActiveTxnTimestampEquals(boost::none, commitEntryTs);
            assertOldestActiveTxnTimestampEquals(boost::none, nullTs);
        }
        txnParticipant.unstashTransactionResources(_opCtx, "insert");
        const BSONObj doc2 = BSON("_id" << 2 << "TestValue" << 2);
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IX, LockMode::MODE_IX);
            insertDocument(autoColl.getCollection(), InsertStatement(doc2));
        }
        txnParticipant.prepareTransaction(_opCtx, {});

        const BSONObj query1 = BSON("_id" << 1);
        const BSONObj query2 = BSON("_id" << 2);

        txnParticipant.stashTransactionResources(_opCtx);
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IS, LockMode::MODE_IS);
            auto coll = autoColl.getCollection();
            assertDocumentAtTimestamp(coll, presentTs, BSONObj());
            assertDocumentAtTimestamp(coll, beforeTxnTs, BSONObj());
            assertDocumentAtTimestamp(coll, firstOplogEntryTs, BSONObj());

            {
                IgnorePrepareBlock ignorePrepare(_opCtx);
                // Perform the following while ignoring prepare conflicts. These calls would
                // otherwise wait forever until the prepared transaction committed or aborted.
                assertDocumentAtTimestamp(coll, prepareEntryTs, BSONObj());
                assertDocumentAtTimestamp(coll, commitEntryTs, BSONObj());
                assertDocumentAtTimestamp(coll, nullTs, BSONObj());
            }

            assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, firstOplogEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, commitEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, nullTs, true);

            // We haven't committed the prepared transaction
            assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, firstOplogEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, prepareEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, nullTs, false);

            assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
            assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
            assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, firstOplogEntryTs);
            assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, prepareEntryTs);
            // The transaction has not been committed yet and is still considered active.
            assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, commitEntryTs);
            assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, nullTs);
        }

        txnParticipant.unstashTransactionResources(_opCtx, "commitTransaction");

        {
            FailPointEnableBlock failPointBlock("skipCommitTxnCheckPrepareMajorityCommitted");
            txnParticipant.commitPreparedTransaction(_opCtx, commitEntryTs, {});
        }

        txnParticipant.stashTransactionResources(_opCtx);
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
            auto coll = autoColl.getCollection();
            assertDocumentAtTimestamp(coll, presentTs, BSONObj());
            assertDocumentAtTimestamp(coll, beforeTxnTs, BSONObj());
            assertDocumentAtTimestamp(coll, firstOplogEntryTs, BSONObj());
            assertDocumentAtTimestamp(coll, prepareEntryTs, BSONObj());
            assertFilteredDocumentAtTimestamp(coll, query1, commitEntryTs, doc);
            assertFilteredDocumentAtTimestamp(coll, query2, commitEntryTs, doc2);
            assertFilteredDocumentAtTimestamp(coll, query1, nullTs, doc);
            assertFilteredDocumentAtTimestamp(coll, query2, nullTs, doc2);

            // The prepare oplog entry should exist at prepareEntryTs and onwards.
            assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, commitEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, nullTs, true);

            // The commit oplog entry should exist at commitEntryTs and onwards.
            assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, prepareEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(commitFilter, nullTs, true);

            // The first oplog entry should exist at firstOplogEntryTs and onwards.
            const auto firstOplogEntryFilter =
                BSON("ts" << firstOplogEntryTs << "o"
                          << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                                << "i"
                                                                << "ns"
                                                                << nss.ns()
                                                                << "ui"
                                                                << coll->uuid().get()
                                                                << "o"
                                                                << doc))
                                             << "partialTxn"
                                             << true));
            assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, firstOplogEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, prepareEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, commitEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, nullTs, true);
            // The prepare oplog entry should exist at prepareEntryTs and onwards.
            const auto prepareOplogEntryFilter =
                BSON("ts" << prepareEntryTs << "o"
                          << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                                << "i"
                                                                << "ns"
                                                                << nss.ns()
                                                                << "ui"
                                                                << coll->uuid().get()
                                                                << "o"
                                                                << doc2))
                                             << "prepare"
                                             << true
                                             << "count"
                                             << 2));
            assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, firstOplogEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, prepareEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, commitEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, nullTs, true);

            assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
            assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
            assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, firstOplogEntryTs);
            assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, prepareEntryTs);
            // The transaction is no longer considered active after being committed.
            assertOldestActiveTxnTimestampEquals(boost::none, commitEntryTs);
            assertOldestActiveTxnTimestampEquals(boost::none, nullTs);

            // The session state should go to inProgress at firstOplogEntryTs, then to prepared at
            // prepareEntryTs, and then finally to committed at commitEntryTs.
            auto sessionInfo = getSessionTxnInfoAtTimestamp(firstOplogEntryTs, true);
            ASSERT_EQ(sessionInfo["state"].String(), "inProgress");
            ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), firstOplogEntryTs);
            ASSERT_EQ(sessionInfo["startOpTime"]["ts"].timestamp(), firstOplogEntryTs);

            sessionInfo = getSessionTxnInfoAtTimestamp(prepareEntryTs, true);
            ASSERT_EQ(sessionInfo["state"].String(), "prepared");
            ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), prepareEntryTs);
            ASSERT_EQ(sessionInfo["startOpTime"]["ts"].timestamp(), firstOplogEntryTs);

            sessionInfo = getSessionTxnInfoAtTimestamp(nullTs, true);
            ASSERT_EQ(sessionInfo["state"].String(), "committed");
            ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), commitEntryTs);
            ASSERT_FALSE(sessionInfo.hasField("startOpTime"));
        }
    }

protected:
    Timestamp firstOplogEntryTs, secondOplogEntryTs, prepareEntryTs;

private:
    MultiOplogScopedSettings multiOplogSettings;
};

class AbortPreparedMultiOplogEntryTransaction : public MultiDocumentTransactionTest {
public:
    AbortPreparedMultiOplogEntryTransaction()
        : MultiDocumentTransactionTest("preparedMultiOplogEntryTransaction") {
        const auto currentTime = _clock->getClusterTime();
        prepareEntryTs = currentTime.addTicks(1).asTimestamp();
        abortEntryTs = currentTime.addTicks(2).asTimestamp();
    }

    void run() {
        auto txnParticipant = TransactionParticipant::get(_opCtx);
        ASSERT(txnParticipant);
        unittest::log() << "PrepareTS: " << prepareEntryTs;
        unittest::log() << "AbortTS: " << abortEntryTs;

        const auto prepareFilter = BSON("ts" << prepareEntryTs);
        const auto abortFilter = BSON("ts" << abortEntryTs);
        {
            assertOplogDocumentExistsAtTimestamp(abortFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, prepareEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, abortEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, nullTs, false);
        }
        txnParticipant.unstashTransactionResources(_opCtx, "insert");

        txnParticipant.prepareTransaction(_opCtx, {});

        txnParticipant.stashTransactionResources(_opCtx);
        {
            assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, abortEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, nullTs, true);

            assertOplogDocumentExistsAtTimestamp(abortFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, firstOplogEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, prepareEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, abortEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, nullTs, false);
        }

        txnParticipant.unstashTransactionResources(_opCtx, "abortTransaction");

        txnParticipant.abortActiveTransaction(_opCtx);

        txnParticipant.stashTransactionResources(_opCtx);
        {
            // The prepare oplog entry should exist at prepareEntryTs and onwards.
            assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, abortEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, nullTs, true);

            // The abort oplog entry should exist at abortEntryTs and onwards.
            assertOplogDocumentExistsAtTimestamp(abortFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, prepareEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, abortEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(abortFilter, nullTs, true);

            UUID ui = UUID::gen();
            {
                AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
                auto coll = autoColl.getCollection();
                ASSERT(coll);
                ui = coll->uuid().get();
            }

            // The prepare oplog entry should exist at firstOplogEntryTs and onwards.
            const auto prepareOplogEntryFilter =
                BSON("ts" << prepareEntryTs << "o" << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                                                         << "i"
                                                                                         << "ns"
                                                                                         << nss.ns()
                                                                                         << "ui"
                                                                                         << ui
                                                                                         << "o"
                                                                                         << doc))
                                                                      << "prepare"
                                                                      << true));
            assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, prepareEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, abortEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, nullTs, true);

            assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
            assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
            assertOldestActiveTxnTimestampEquals(boost::none, abortEntryTs);
            assertOldestActiveTxnTimestampEquals(boost::none, nullTs);

            // The session state should be "aborted" at abortEntryTs.
            auto sessionInfo = getSessionTxnInfoAtTimestamp(abortEntryTs, true);
            ASSERT_EQ(sessionInfo["state"].String(), "aborted");
            ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), abortEntryTs);
            ASSERT_FALSE(sessionInfo.hasField("startOpTime"));
        }
    }

protected:
    Timestamp firstOplogEntryTs, secondOplogEntryTs, prepareEntryTs, abortEntryTs;

private:
    MultiOplogScopedSettings multiOplogSettings;
};

class PreparedMultiDocumentTransaction : public MultiDocumentTransactionTest {
public:
    PreparedMultiDocumentTransaction()
        : MultiDocumentTransactionTest("preparedMultiDocumentTransaction") {}

    void run() {
        auto txnParticipant = TransactionParticipant::get(_opCtx);
        ASSERT(txnParticipant);

        const auto currentTime = _clock->getClusterTime();
        const auto prepareTs = currentTime.addTicks(1).asTimestamp();
        commitEntryTs = currentTime.addTicks(2).asTimestamp();
        unittest::log() << "Prepare TS: " << prepareTs;
        logTimestamps();

        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IS, LockMode::MODE_IS);
            auto coll = autoColl.getCollection();
            assertDocumentAtTimestamp(coll, prepareTs, BSONObj());
            assertDocumentAtTimestamp(coll, commitEntryTs, BSONObj());

            const auto prepareFilter = BSON("ts" << prepareTs);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, commitEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, nullTs, false);

            const auto commitFilter = BSON("ts" << commitEntryTs);
            assertOplogDocumentExistsAtTimestamp(commitFilter, prepareTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, false);
        }
        txnParticipant.unstashTransactionResources(_opCtx, "insert");

        txnParticipant.prepareTransaction(_opCtx, {});

        txnParticipant.stashTransactionResources(_opCtx);
        assertHasStartOpTime();
        {
            const auto prepareFilter = BSON("ts" << prepareTs);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, commitEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, nullTs, true);

            const auto commitFilter = BSON("ts" << commitEntryTs);
            assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, prepareTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, nullTs, false);

            assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
            assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
            assertOldestActiveTxnTimestampEquals(prepareTs, prepareTs);
            assertOldestActiveTxnTimestampEquals(prepareTs, nullTs);
            assertOldestActiveTxnTimestampEquals(prepareTs, commitEntryTs);
        }
        txnParticipant.unstashTransactionResources(_opCtx, "commitTransaction");

        {
            FailPointEnableBlock failPointBlock("skipCommitTxnCheckPrepareMajorityCommitted");
            txnParticipant.commitPreparedTransaction(_opCtx, commitEntryTs, {});
        }

        assertNoStartOpTime();

        txnParticipant.stashTransactionResources(_opCtx);
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
            auto coll = autoColl.getCollection();
            assertDocumentAtTimestamp(coll, presentTs, BSONObj());
            assertDocumentAtTimestamp(coll, beforeTxnTs, BSONObj());
            assertDocumentAtTimestamp(coll, prepareTs, BSONObj());
            assertDocumentAtTimestamp(coll, commitEntryTs, doc);
            assertDocumentAtTimestamp(coll, nullTs, doc);

            const auto prepareFilter = BSON("ts" << prepareTs);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, commitEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, nullTs, true);

            const auto commitFilter = BSON("ts" << commitEntryTs);
            assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, prepareTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(commitFilter, nullTs, true);

            assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
            assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
            assertOldestActiveTxnTimestampEquals(prepareTs, prepareTs);
            assertOldestActiveTxnTimestampEquals(boost::none, commitEntryTs);
            assertOldestActiveTxnTimestampEquals(boost::none, nullTs);
        }
    }
};

class AbortedPreparedMultiDocumentTransaction : public MultiDocumentTransactionTest {
public:
    AbortedPreparedMultiDocumentTransaction()
        : MultiDocumentTransactionTest("abortedPreparedMultiDocumentTransaction") {}

    void run() {
        auto txnParticipant = TransactionParticipant::get(_opCtx);
        ASSERT(txnParticipant);

        const auto currentTime = _clock->getClusterTime();
        const auto prepareTs = currentTime.addTicks(1).asTimestamp();
        const auto abortEntryTs = currentTime.addTicks(2).asTimestamp();
        unittest::log() << "Prepare TS: " << prepareTs;
        logTimestamps();

        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IS, LockMode::MODE_IS);
            auto coll = autoColl.getCollection();
            assertDocumentAtTimestamp(coll, prepareTs, BSONObj());
            assertDocumentAtTimestamp(coll, abortEntryTs, BSONObj());

            const auto prepareFilter = BSON("ts" << prepareTs);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, abortEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, nullTs, false);

            const auto commitFilter = BSON("ts" << abortEntryTs);
            assertOplogDocumentExistsAtTimestamp(commitFilter, prepareTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, abortEntryTs, false);
        }
        txnParticipant.unstashTransactionResources(_opCtx, "insert");

        txnParticipant.prepareTransaction(_opCtx, {});

        txnParticipant.stashTransactionResources(_opCtx);
        assertHasStartOpTime();
        {
            const auto prepareFilter = BSON("ts" << prepareTs);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, abortEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, nullTs, true);

            const auto abortFilter = BSON("ts" << abortEntryTs);
            assertOplogDocumentExistsAtTimestamp(abortFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, prepareTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, abortEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, nullTs, false);

            assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
            assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
            assertOldestActiveTxnTimestampEquals(prepareTs, prepareTs);
            assertOldestActiveTxnTimestampEquals(prepareTs, nullTs);
            assertOldestActiveTxnTimestampEquals(prepareTs, abortEntryTs);
        }
        txnParticipant.unstashTransactionResources(_opCtx, "abortTransaction");

        txnParticipant.abortActiveTransaction(_opCtx);
        assertNoStartOpTime();

        txnParticipant.stashTransactionResources(_opCtx);
        {
            AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
            auto coll = autoColl.getCollection();
            assertDocumentAtTimestamp(coll, presentTs, BSONObj());
            assertDocumentAtTimestamp(coll, beforeTxnTs, BSONObj());
            assertDocumentAtTimestamp(coll, prepareTs, BSONObj());
            assertDocumentAtTimestamp(coll, abortEntryTs, BSONObj());
            assertDocumentAtTimestamp(coll, nullTs, BSONObj());

            const auto prepareFilter = BSON("ts" << prepareTs);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, abortEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(prepareFilter, nullTs, true);

            const auto abortFilter = BSON("ts" << abortEntryTs);
            assertOplogDocumentExistsAtTimestamp(abortFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, prepareTs, false);
            assertOplogDocumentExistsAtTimestamp(abortFilter, abortEntryTs, true);
            assertOplogDocumentExistsAtTimestamp(abortFilter, nullTs, true);

            assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
            assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
            assertOldestActiveTxnTimestampEquals(prepareTs, prepareTs);
            assertOldestActiveTxnTimestampEquals(boost::none, abortEntryTs);
            assertOldestActiveTxnTimestampEquals(boost::none, nullTs);
        }
    }
};

class AllStorageTimestampTests : public unittest::Suite {
public:
    AllStorageTimestampTests() : unittest::Suite("StorageTimestampTests") {}
    void setupTests() {
        // Only run on storage engines that support snapshot reads.
        auto storageEngine = cc().getServiceContext()->getStorageEngine();
        if (!storageEngine->supportsReadConcernSnapshot() ||
            !mongo::serverGlobalParams.enableMajorityReadConcern) {
            unittest::log() << "Skipping this test suite because storage engine "
                            << storageGlobalParams.engine << " does not support timestamp writes.";
            return;
        }

        add<SecondaryInsertTimes>();
        add<SecondaryArrayInsertTimes>();
        add<SecondaryDeleteTimes>();
        add<SecondaryUpdateTimes>();
        add<SecondaryInsertToUpsert>();
        add<SecondaryAtomicApplyOps>();
        add<SecondaryAtomicApplyOpsWCEToNonAtomic>();
        add<SecondaryCreateCollection>();
        add<SecondaryCreateTwoCollections>();
        add<SecondaryCreateCollectionBetweenInserts>();
        add<PrimaryCreateCollectionInApplyOps>();
        add<SecondarySetIndexMultikeyOnInsert>();
        add<InitialSyncSetIndexMultikeyOnInsert>();
        add<PrimarySetIndexMultikeyOnInsert>();
        add<PrimarySetIndexMultikeyOnInsertUnreplicated>();
        add<InitializeMinValid>();
        add<SetMinValidInitialSyncFlag>();
        add<SetMinValidToAtLeast>();
        add<SetMinValidAppliedThrough>();
        // KVDropDatabase<SimulatePrimary>
        add<KVDropDatabase<false>>();
        add<KVDropDatabase<true>>();
        // TimestampIndexBuilds<SimulatePrimary>
        add<TimestampIndexBuilds<false>>();
        add<TimestampIndexBuilds<true>>();
        // TODO (SERVER-40894): Make index builds timestamp drained writes
        // add<TimestampIndexBuildDrain<false>>();
        // add<TimestampIndexBuildDrain<true>>();
        add<TimestampMultiIndexBuilds>();
        add<TimestampMultiIndexBuildsDuringRename>();
        add<TimestampIndexDrops>();
        add<TimestampIndexBuilderOnPrimary>();
        add<SecondaryReadsDuringBatchApplicationAreAllowed>();
        add<ViewCreationSeparateTransaction>();
        add<CreateCollectionWithSystemIndex>();
        add<MultiDocumentTransaction>();
        add<MultiOplogEntryTransaction>();
        add<CommitPreparedMultiOplogEntryTransaction>();
        add<AbortPreparedMultiOplogEntryTransaction>();
        add<PreparedMultiDocumentTransaction>();
        add<AbortedPreparedMultiDocumentTransaction>();
    }
};

unittest::SuiteInstance<AllStorageTimestampTests> allStorageTimestampTests;
}  // namespace mongo
