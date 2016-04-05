/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/head_manager.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/record_id.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"

using std::unique_ptr;
using std::list;
using std::string;

namespace RollbackTests {

namespace {
void dropDatabase(OperationContext* txn, const NamespaceString& nss) {
    ScopedTransaction transaction(txn, MODE_X);
    Lock::GlobalWrite globalWriteLock(txn->lockState());
    Database* db = dbHolder().get(txn, nss.db());

    if (db) {
        Database::dropDatabase(txn, db);
    }
}
bool collectionExists(OldClientContext* ctx, const string& ns) {
    const DatabaseCatalogEntry* dbEntry = ctx->db()->getDatabaseCatalogEntry();
    list<string> names;
    dbEntry->getCollectionNamespaces(&names);
    return std::find(names.begin(), names.end(), ns) != names.end();
}
void createCollection(OperationContext* txn, const NamespaceString& nss) {
    ScopedTransaction transaction(txn, MODE_IX);
    Lock::DBLock dbXLock(txn->lockState(), nss.db(), MODE_X);
    OldClientContext ctx(txn, nss.ns());
    {
        WriteUnitOfWork uow(txn);
        ASSERT(!collectionExists(&ctx, nss.ns()));
        ASSERT_OK(userCreateNS(txn, ctx.db(), nss.ns(), BSONObj(), false));
        ASSERT(collectionExists(&ctx, nss.ns()));
        uow.commit();
    }
}
Status renameCollection(OperationContext* txn,
                        const NamespaceString& source,
                        const NamespaceString& target) {
    ASSERT_EQ(source.db(), target.db());
    Database* db = dbHolder().get(txn, source.db());
    return db->renameCollection(txn, source.ns(), target.ns(), false);
}
Status truncateCollection(OperationContext* txn, const NamespaceString& nss) {
    Collection* coll = dbHolder().get(txn, nss.db())->getCollection(nss.ns());
    return coll->truncate(txn);
}

void insertRecord(OperationContext* txn, const NamespaceString& nss, const BSONObj& data) {
    Collection* coll = dbHolder().get(txn, nss.db())->getCollection(nss.ns());
    OpDebug* const nullOpDebug = nullptr;
    ASSERT_OK(coll->insertDocument(txn, data, nullOpDebug, false));
}
void assertOnlyRecord(OperationContext* txn, const NamespaceString& nss, const BSONObj& data) {
    Collection* coll = dbHolder().get(txn, nss.db())->getCollection(nss.ns());
    auto cursor = coll->getCursor(txn);

    auto record = cursor->next();
    ASSERT(record);
    ASSERT_EQ(data, record->data.releaseToBson());

    ASSERT(!cursor->next());
}
void assertEmpty(OperationContext* txn, const NamespaceString& nss) {
    Collection* coll = dbHolder().get(txn, nss.db())->getCollection(nss.ns());
    ASSERT(!coll->getCursor(txn)->next());
}
bool indexExists(OperationContext* txn, const NamespaceString& nss, const string& idxName) {
    Collection* coll = dbHolder().get(txn, nss.db())->getCollection(nss.ns());
    return coll->getIndexCatalog()->findIndexByName(txn, idxName, true) != NULL;
}
bool indexReady(OperationContext* txn, const NamespaceString& nss, const string& idxName) {
    Collection* coll = dbHolder().get(txn, nss.db())->getCollection(nss.ns());
    return coll->getIndexCatalog()->findIndexByName(txn, idxName, false) != NULL;
}
size_t getNumIndexEntries(OperationContext* txn,
                          const NamespaceString& nss,
                          const string& idxName) {
    size_t numEntries = 0;

    Collection* coll = dbHolder().get(txn, nss.db())->getCollection(nss.ns());
    IndexCatalog* catalog = coll->getIndexCatalog();
    IndexDescriptor* desc = catalog->findIndexByName(txn, idxName, false);

    if (desc) {
        auto cursor = catalog->getIndex(desc)->newCursor(txn);

        for (auto kv = cursor->seek(kMinBSONKey, true); kv; kv = cursor->next()) {
            numEntries++;
        }
    }

    return numEntries;
}

void dropIndex(OperationContext* txn, const NamespaceString& nss, const string& idxName) {
    Collection* coll = dbHolder().get(txn, nss.db())->getCollection(nss.ns());
    IndexDescriptor* desc = coll->getIndexCatalog()->findIndexByName(txn, idxName);
    ASSERT(desc);
    ASSERT_OK(coll->getIndexCatalog()->dropIndex(txn, desc));
}
}  // namespace

template <bool rollback, bool defaultIndexes>
class CreateCollection {
public:
    void run() {
        string ns = "unittests.rollback_create_collection";
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        NamespaceString nss(ns);
        dropDatabase(&txn, nss);

        ScopedTransaction transaction(&txn, MODE_IX);
        Lock::DBLock dbXLock(txn.lockState(), nss.db(), MODE_X);
        OldClientContext ctx(&txn, ns);
        {
            WriteUnitOfWork uow(&txn);
            ASSERT(!collectionExists(&ctx, ns));
            ASSERT_OK(userCreateNS(&txn, ctx.db(), ns, BSONObj(), defaultIndexes));
            ASSERT(collectionExists(&ctx, ns));
            if (!rollback) {
                uow.commit();
            }
        }
        if (rollback) {
            ASSERT(!collectionExists(&ctx, ns));
        } else {
            ASSERT(collectionExists(&ctx, ns));
        }
    }
};

template <bool rollback, bool defaultIndexes>
class DropCollection {
public:
    void run() {
        string ns = "unittests.rollback_drop_collection";
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        NamespaceString nss(ns);
        dropDatabase(&txn, nss);

        ScopedTransaction transaction(&txn, MODE_IX);
        Lock::DBLock dbXLock(txn.lockState(), nss.db(), MODE_X);
        OldClientContext ctx(&txn, ns);
        {
            WriteUnitOfWork uow(&txn);
            ASSERT(!collectionExists(&ctx, ns));
            ASSERT_OK(userCreateNS(&txn, ctx.db(), ns, BSONObj(), defaultIndexes));
            uow.commit();
        }
        ASSERT(collectionExists(&ctx, ns));

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&txn);
            ASSERT(collectionExists(&ctx, ns));
            ASSERT_OK(ctx.db()->dropCollection(&txn, ns));
            ASSERT(!collectionExists(&ctx, ns));
            if (!rollback) {
                uow.commit();
            }
        }
        if (rollback) {
            ASSERT(collectionExists(&ctx, ns));
        } else {
            ASSERT(!collectionExists(&ctx, ns));
        }
    }
};

