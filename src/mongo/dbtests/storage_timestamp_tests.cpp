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
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

class StorageTimestampTest {
public:
    ServiceContext::UniqueOperationContext _opCtxRaii = cc().makeOperationContext();
    OperationContext* _opCtx = _opCtxRaii.get();
    LogicalClock* _clock = LogicalClock::get(_opCtx);

    StorageTimestampTest() {
        if (!(mongo::storageGlobalParams.engine == "wiredTiger" &&
              mongo::serverGlobalParams.enableMajorityReadConcern)) {
            return;
        }

        repl::ReplSettings replSettings;
        replSettings.setOplogSizeBytes(10 * 1024 * 1024);
        replSettings.setReplSetString("rs0");
        auto coordinatorMock =
            new repl::ReplicationCoordinatorMock(_opCtx->getServiceContext(), replSettings);
        coordinatorMock->alwaysAllowWrites(true);
        setGlobalReplicationCoordinator(coordinatorMock);

        // Since the Client object persists across tests, even though the global
        // ReplicationCoordinator does not, we need to clear the last op associated with the client
        // to avoid the invariant in ReplClientInfo::setLastOp that the optime only goes forward.
        repl::ReplClientInfo::forClient(_opCtx->getClient()).clearLastOp_forTest();

        getGlobalServiceContext()->setOpObserver(stdx::make_unique<OpObserverImpl>());

        repl::setOplogCollectionName();
        repl::createOplog(_opCtx);

        ASSERT_OK(_clock->advanceClusterTime(LogicalTime(Timestamp(1, 0))));
    }

    ~StorageTimestampTest() {
        if (!(mongo::storageGlobalParams.engine == "wiredTiger" &&
              mongo::serverGlobalParams.enableMajorityReadConcern)) {
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
            invariant(_opCtx->recoveryUnit()->selectSnapshot(Timestamp::min()).isOK());
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

    StatusWith<BSONObj> doAtomicApplyOps(const std::string& dbName,
                                         const std::list<BSONObj>& applyOpsList) {
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
                                            const std::list<BSONObj>& applyOpsList,
                                            Timestamp dummyTs) {
        BSONArrayBuilder builder;
        builder.append(applyOpsList);
        builder << BSON("ts" << dummyTs << "t" << 1LL << "h" << 1 << "op"
                             << "c"
                             << "ns"
                             << "test.$cmd"
                             << "o"
                             << BSON("applyOps" << BSONArrayBuilder().obj()));
        BSONObjBuilder result;
        Status status = applyOps(_opCtx,
                                 dbName,
                                 BSON("applyOps" << builder.arr()),
                                 repl::OplogApplication::Mode::kApplyOpsCmd,
                                 &result);
        if (!status.isOK()) {
            return status;
        }

        return {result.obj()};
    }
};

class SecondaryInsertTimes : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (!(mongo::storageGlobalParams.engine == "wiredTiger" &&
              mongo::serverGlobalParams.enableMajorityReadConcern)) {
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
            auto recoveryUnit = _opCtx->recoveryUnit();
            recoveryUnit->abandonSnapshot();
            ASSERT_OK(recoveryUnit->selectSnapshot(firstInsertTime.addTicks(idx).asTimestamp()));
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
        if (!(mongo::storageGlobalParams.engine == "wiredTiger" &&
              mongo::serverGlobalParams.enableMajorityReadConcern)) {
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
            auto recoveryUnit = _opCtx->recoveryUnit();
            recoveryUnit->abandonSnapshot();
            ASSERT_OK(recoveryUnit->selectSnapshot(firstInsertTime.addTicks(idx).asTimestamp()));
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
        if (!(mongo::storageGlobalParams.engine == "wiredTiger" &&
              mongo::serverGlobalParams.enableMajorityReadConcern)) {
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
                               << BSON("_id" << num))},
                    startDeleteTime.addTicks(num).asTimestamp())
                    .getStatus());
        }

        for (std::int32_t num = 0; num <= docsToInsert; ++num) {
            // The first loop queries at `lastInsertTime` and should count all documents. Querying
            // at each successive tick counts one less document.
            auto recoveryUnit = _opCtx->recoveryUnit();
            recoveryUnit->abandonSnapshot();
            ASSERT_OK(recoveryUnit->selectSnapshot(lastInsertTime.addTicks(num).asTimestamp()));
            ASSERT_EQ(docsToInsert - num, itCount(autoColl.getCollection()));
        }
    }
};

