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

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/oplog_writer_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

using std::set;
using std::unique_ptr;

/**
 * Unit tests related to DBHelpers
 */

static const char* const ns = "unittests.removetests";

// TODO: Normalize with test framework
/** Simple test for Helpers::RemoveRange. */
class RemoveRange {
public:
    RemoveRange() : _min(4), _max(8) {}

    void run() {}

private:
    BSONArray expected() const {
        BSONArrayBuilder bab;
        for (int i = 0; i < _min; ++i) {
            bab << BSON("_id" << i);
        }
        for (int i = _max; i < 10; ++i) {
            bab << BSON("_id" << i);
        }
        return bab.arr();
    }

    BSONArray docs(OperationContext* opCtx) const {
        DBDirectClient client(opCtx);
        FindCommandRequest findRequest{NamespaceString{ns}};
        findRequest.setHint(BSON("_id" << 1));
        std::unique_ptr<DBClientCursor> cursor = client.find(std::move(findRequest));
        BSONArrayBuilder bab;
        while (cursor->more()) {
            bab << cursor->next();
        }
        return bab.arr();
    }
    int _min;
    int _max;
};

class FindAndNoopUpdateTest {
public:
    void run() {
        auto serviceContext = getGlobalServiceContext();

        repl::ReplSettings replSettings;
        replSettings.setOplogSizeBytes(10 * 1024 * 1024);
        replSettings.setReplSetString("rs");
        setGlobalReplSettings(replSettings);
        auto coordinatorMock = new repl::ReplicationCoordinatorMock(serviceContext, replSettings);
        _coordinatorMock = coordinatorMock;
        coordinatorMock->alwaysAllowWrites(true);
        repl::ReplicationCoordinator::set(
            serviceContext, std::unique_ptr<repl::ReplicationCoordinator>(coordinatorMock));

        NamespaceString nss("test.findandnoopupdate");

        auto client1 = serviceContext->makeClient("client1");
        auto opCtx1 = client1->makeOperationContext();

        auto client2 = serviceContext->makeClient("client2");
        auto opCtx2 = client2->makeOperationContext();

        auto registry = std::make_unique<OpObserverRegistry>();
        registry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OplogWriterImpl>()));
        opCtx1.get()->getServiceContext()->setOpObserver(std::move(registry));
        repl::createOplog(opCtx1.get());

        Lock::DBLock dbLk1(opCtx1.get(), nss.dbName(), LockMode::MODE_IX);
        Lock::CollectionLock collLk1(opCtx1.get(), nss, LockMode::MODE_IX);

        Lock::DBLock dbLk2(opCtx2.get(), nss.dbName(), LockMode::MODE_IX);
        Lock::CollectionLock collLk2(opCtx2.get(), nss, LockMode::MODE_IX);

        Database* db =
            DatabaseHolder::get(opCtx1.get())->openDb(opCtx1.get(), nss.dbName(), nullptr);

        // Create the collection and insert one doc
        BSONObj doc = BSON("_id" << 1 << "x" << 2);
        BSONObj idQuery = BSON("_id" << 1);

        CollectionPtr collection1;
        {
            WriteUnitOfWork wuow(opCtx1.get());
            collection1 = db->createCollection(opCtx1.get(), nss, CollectionOptions(), true);
            ASSERT_TRUE(collection1 != nullptr);
            ASSERT_TRUE(collection1
                            ->insertDocument(
                                opCtx1.get(), InsertStatement(doc), nullptr /* opDebug */, false)
                            .isOK());
            wuow.commit();
        }

        BSONObj result;
        Helpers::findById(opCtx1.get(), nss.ns(), idQuery, result, nullptr, nullptr);
        ASSERT_BSONOBJ_EQ(result, doc);

        // Assert that the same doc still exists after findByIdAndNoopUpdate
        {
            WriteUnitOfWork wuow(opCtx1.get());
            BSONObj res;
            auto lastApplied = repl::ReplicationCoordinator::get(opCtx1->getServiceContext())
                                   ->getMyLastAppliedOpTime()
                                   .getTimestamp();
            ASSERT_OK(opCtx1->recoveryUnit()->setTimestamp(lastApplied + 1));
            auto foundDoc = Helpers::findByIdAndNoopUpdate(opCtx1.get(), collection1, idQuery, res);
            wuow.commit();
            ASSERT_TRUE(foundDoc);
            ASSERT_BSONOBJ_EQ(res, doc);
        }

        // Assert that findByIdAndNoopUpdate did not generate an oplog entry.
        BSONObj oplogEntry;
        Helpers::getLast(opCtx1.get(), NamespaceString::kRsOplogNamespace, oplogEntry);
        ASSERT_BSONOBJ_NE(oplogEntry, BSONObj());
        ASSERT_TRUE(oplogEntry.getStringField("op") == "i"_sd);

        // Run two concurrent storage transactions. Run findByIdAndNoopUpdate in one, and then
        // attempt to delete all docs in the collection in the other. Assert that the delete op
        // throws a WCE.
        assertWriteAttemptAfterFindAndNoopUpdateThrowsWCE(
            opCtx1.get(), opCtx2.get(), nss, db, doc, idQuery);

        // Run two concurrent storage transactions. Run a delete op to remove all documents in the
        // collection in one, and then attempt to run findByIdAndNoopUpdate in the second. Assert
        // that findByIdAndNoopUpdate throws WCE.
        assertFindAndNoopUpdateAfterWriteThrowsWCE(
            opCtx1.get(), opCtx2.get(), nss, db, doc, idQuery);
    }