template <bool rollback, bool defaultIndexes>
class RenameCollection {
public:
    void run() {
        NamespaceString source("unittests.rollback_rename_collection_src");
        NamespaceString target("unittests.rollback_rename_collection_dest");
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;

        dropDatabase(&txn, source);
        dropDatabase(&txn, target);

        ScopedTransaction transaction(&txn, MODE_X);
        Lock::GlobalWrite globalWriteLock(txn.lockState());
        OldClientContext ctx(&txn, source.ns());

        {
            WriteUnitOfWork uow(&txn);
            ASSERT(!collectionExists(&ctx, source.ns()));
            ASSERT(!collectionExists(&ctx, target.ns()));
            ASSERT_OK(userCreateNS(&txn, ctx.db(), source.ns(), BSONObj(), defaultIndexes));
            uow.commit();
        }
        ASSERT(collectionExists(&ctx, source.ns()));
        ASSERT(!collectionExists(&ctx, target.ns()));

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&txn);
            ASSERT_OK(renameCollection(&txn, source, target));
            ASSERT(!collectionExists(&ctx, source.ns()));
            ASSERT(collectionExists(&ctx, target.ns()));
            if (!rollback) {
                uow.commit();
            }
        }
        if (rollback) {
            ASSERT(collectionExists(&ctx, source.ns()));
            ASSERT(!collectionExists(&ctx, target.ns()));
        } else {
            ASSERT(!collectionExists(&ctx, source.ns()));
            ASSERT(collectionExists(&ctx, target.ns()));
        }
    }
};