class SecondaryUpdateTimes : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (!(mongo::storageGlobalParams.engine == "wiredTiger" &&
              mongo::serverGlobalParams.enableMajorityReadConcern)) {
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
                               << updates[idx].first)},
                    firstUpdateTime.addTicks(idx).asTimestamp())
                    .getStatus());
        }

        for (std::size_t idx = 0; idx < updates.size(); ++idx) {
            // Querying at each successive ticks after `insertTime` sees the document transform in
            // the series.
            auto recoveryUnit = _opCtx->recoveryUnit();
            recoveryUnit->abandonSnapshot();
            ASSERT_OK(recoveryUnit->selectSnapshot(insertTime.addTicks(idx + 1).asTimestamp()));

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
        if (!(mongo::storageGlobalParams.engine == "wiredTiger" &&
              mongo::serverGlobalParams.enableMajorityReadConcern)) {
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
        auto swResult = doNonAtomicApplyOps(
            nss.db().toString(),
            {BSON("ts" << insertTime.asTimestamp() << "t" << 1LL << "h" << 0xBEEFBEEFLL << "v" << 2
                       << "op"
                       << "i"
                       << "ns"
                       << nss.ns()
                       << "ui"
                       << autoColl.getCollection()->uuid().get()
                       << "o"
                       << BSON("_id" << 0 << "field" << 0)),
             BSON("ts" << insertTime.addTicks(1).asTimestamp() << "t" << 1LL << "h" << 0xBEEFBEEFLL
                       << "v"
                       << 2
                       << "op"
                       << "i"
                       << "ns"
                       << nss.ns()
                       << "ui"
                       << autoColl.getCollection()->uuid().get()
                       << "o"
                       << BSON("_id" << 0))},
            insertTime.addTicks(1).asTimestamp());
        ASSERT_OK(swResult);

        BSONObj& result = swResult.getValue();
        ASSERT_EQ(3, result.getIntField("applied"));
        ASSERT(result["results"].Array()[0].Bool());
        ASSERT(result["results"].Array()[1].Bool());
        ASSERT(result["results"].Array()[2].Bool());

        // Reading at `insertTime` should show the original document, `{_id: 0, field: 0}`.
        auto recoveryUnit = _opCtx->recoveryUnit();
        recoveryUnit->abandonSnapshot();
        ASSERT_OK(recoveryUnit->selectSnapshot(insertTime.asTimestamp()));
        auto doc = findOne(autoColl.getCollection());
        ASSERT_EQ(0,
                  SimpleBSONObjComparator::kInstance.compare(doc, BSON("_id" << 0 << "field" << 0)))
            << "Doc: " << doc.toString() << " Expected: {_id: 0, field: 0}";

        // Reading at `insertTime + 1` should show the second insert that got converted to an
        // upsert, `{_id: 0}`.
        recoveryUnit->abandonSnapshot();
        ASSERT_OK(recoveryUnit->selectSnapshot(insertTime.addTicks(1).asTimestamp()));
        doc = findOne(autoColl.getCollection());
        ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, BSON("_id" << 0)))
            << "Doc: " << doc.toString() << " Expected: {_id: 0}";
    }
};

class SecondaryAtomicApplyOps : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (!(mongo::storageGlobalParams.engine == "wiredTiger" &&
              mongo::serverGlobalParams.enableMajorityReadConcern)) {
            return;
        }

        // Create a new collection.
        NamespaceString nss("unittests.insertToUpsert");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        // Reserve a timestamp before the inserts should happen.
        const LogicalTime preInsertTimestamp = _clock->reserveTicks(1);
        auto swResult = doAtomicApplyOps(nss.db().toString(),
                                         {BSON("v" << 2 << "op"
                                                   << "i"
                                                   << "ns"
                                                   << nss.ns()
                                                   << "ui"
                                                   << autoColl.getCollection()->uuid().get()
                                                   << "o"
                                                   << BSON("_id" << 0)),
                                          BSON("v" << 2 << "op"
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
        ASSERT_OK(recoveryUnit->selectSnapshot(preInsertTimestamp.asTimestamp()));
        ASSERT_EQ(0, itCount(autoColl.getCollection()))
            << "Should not observe a write at `preInsertTimestamp`. TS: "
            << preInsertTimestamp.asTimestamp();

        // Reading at `preInsertTimestamp + 1` should observe both inserts.
        recoveryUnit = _opCtx->recoveryUnit();
        recoveryUnit->abandonSnapshot();
        ASSERT_OK(recoveryUnit->selectSnapshot(preInsertTimestamp.addTicks(1).asTimestamp()));
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
        if (!(mongo::storageGlobalParams.engine == "wiredTiger" &&
              mongo::serverGlobalParams.enableMajorityReadConcern)) {
            return;
        }

        // Create a new collectiont.
        NamespaceString nss("unitteTsts.insertToUpsert");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        const LogicalTime preInsertTimestamp = _clock->reserveTicks(1);
        auto swResult = doAtomicApplyOps(nss.db().toString(),
                                         {BSON("v" << 2 << "op"
                                                   << "i"
                                                   << "ns"
                                                   << nss.ns()
                                                   << "ui"
                                                   << autoColl.getCollection()->uuid().get()
                                                   << "o"
                                                   << BSON("_id" << 0 << "field" << 0)),
                                          BSON("v" << 2 << "op"
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
        ASSERT_OK(recoveryUnit->selectSnapshot(preInsertTimestamp.asTimestamp()));
        ASSERT_EQ(0, itCount(autoColl.getCollection()))
            << "Should not find any documents at `preInsertTimestamp`. TS: "
            << preInsertTimestamp.asTimestamp();

        // Reading at `preInsertTimestamp + 1` should show the final state of the document.
        recoveryUnit->abandonSnapshot();
        ASSERT_OK(recoveryUnit->selectSnapshot(preInsertTimestamp.addTicks(1).asTimestamp()));
        auto doc = findOne(autoColl.getCollection());
        ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, BSON("_id" << 0)))
            << "Doc: " << doc.toString() << " Expected: {_id: 0}";
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
    }
};

unittest::SuiteInstance<AllStorageTimestampTests> allStorageTimestampTests;
}  // namespace mongo