private:
    void assertWriteAttemptAfterFindAndNoopUpdateThrowsWCE(OperationContext* opCtx1,
                                                           OperationContext* opCtx2,
                                                           const NamespaceString& nss,
                                                           Database* db,
                                                           const BSONObj& doc,
                                                           const BSONObj& idQuery) {
        {
            WriteUnitOfWork wuow1(opCtx1);

            WriteUnitOfWork wuow2(opCtx2);
            auto collection2 =
                CollectionCatalog::get(opCtx2)->lookupCollectionByNamespace(opCtx2, nss);
            ASSERT(collection2 != nullptr);
            auto lastApplied = repl::ReplicationCoordinator::get(opCtx2->getServiceContext())
                                   ->getMyLastAppliedOpTime()
                                   .getTimestamp();
            ASSERT_OK(opCtx2->recoveryUnit()->setTimestamp(lastApplied + 1));
            BSONObj res;
            ASSERT_TRUE(Helpers::findByIdAndNoopUpdate(opCtx2, collection2, idQuery, res));

            ASSERT_THROWS(Helpers::emptyCollection(opCtx1, nss), WriteConflictException);

            wuow2.commit();
        }

        // Assert that the doc still exists in the collection.
        BSONObj res1;
        Helpers::findById(opCtx1, nss.ns(), idQuery, res1, nullptr, nullptr);
        ASSERT_BSONOBJ_EQ(res1, doc);

        BSONObj res2;
        Helpers::findById(opCtx2, nss.ns(), idQuery, res2, nullptr, nullptr);
        ASSERT_BSONOBJ_EQ(res2, doc);

        // Assert that findByIdAndNoopUpdate did not generate an oplog entry.
        BSONObj oplogEntry;
        Helpers::getLast(opCtx2, NamespaceString::kRsOplogNamespace, oplogEntry);
        ASSERT_BSONOBJ_NE(oplogEntry, BSONObj());
        ASSERT_TRUE(oplogEntry.getStringField("op") == "i"_sd);
    }

    void assertFindAndNoopUpdateAfterWriteThrowsWCE(OperationContext* opCtx1,
                                                    OperationContext* opCtx2,
                                                    const NamespaceString& nss,
                                                    Database* db,
                                                    const BSONObj& doc,
                                                    const BSONObj& idQuery) {
        {
            WriteUnitOfWork wuow1(opCtx1);
            auto lastApplied = repl::ReplicationCoordinator::get(opCtx1->getServiceContext())
                                   ->getMyLastAppliedOpTime()
                                   .getTimestamp();
            ASSERT_OK(opCtx1->recoveryUnit()->setTimestamp(lastApplied + 1));
            Helpers::emptyCollection(opCtx1, nss);

            {
                WriteUnitOfWork wuow2(opCtx2);
                auto collection2 =
                    CollectionCatalog::get(opCtx2)->lookupCollectionByNamespace(opCtx2, nss);
                ASSERT(collection2 != nullptr);

                BSONObj res;
                ASSERT_THROWS(Helpers::findByIdAndNoopUpdate(opCtx2, collection2, idQuery, res),
                              WriteConflictException);
            }

            wuow1.commit();
        }

        // Assert that the first storage transaction succeeded and that the doc is removed.
        BSONObj res1;
        Helpers::findById(opCtx1, nss.ns(), idQuery, res1, nullptr, nullptr);
        ASSERT_BSONOBJ_EQ(res1, BSONObj());

        BSONObj res2;
        Helpers::findById(opCtx2, nss.ns(), idQuery, res2, nullptr, nullptr);
        ASSERT_BSONOBJ_EQ(res2, BSONObj());
    }

    repl::ReplicationCoordinatorMock* _coordinatorMock;
};

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("dbhelpers") {}
    void setupTests() {
        add<RemoveRange>();
        add<FindAndNoopUpdateTest>();
    }
};

OldStyleSuiteInitializer<All> myall;

}  // namespace
}  // namespace mongo