template <bool rollback, bool defaultIndexes>
class RenameDropTargetCollection {
public:
    void run() {
        NamespaceString source("unittests.rollback_rename_droptarget_collection_src");
        NamespaceString target("unittests.rollback_rename_droptarget_collection_dest");
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;

        dropDatabase(&txn, source);
        dropDatabase(&txn, target);

        ScopedTransaction transaction(&txn, MODE_X);
        Lock::GlobalWrite globalWriteLock(txn.lockState());
        OldClientContext ctx(&txn, source.ns());

        BSONObj sourceDoc = BSON("_id"
                                 << "source");
        BSONObj targetDoc = BSON("_id"
                                 << "target");

        {
            WriteUnitOfWork uow(&txn);
            ASSERT(!collectionExists(&ctx, source.ns()));
            ASSERT(!collectionExists(&ctx, target.ns()));
            ASSERT_OK(userCreateNS(&txn, ctx.db(), source.ns(), BSONObj(), defaultIndexes));
            ASSERT_OK(userCreateNS(&txn, ctx.db(), target.ns(), BSONObj(), defaultIndexes));

            insertRecord(&txn, source, sourceDoc);
            insertRecord(&txn, target, targetDoc);

            uow.commit();
        }
        ASSERT(collectionExists(&ctx, source.ns()));
        ASSERT(collectionExists(&ctx, target.ns()));
        assertOnlyRecord(&txn, source, sourceDoc);
        assertOnlyRecord(&txn, target, targetDoc);

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&txn);
            ASSERT_OK(ctx.db()->dropCollection(&txn, target.ns()));
            ASSERT_OK(renameCollection(&txn, source, target));
            ASSERT(!collectionExists(&ctx, source.ns()));
            ASSERT(collectionExists(&ctx, target.ns()));
            assertOnlyRecord(&txn, target, sourceDoc);
            if (!rollback) {
                uow.commit();
            }
        }
        if (rollback) {
            ASSERT(collectionExists(&ctx, source.ns()));
            ASSERT(collectionExists(&ctx, target.ns()));
            assertOnlyRecord(&txn, source, sourceDoc);
            assertOnlyRecord(&txn, target, targetDoc);
        } else {
            ASSERT(!collectionExists(&ctx, source.ns()));
            ASSERT(collectionExists(&ctx, target.ns()));
            assertOnlyRecord(&txn, target, sourceDoc);
        }
    }
};

template <bool rollback, bool defaultIndexes>
class ReplaceCollection {
public:
    void run() {
        NamespaceString nss("unittests.rollback_replace_collection");
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        dropDatabase(&txn, nss);

        ScopedTransaction transaction(&txn, MODE_IX);
        Lock::DBLock dbXLock(txn.lockState(), nss.db(), MODE_X);
        OldClientContext ctx(&txn, nss.ns());

        BSONObj oldDoc = BSON("_id"
                              << "old");
        BSONObj newDoc = BSON("_id"
                              << "new");

        {
            WriteUnitOfWork uow(&txn);
            ASSERT(!collectionExists(&ctx, nss.ns()));
            ASSERT_OK(userCreateNS(&txn, ctx.db(), nss.ns(), BSONObj(), defaultIndexes));
            insertRecord(&txn, nss, oldDoc);
            uow.commit();
        }
        ASSERT(collectionExists(&ctx, nss.ns()));
        assertOnlyRecord(&txn, nss, oldDoc);

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&txn);
            ASSERT_OK(ctx.db()->dropCollection(&txn, nss.ns()));
            ASSERT(!collectionExists(&ctx, nss.ns()));
            ASSERT_OK(userCreateNS(&txn, ctx.db(), nss.ns(), BSONObj(), defaultIndexes));
            ASSERT(collectionExists(&ctx, nss.ns()));
            insertRecord(&txn, nss, newDoc);
            assertOnlyRecord(&txn, nss, newDoc);
            if (!rollback) {
                uow.commit();
            }
        }
        ASSERT(collectionExists(&ctx, nss.ns()));
        if (rollback) {
            assertOnlyRecord(&txn, nss, oldDoc);
        } else {
            assertOnlyRecord(&txn, nss, newDoc);
        }
    }
};

