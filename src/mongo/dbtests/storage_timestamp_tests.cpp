/**
 *    Copyright (C) 2017 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <cstdint>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
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
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
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
            _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNone);
        } else {
            _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, ts);
        }
    }

    ~OneOffRead() {
        _opCtx->recoveryUnit()->abandonSnapshot();
        _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNone);
    }

private:
    OperationContext* _opCtx;
};
}

const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

BSONCollectionCatalogEntry::IndexMetaData getIndexMetaData(
    const BSONCollectionCatalogEntry::MetaData& collMetaData, StringData indexName) {
    const auto idxOffset = collMetaData.findIndexOffset(indexName);
    invariant(idxOffset > -1);
    return collMetaData.indexes[idxOffset];
}

class DoNothingOplogApplierObserver : public repl::OplogApplier::Observer {
public:
    void onBatchBegin(const repl::OplogApplier::Operations&) final {}
    void onBatchEnd(const StatusWith<repl::OpTime>&, const repl::OplogApplier::Operations&) final {}
    void onMissingDocumentsFetchedAndInserted(const std::vector<FetchInfo>&) final {}
    void onOperationConsumed(const BSONObj&) final {}
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
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        repl::ReplSettings replSettings;
        replSettings.setOplogSizeBytes(10 * 1024 * 1024);
        replSettings.setReplSetString("rs0");
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
        registry->addObserver(stdx::make_unique<UUIDCatalogObserver>());
        registry->addObserver(stdx::make_unique<OpObserverImpl>());
        _opCtx->getServiceContext()->setOpObserver(std::move(registry));

        repl::setOplogCollectionName(getGlobalServiceContext());
        repl::createOplog(_opCtx);

        ASSERT_OK(_clock->advanceClusterTime(LogicalTime(Timestamp(1, 0))));

        ASSERT_EQUALS(presentTs, pastLt.addTicks(1).asTimestamp());
        setReplCoordAppliedOpTime(repl::OpTime(presentTs, presentTerm));
    }

    ~StorageTimestampTest() {
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
            _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNone);
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
            invariant(dbRaii.getDb()->createCollection(_opCtx, nss.ns()));
            wunit.commit();
        });
    }

    void insertDocument(Collection* coll, const InsertStatement& stmt) {
        // Insert some documents.
        OpDebug* const nullOpDebug = nullptr;
        const bool enforceQuota = false;
        const bool fromMigrate = false;
        ASSERT_OK(coll->insertDocument(_opCtx, stmt, nullOpDebug, enforceQuota, fromMigrate));
    }

    void createIndex(Collection* coll, std::string indexName, const BSONObj& indexKey) {

        // Build an index.
        MultiIndexBlock indexer(_opCtx, coll);
        BSONObj indexInfoObj;
        {
            auto swIndexInfoObj = indexer.init({BSON(
                "v" << 2 << "name" << indexName << "ns" << coll->ns().ns() << "key" << indexKey)});
            ASSERT_OK(swIndexInfoObj.getStatus());
            indexInfoObj = std::move(swIndexInfoObj.getValue()[0]);
        }

        ASSERT_OK(indexer.insertAllDocumentsInCollection());

        {
            WriteUnitOfWork wuow(_opCtx);
            // Timestamping index completion. Primaries write an oplog entry.
            indexer.commit();
            // The op observer is not called from the index builder, but rather the
            // `createIndexes` command.
            _opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                _opCtx, coll->ns(), coll->uuid(), indexInfoObj, false);
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
        return kvCatalog->getMetaData(_opCtx, ns.ns());
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

    BSONObj queryOplog(const BSONObj& query) {
        OneOffRead oor(_opCtx, Timestamp::min());
        BSONObj ret;
        ASSERT_TRUE(Helpers::findOne(
            _opCtx,
            AutoGetCollectionForRead(_opCtx, NamespaceString::kRsOplogNamespace).getCollection(),
            query,
            ret));
        return ret;
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
        ASSERT_EQ(expectedDoc.getOldOplogDeleteFromPoint(), doc.getOldOplogDeleteFromPoint())
            << "Old oplogDeleteFromPoint timestamps weren't equal at " << ts.toString()
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

    void setReplCoordAppliedOpTime(const repl::OpTime& opTime) {
        repl::ReplicationCoordinator::get(getGlobalServiceContext())
            ->setMyLastAppliedOpTime(opTime);
    }

    /**
     * Asserts that the given collection is in (or not in) the KVCatalog's list of idents at the
     * provided timestamp.
     */
    void assertNamespaceInIdents(NamespaceString nss, Timestamp ts, bool shouldExpect) {
        OneOffRead oor(_opCtx, ts);
        KVCatalog* kvCatalog =
            static_cast<KVStorageEngine*>(_opCtx->getServiceContext()->getStorageEngine())
                ->getCatalog();

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IS, LockMode::MODE_IS);

        // getCollectionIdent() returns the ident for the given namespace in the KVCatalog.
        // getAllIdents() actually looks in the RecordStore for a list of all idents, and is thus
        // versioned by timestamp. These tests do not do any renames, so we can expect the
        // namespace to have a consistent ident across timestamps, if it exists.
        auto expectedIdent = kvCatalog->getCollectionIdent(nss.ns());
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

    std::tuple<std::string, std::string> getNewCollectionIndexIdent(
        KVCatalog* kvCatalog, std::vector<std::string>& origIdents) {
        // Find the collection and index ident by performing a set difference on the original
        // idents and the current idents.
        std::vector<std::string> identsWithColl = kvCatalog->getAllIdents(_opCtx);
        std::sort(origIdents.begin(), origIdents.end());
        std::sort(identsWithColl.begin(), identsWithColl.end());
        std::vector<std::string> collAndIdxIdents;
        std::set_difference(identsWithColl.begin(),
                            identsWithColl.end(),
                            origIdents.begin(),
                            origIdents.end(),
                            std::back_inserter(collAndIdxIdents));

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
            ASSERT_FALSE(catalog->isIndexMultikey(opCtx, indexName, &actualMultikeyPaths));
        } else {
            ASSERT(catalog->isIndexMultikey(opCtx, indexName, &actualMultikeyPaths));
        }

        const bool match = (expectedMultikeyPaths == actualMultikeyPaths);
        if (!match) {
            FAIL(str::stream() << "Expected: " << dumpMultikeyPaths(expectedMultikeyPaths)
                               << ", Actual: "
                               << dumpMultikeyPaths(actualMultikeyPaths));
        }
        ASSERT_TRUE(match);
    }
};