template <bool rollback, bool defaultIndexes>
class CreateDropCollection {
public:
    void run() {
        NamespaceString nss("unittests.rollback_create_drop_collection");
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        dropDatabase(&txn, nss);

        ScopedTransaction transaction(&txn, MODE_IX);
        Lock::DBLock dbXLock(txn.lockState(), nss.db(), MODE_X);
        OldClientContext ctx(&txn, nss.ns());

        BSONObj doc = BSON("_id"
                           << "example string");

        ASSERT(!collectionExists(&ctx, nss.ns()));
        {
            WriteUnitOfWork uow(&txn);

            ASSERT_OK(userCreateNS(&txn, ctx.db(), nss.ns(), BSONObj(), defaultIndexes));
            ASSERT(collectionExists(&ctx, nss.ns()));
            insertRecord(&txn, nss, doc);
            assertOnlyRecord(&txn, nss, doc);

            ASSERT_OK(ctx.db()->dropCollection(&txn, nss.ns()));
            ASSERT(!collectionExists(&ctx, nss.ns()));

            if (!rollback) {
                uow.commit();
            }
        }
        ASSERT(!collectionExists(&ctx, nss.ns()));
    }
};

template <bool rollback, bool defaultIndexes>
class TruncateCollection {
public:
    void run() {
        NamespaceString nss("unittests.rollback_truncate_collection");
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        dropDatabase(&txn, nss);

        ScopedTransaction transaction(&txn, MODE_IX);
        Lock::DBLock dbXLock(txn.lockState(), nss.db(), MODE_X);
        OldClientContext ctx(&txn, nss.ns());

        BSONObj doc = BSON("_id"
                           << "foo");

        ASSERT(!collectionExists(&ctx, nss.ns()));
        {
            WriteUnitOfWork uow(&txn);

            ASSERT_OK(userCreateNS(&txn, ctx.db(), nss.ns(), BSONObj(), defaultIndexes));
            ASSERT(collectionExists(&ctx, nss.ns()));
            insertRecord(&txn, nss, doc);
            assertOnlyRecord(&txn, nss, doc);
            uow.commit();
        }
        assertOnlyRecord(&txn, nss, doc);

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&txn);

            ASSERT_OK(truncateCollection(&txn, nss));
            ASSERT(collectionExists(&ctx, nss.ns()));
            assertEmpty(&txn, nss);

            if (!rollback) {
                uow.commit();
            }
        }
        ASSERT(collectionExists(&ctx, nss.ns()));
        if (rollback) {
            assertOnlyRecord(&txn, nss, doc);
        } else {
            assertEmpty(&txn, nss);
        }
    }
};

template <bool rollback>
class CreateIndex {
public:
    void run() {
        string ns = "unittests.rollback_create_index";
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        NamespaceString nss(ns);
        dropDatabase(&txn, nss);
        createCollection(&txn, nss);

        ScopedTransaction transaction(&txn, MODE_IX);
        AutoGetDb autoDb(&txn, nss.db(), MODE_X);

        Collection* coll = autoDb.getDb()->getCollection(ns);
        IndexCatalog* catalog = coll->getIndexCatalog();

        string idxName = "a";
        BSONObj spec = BSON("ns" << ns << "key" << BSON("a" << 1) << "name" << idxName);

        // END SETUP / START TEST

        {
            WriteUnitOfWork uow(&txn);
            ASSERT_OK(catalog->createIndexOnEmptyCollection(&txn, spec));
            insertRecord(&txn, nss, BSON("a" << 1));
            insertRecord(&txn, nss, BSON("a" << 2));
            insertRecord(&txn, nss, BSON("a" << 3));
            if (!rollback) {
                uow.commit();
            }
        }

        if (rollback) {
            ASSERT(!indexExists(&txn, nss, idxName));
        } else {
            ASSERT(indexReady(&txn, nss, idxName));
        }
    }
};

template <bool rollback>
class DropIndex {
public:
    void run() {
        string ns = "unittests.rollback_drop_index";
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        NamespaceString nss(ns);
        dropDatabase(&txn, nss);
        createCollection(&txn, nss);

        ScopedTransaction transaction(&txn, MODE_IX);
        AutoGetDb autoDb(&txn, nss.db(), MODE_X);

        Collection* coll = autoDb.getDb()->getCollection(ns);
        IndexCatalog* catalog = coll->getIndexCatalog();

        string idxName = "a";
        BSONObj spec = BSON("ns" << ns << "key" << BSON("a" << 1) << "name" << idxName);

        {
            WriteUnitOfWork uow(&txn);
            ASSERT_OK(catalog->createIndexOnEmptyCollection(&txn, spec));
            insertRecord(&txn, nss, BSON("a" << 1));
            insertRecord(&txn, nss, BSON("a" << 2));
            insertRecord(&txn, nss, BSON("a" << 3));
            uow.commit();
        }
        ASSERT(indexReady(&txn, nss, idxName));
        ASSERT_EQ(3u, getNumIndexEntries(&txn, nss, idxName));

        // END SETUP / START TEST

        {
            WriteUnitOfWork uow(&txn);

            dropIndex(&txn, nss, idxName);
            ASSERT(!indexExists(&txn, nss, idxName));

            if (!rollback) {
                uow.commit();
            }
        }
        if (rollback) {
            ASSERT(indexExists(&txn, nss, idxName));
            ASSERT(indexReady(&txn, nss, idxName));
            ASSERT_EQ(3u, getNumIndexEntries(&txn, nss, idxName));
        } else {
            ASSERT(!indexExists(&txn, nss, idxName));
        }
    }
};

template <bool rollback>
class CreateDropIndex {
public:
    void run() {
        string ns = "unittests.rollback_create_drop_index";
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        NamespaceString nss(ns);
        dropDatabase(&txn, nss);
        createCollection(&txn, nss);

        ScopedTransaction transaction(&txn, MODE_IX);
        AutoGetDb autoDb(&txn, nss.db(), MODE_X);

        Collection* coll = autoDb.getDb()->getCollection(ns);
        IndexCatalog* catalog = coll->getIndexCatalog();

        string idxName = "a";
        BSONObj spec = BSON("ns" << ns << "key" << BSON("a" << 1) << "name" << idxName);

        // END SETUP / START TEST

        {
            WriteUnitOfWork uow(&txn);

            ASSERT_OK(catalog->createIndexOnEmptyCollection(&txn, spec));
            insertRecord(&txn, nss, BSON("a" << 1));
            insertRecord(&txn, nss, BSON("a" << 2));
            insertRecord(&txn, nss, BSON("a" << 3));
            ASSERT(indexExists(&txn, nss, idxName));
            ASSERT_EQ(3u, getNumIndexEntries(&txn, nss, idxName));

            dropIndex(&txn, nss, idxName);
            ASSERT(!indexExists(&txn, nss, idxName));

            if (!rollback) {
                uow.commit();
            }
        }

        ASSERT(!indexExists(&txn, nss, idxName));
    }
};