class SecondaryInsertTimes : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
                                   << "h"
                                   << 0xBEEFBEEFLL
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
                                      << "h"
                                      << 1
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
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        // Create a new collection.
        NamespaceString nss("unittests.timestampedUpdates");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        const std::uint32_t docsToInsert = 10;
        const LogicalTime firstInsertTime = _clock->reserveTicks(docsToInsert);
        BSONObjBuilder fullCommand;
        BSONArrayBuilder applyOpsB(fullCommand.subarrayStart("applyOps"));

        BSONObjBuilder applyOpsElem1Builder;

        // Populate the "ts" field with an array of all the grouped inserts' timestamps.
        BSONArrayBuilder tsArrayBuilder(applyOpsElem1Builder.subarrayStart("ts"));
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            tsArrayBuilder.append(firstInsertTime.addTicks(idx).asTimestamp());
        }
        tsArrayBuilder.done();

        // Populate the "t" (term) field with an array of all the grouped inserts' terms.
        BSONArrayBuilder tArrayBuilder(applyOpsElem1Builder.subarrayStart("t"));
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            tArrayBuilder.append(1LL);
        }
        tArrayBuilder.done();

        // Populate the "o" field with an array of all the grouped inserts.
        BSONArrayBuilder oArrayBuilder(applyOpsElem1Builder.subarrayStart("o"));
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            oArrayBuilder.append(BSON("_id" << idx));
        }
        oArrayBuilder.done();

        applyOpsElem1Builder << "h" << 0xBEEFBEEFLL << "v" << 2 << "op"
                             << "i"
                             << "ns" << nss.ns() << "ui" << autoColl.getCollection()->uuid().get();

        applyOpsB.append(applyOpsElem1Builder.done());

        BSONObjBuilder applyOpsElem2Builder;
        applyOpsElem2Builder << "ts" << firstInsertTime.addTicks(docsToInsert).asTimestamp() << "t"
                             << 1LL << "h" << 1 << "op"
                             << "c"
                             << "ns"
                             << "test.$cmd"
                             << "o" << BSON("applyOps" << BSONArrayBuilder().obj());

        applyOpsB.append(applyOpsElem2Builder.done());
        applyOpsB.done();
        // Apply the group of inserts.
        BSONObjBuilder result;
        ASSERT_OK(applyOps(_opCtx,
                           nss.db().toString(),
                           fullCommand.done(),
                           repl::OplogApplication::Mode::kApplyOpsCmd,
                           &result));


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
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
                    {BSON("ts" << startDeleteTime.addTicks(num).asTimestamp() << "t" << 0LL << "h"
                               << 0xBEEFBEEFLL
                               << "v"
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
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
                    {BSON("ts" << firstUpdateTime.addTicks(idx).asTimestamp() << "t" << 0LL << "h"
                               << 0xBEEFBEEFLL
                               << "v"
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
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
        ASSERT_EQ(op.getNamespace().ns(), nss.getCommandNS().ns()) << op.toBSON();
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
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
        auto op0 = repl::OplogEntry(
            BSON("ts" << insertTime0.asTimestamp() << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2
                      << "op"
                      << "i"
                      << "ns"
                      << nss.ns()
                      << "ui"
                      << uuid
                      << "o"
                      << doc0));
        auto op1 = repl::OplogEntry(
            BSON("ts" << insertTime1.asTimestamp() << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2
                      << "op"
                      << "i"
                      << "ns"
                      << nss.ns()
                      << "ui"
                      << uuid
                      << "o"
                      << doc1));
        auto op2 = repl::OplogEntry(
            BSON("ts" << insertTime2.asTimestamp() << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2
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
        auto writerPool = repl::SyncTail::makeWriterPool();
        repl::OplogApplier oplogApplier(nullptr,
                                        nullptr,
                                        &observer,
                                        nullptr,
                                        _consistencyMarkers,
                                        storageInterface,
                                        {},
                                        writerPool.get());
        ASSERT_EQUALS(op2.getOpTime(), unittest::assertGet(oplogApplier.multiApply(_opCtx, ops)));

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
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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

        const LogicalTime pastTime = _clock->reserveTicks(1);
        const LogicalTime insertTime0 = _clock->reserveTicks(1);
        const LogicalTime indexBuildTime = _clock->reserveTicks(1);
        const LogicalTime insertTime1 = _clock->reserveTicks(1);
        const LogicalTime insertTime2 = _clock->reserveTicks(1);

        BSONObj doc0 = BSON("_id" << 0 << "a" << 3);
        BSONObj doc1 = BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2));
        BSONObj doc2 = BSON("_id" << 2 << "a" << BSON_ARRAY(1 << 2));
        auto op0 = repl::OplogEntry(
            BSON("ts" << insertTime0.asTimestamp() << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2
                      << "op"
                      << "i"
                      << "ns"
                      << nss.ns()
                      << "ui"
                      << uuid
                      << "o"
                      << doc0));
        auto op1 = repl::OplogEntry(
            BSON("ts" << insertTime1.asTimestamp() << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2
                      << "op"
                      << "i"
                      << "ns"
                      << nss.ns()
                      << "ui"
                      << uuid
                      << "o"
                      << doc1));
        auto op2 = repl::OplogEntry(
            BSON("ts" << insertTime2.asTimestamp() << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2
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
        auto createIndexOp = repl::OplogEntry(BSON(
            "ts" << indexBuildTime.asTimestamp() << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2
                 << "op"
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
        auto writerPool = repl::SyncTail::makeWriterPool();
        repl::OplogApplier::Options options;
        options.allowNamespaceNotFoundErrorsOnCrudOps = true;
        options.missingDocumentSourceForInitialSync = HostAndPort("localhost", 123);
        repl::OplogApplier oplogApplier(nullptr,
                                        nullptr,
                                        &observer,
                                        nullptr,
                                        _consistencyMarkers,
                                        storageInterface,
                                        options,
                                        writerPool.get());
        auto lastTime = unittest::assertGet(oplogApplier.multiApply(_opCtx, ops));
        ASSERT_EQ(lastTime.getTimestamp(), insertTime2.asTimestamp());

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

class PrimarySetIndexMultikeyOnInsert : public StorageTimestampTest {

public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        NamespaceString nss(repl::ReplicationConsistencyMarkersImpl::kDefaultMinValidNamespace);
        reset(nss);
        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);
        auto minValidColl = autoColl.getCollection();

        repl::ReplicationConsistencyMarkersImpl consistencyMarkers(
            repl::StorageInterface::get(_opCtx));
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
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
 * exercise the `KVStorageEngine::dropDatabase` method.
 */
template <bool SimulatePrimary>
class KVDropDatabase : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        auto storageInterface = repl::StorageInterface::get(_opCtx);
        repl::DropPendingCollectionReaper::set(
            _opCtx->getServiceContext(),
            stdx::make_unique<repl::DropPendingCollectionReaper>(storageInterface));

        auto kvStorageEngine =
            dynamic_cast<KVStorageEngine*>(_opCtx->getServiceContext()->getStorageEngine());
        KVCatalog* kvCatalog = kvStorageEngine->getCatalog();

        // Declare the database to be in a "synced" state, i.e: in steady-state replication.
        Timestamp syncTime = _clock->reserveTicks(1).asTimestamp();
        invariant(!syncTime.isNull());
        kvStorageEngine->setInitialDataTimestamp(syncTime);

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
            ASSERT_OK(db->dropCollection(_opCtx, nss.ns()));
            wuow.commit();
        }

        // Reserve a tick, this represents a time after the rename in which the `kvDropDatabase`
        // ident for `kvDropDatabase` still exists.
        const Timestamp postRenameTime = _clock->reserveTicks(1).asTimestamp();

        // The namespace has changed, but the ident still exists as-is after the rename.
        assertIdentsExistAtTimestamp(kvCatalog, collIdent, indexIdent, postRenameTime);

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
 * enforced via the parallel batch writer lock).
 */
template <bool SimulatePrimary>
class TimestampIndexBuilds : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        const bool SimulateSecondary = !SimulatePrimary;
        if (SimulateSecondary) {
            // The MemberState is inspected during index builds to use a "ghost" write to timestamp
            // index completion.
            ASSERT_OK(_coordinatorMock->setFollowerMode({repl::MemberState::MS::RS_SECONDARY}));
        }

        auto kvStorageEngine =
            dynamic_cast<KVStorageEngine*>(_opCtx->getServiceContext()->getStorageEngine());
        KVCatalog* kvCatalog = kvStorageEngine->getCatalog();

        NamespaceString nss("unittests.timestampIndexBuilds");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_X);

        const LogicalTime insertTimestamp = _clock->reserveTicks(1);
        {
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(autoColl.getCollection(),
                           InsertStatement(BSON("_id" << 0 << "a" << BSON_ARRAY(1 << 2)),
                                           insertTimestamp.asTimestamp(),
                                           0LL));
            wuow.commit();
            ASSERT_EQ(1, itCount(autoColl.getCollection()));
        }

        // Save the pre-state idents so we can capture the specific ident related to index
        // creation.
        std::vector<std::string> origIdents = kvCatalog->getAllIdents(_opCtx);

        // Build an index on `{a: 1}`. This index will be multikey.
        MultiIndexBlock indexer(_opCtx, autoColl.getCollection());
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

            auto swIndexInfoObj = indexer.init({BSON("v" << 2 << "unique" << true << "name"
                                                         << "a_1"
                                                         << "ns"
                                                         << nss.ns()
                                                         << "key"
                                                         << BSON("a" << 1))});
            ASSERT_OK(swIndexInfoObj.getStatus());
            indexInfoObj = std::move(swIndexInfoObj.getValue()[0]);
        }

        const LogicalTime afterIndexInit = _clock->reserveTicks(2);

        // Inserting all the documents has the side-effect of setting internal state on the index
        // builder that the index is multikey.
        ASSERT_OK(indexer.insertAllDocumentsInCollection());

        {
            WriteUnitOfWork wuow(_opCtx);
            // All callers of `MultiIndexBlock::commit` are responsible for timestamping index
            // completion.  Primaries write an oplog entry. Secondaries explicitly set a
            // timestamp.
            indexer.commit();
            if (SimulatePrimary) {
                // The op observer is not called from the index builder, but rather the
                // `createIndexes` command.
                _opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                    _opCtx, nss, autoColl.getCollection()->uuid(), indexInfoObj, false);
            } else {
                ASSERT_OK(
                    _opCtx->recoveryUnit()->setTimestamp(_clock->getClusterTime().asTimestamp()));
            }
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

class TimestampMultiIndexBuilds : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        auto kvStorageEngine =
            dynamic_cast<KVStorageEngine*>(_opCtx->getServiceContext()->getStorageEngine());
        KVCatalog* kvCatalog = kvStorageEngine->getCatalog();

        NamespaceString nss("unittests.timestampMultiIndexBuilds");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_X);

        const LogicalTime insertTimestamp = _clock->reserveTicks(1);
        {
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(autoColl.getCollection(),
                           InsertStatement(BSON("_id" << 0 << "a" << 1 << "b" << 2 << "c" << 3),
                                           insertTimestamp.asTimestamp(),
                                           0LL));
            wuow.commit();
            ASSERT_EQ(1, itCount(autoColl.getCollection()));
        }

        // Save the pre-state idents so we can capture the specific ident related to index
        // creation.
        std::vector<std::string> origIdents = kvCatalog->getAllIdents(_opCtx);

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

class TimestampIndexDrops : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }
        auto kvStorageEngine =
            dynamic_cast<KVStorageEngine*>(_opCtx->getServiceContext()->getStorageEngine());
        KVCatalog* kvCatalog = kvStorageEngine->getCatalog();

        NamespaceString nss("unittests.timestampIndexDrops");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_X);

        const LogicalTime insertTimestamp = _clock->reserveTicks(1);
        {
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(autoColl.getCollection(),
                           InsertStatement(BSON("_id" << 0 << "a" << 1 << "b" << 2 << "c" << 3),
                                           insertTimestamp.asTimestamp(),
                                           0LL));
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
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }
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

        auto joinGuard = MakeGuard([&] {
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
        auto insertOp = repl::OplogEntry(
            BSON("ts" << futureTs << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2 << "op"
                      << "i"
                      << "ns"
                      << ns.ns()
                      << "ui"
                      << uuid
                      << "o"
                      << doc0));

        // Apply the operation.
        auto storageInterface = repl::StorageInterface::get(_opCtx);
        auto writerPool = repl::SyncTail::makeWriterPool(1);
        repl::SyncTail syncTail(
            nullptr, _consistencyMarkers, storageInterface, applyOperationFn, writerPool.get());
        auto lastOpTime = unittest::assertGet(syncTail.multiApply(_opCtx, {insertOp}));
        ASSERT_EQ(insertOp.getOpTime(), lastOpTime);

        joinGuard.Dismiss();
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
 * This test will exercise IndexBuilder code on primaries by performing a background index build
 * via an `applyOps` command.
 */
template <bool Foreground>
class TimestampIndexBuilderOnPrimary : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date support timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

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
                           InsertStatement(doc, setupStart.asTimestamp(), 0LL));
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
            // Create a background index via `applyOps`. We will timestamp the beginning at
            // `startBuildTs` and the end, due to manipulation of the logical clock, should be
            // timestamped at `endBuildTs`.
            const auto beforeBuildTime = _clock->reserveTicks(3);
            const auto startBuildTs = beforeBuildTime.addTicks(1).asTimestamp();
            const auto endBuildTs = beforeBuildTime.addTicks(3).asTimestamp();

            // Grab the existing idents to identify the ident created by the index build.
            auto kvStorageEngine =
                dynamic_cast<KVStorageEngine*>(_opCtx->getServiceContext()->getStorageEngine());
            KVCatalog* kvCatalog = kvStorageEngine->getCatalog();
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
                                                  << "field_1"
                                                  << "background"
                                                  << (Foreground ? false : true));

            auto createIndexOp =
                BSON("ts" << startBuildTs << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2 << "op"
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
            if (Foreground) {
                // In the Foreground case, the index build should start and finish at
                // `startBuildTs`.
                ASSERT_TRUE(
                    getIndexMetaData(getMetaDataAtTime(kvCatalog, nss, startBuildTs), "field_1")
                        .ready);
            } else {
                // In the Background case, the index build should not be "ready" at `startBuildTs`.
                ASSERT_FALSE(
                    getIndexMetaData(getMetaDataAtTime(kvCatalog, nss, startBuildTs), "field_1")
                        .ready);
                assertIdentsExistAtTimestamp(kvCatalog, "", indexIdent, endBuildTs);
                ASSERT_TRUE(
                    getIndexMetaData(getMetaDataAtTime(kvCatalog, nss, endBuildTs), "field_1")
                        .ready);
            }
        }
    }
};


class AllStorageTimestampTests : public unittest::Suite {
public:
    AllStorageTimestampTests() : unittest::Suite("StorageTimestampTests") {}
    void setupTests() {
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
        add<TimestampMultiIndexBuilds>();
        add<TimestampIndexDrops>();
        // TimestampIndexBuilderOnPrimary<Background>
        add<TimestampIndexBuilderOnPrimary<false>>();
        add<TimestampIndexBuilderOnPrimary<true>>();
        add<SecondaryReadsDuringBatchApplicationAreAllowed>();
    }
};

unittest::SuiteInstance<AllStorageTimestampTests> allStorageTimestampTests;
}  // namespace mongo