template <bool rollback>
class SetIndexHead {
public:
    void run() {
        string ns = "unittests.rollback_set_index_head";
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        NamespaceString nss(ns);
        dropDatabase(&txn, nss);
        createCollection(&txn, nss);

        ScopedTransaction transaction(&txn, MODE_IX);
        AutoGetDb autoDb(&txn, nss.db(), MODE_X);

        Collection* coll = autoDb.getDb()->getCollection(ns);
        IndexCatalog* catalog = coll->getIndexCatalog();

        string idxName = "a";
        BSONObj spec = BSON("ns" << ns << "key" << BSON("a" << 1) << "name" << idxName);

        {
            WriteUnitOfWork uow(&txn);
            ASSERT_OK(catalog->createIndexOnEmptyCollection(&txn, spec));
            uow.commit();
        }

        IndexDescriptor* indexDesc = catalog->findIndexByName(&txn, idxName);
        invariant(indexDesc);
        const IndexCatalogEntry* ice = catalog->getEntry(indexDesc);
        invariant(ice);
        HeadManager* headManager = ice->headManager();

        const RecordId oldHead = headManager->getHead(&txn);
        ASSERT_EQ(oldHead, ice->head(&txn));

        const RecordId dummyHead(123, 456);
        ASSERT_NE(oldHead, dummyHead);

        // END SETUP / START TEST

        {
            WriteUnitOfWork uow(&txn);

            headManager->setHead(&txn, dummyHead);

            ASSERT_EQ(ice->head(&txn), dummyHead);
            ASSERT_EQ(headManager->getHead(&txn), dummyHead);

            if (!rollback) {
                uow.commit();
            }
        }

        if (rollback) {
            ASSERT_EQ(ice->head(&txn), oldHead);
            ASSERT_EQ(headManager->getHead(&txn), oldHead);
        } else {
            ASSERT_EQ(ice->head(&txn), dummyHead);
            ASSERT_EQ(headManager->getHead(&txn), dummyHead);
        }
    }
};

template <bool rollback>
class CreateCollectionAndIndexes {
public:
    void run() {
        string ns = "unittests.rollback_create_collection_and_indexes";
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        NamespaceString nss(ns);
        dropDatabase(&txn, nss);

        ScopedTransaction transaction(&txn, MODE_IX);
        Lock::DBLock dbXLock(txn.lockState(), nss.db(), MODE_X);
        OldClientContext ctx(&txn, nss.ns());

        string idxNameA = "indexA";
        string idxNameB = "indexB";
        string idxNameC = "indexC";
        BSONObj specA = BSON("ns" << ns << "key" << BSON("a" << 1) << "name" << idxNameA);
        BSONObj specB = BSON("ns" << ns << "key" << BSON("b" << 1) << "name" << idxNameB);
        BSONObj specC = BSON("ns" << ns << "key" << BSON("c" << 1) << "name" << idxNameC);

        // END SETUP / START TEST

        {
            WriteUnitOfWork uow(&txn);
            ASSERT(!collectionExists(&ctx, nss.ns()));
            ASSERT_OK(userCreateNS(&txn, ctx.db(), nss.ns(), BSONObj(), false));
            ASSERT(collectionExists(&ctx, nss.ns()));
            Collection* coll = ctx.db()->getCollection(ns);
            IndexCatalog* catalog = coll->getIndexCatalog();

            ASSERT_OK(catalog->createIndexOnEmptyCollection(&txn, specA));
            ASSERT_OK(catalog->createIndexOnEmptyCollection(&txn, specB));
            ASSERT_OK(catalog->createIndexOnEmptyCollection(&txn, specC));

            if (!rollback) {
                uow.commit();
            }
        }  // uow
        if (rollback) {
            ASSERT(!collectionExists(&ctx, ns));
        } else {
            ASSERT(collectionExists(&ctx, ns));
            ASSERT(indexReady(&txn, nss, idxNameA));
            ASSERT(indexReady(&txn, nss, idxNameB));
            ASSERT(indexReady(&txn, nss, idxNameC));
        }
    }
};


class All : public Suite {
public:
    All() : Suite("rollback") {}

    template <template <bool> class T>
    void addAll() {
        add<T<false>>();
        add<T<true>>();
    }

    template <template <bool, bool> class T>
    void addAll() {
        add<T<false, false>>();
        add<T<true, false>>();
        add<T<false, true>>();
        add<T<true, true>>();
    }

    void setupTests() {
        addAll<CreateCollection>();
        addAll<RenameCollection>();
        addAll<DropCollection>();
        addAll<RenameDropTargetCollection>();
        addAll<ReplaceCollection>();
        addAll<CreateDropCollection>();
        addAll<TruncateCollection>();
        addAll<CreateIndex>();
        addAll<DropIndex>();
        addAll<CreateDropIndex>();
        addAll<SetIndexHead>();
        addAll<CreateCollectionAndIndexes>();
    }
};

SuiteInstance<All> all;

}  // namespace RollbackTests
