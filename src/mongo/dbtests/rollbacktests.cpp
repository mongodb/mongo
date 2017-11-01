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
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/head_manager.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/record_id.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"

using std::unique_ptr;
using std::list;
using std::string;

namespace RollbackTests {

namespace {
const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

void dropDatabase(OperationContext* opCtx, const NamespaceString& nss) {
    Lock::GlobalWrite globalWriteLock(opCtx);
    Database* db = dbHolder().get(opCtx, nss.db());

    if (db) {
        Database::dropDatabase(opCtx, db);
    }
}
bool collectionExists(OldClientContext* ctx, const string& ns) {
    const DatabaseCatalogEntry* dbEntry = ctx->db()->getDatabaseCatalogEntry();
    list<string> names;
    dbEntry->getCollectionNamespaces(&names);
    return std::find(names.begin(), names.end(), ns) != names.end();
}
void createCollection(OperationContext* opCtx, const NamespaceString& nss) {
    Lock::DBLock dbXLock(opCtx, nss.db(), MODE_X);
    OldClientContext ctx(opCtx, nss.ns());
    {
        WriteUnitOfWork uow(opCtx);
        ASSERT(!collectionExists(&ctx, nss.ns()));
        ASSERT_OK(userCreateNS(
            opCtx, ctx.db(), nss.ns(), BSONObj(), CollectionOptions::parseForCommand, false));
        ASSERT(collectionExists(&ctx, nss.ns()));
        uow.commit();
    }
}
Status renameCollection(OperationContext* opCtx,
                        const NamespaceString& source,
                        const NamespaceString& target) {
    ASSERT_EQ(source.db(), target.db());
    return renameCollection(opCtx, source, target, {});
}
Status truncateCollection(OperationContext* opCtx, const NamespaceString& nss) {
    Collection* coll = dbHolder().get(opCtx, nss.db())->getCollection(opCtx, nss);
    return coll->truncate(opCtx);
}

void insertRecord(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& data) {
    Collection* coll = dbHolder().get(opCtx, nss.db())->getCollection(opCtx, nss);
    OpDebug* const nullOpDebug = nullptr;
    ASSERT_OK(coll->insertDocument(opCtx, InsertStatement(data), nullOpDebug, false));
}
void assertOnlyRecord(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& data) {
    Collection* coll = dbHolder().get(opCtx, nss.db())->getCollection(opCtx, nss);
    auto cursor = coll->getCursor(opCtx);

    auto record = cursor->next();
    ASSERT(record);
    ASSERT_BSONOBJ_EQ(data, record->data.releaseToBson());

    ASSERT(!cursor->next());
}
void assertEmpty(OperationContext* opCtx, const NamespaceString& nss) {
    Collection* coll = dbHolder().get(opCtx, nss.db())->getCollection(opCtx, nss);
    ASSERT(!coll->getCursor(opCtx)->next());
}
bool indexExists(OperationContext* opCtx, const NamespaceString& nss, const string& idxName) {
    Collection* coll = dbHolder().get(opCtx, nss.db())->getCollection(opCtx, nss);
    return coll->getIndexCatalog()->findIndexByName(opCtx, idxName, true) != NULL;
}
bool indexReady(OperationContext* opCtx, const NamespaceString& nss, const string& idxName) {
    Collection* coll = dbHolder().get(opCtx, nss.db())->getCollection(opCtx, nss);
    return coll->getIndexCatalog()->findIndexByName(opCtx, idxName, false) != NULL;
}
size_t getNumIndexEntries(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const string& idxName) {
    size_t numEntries = 0;

    Collection* coll = dbHolder().get(opCtx, nss.db())->getCollection(opCtx, nss);
    IndexCatalog* catalog = coll->getIndexCatalog();
    IndexDescriptor* desc = catalog->findIndexByName(opCtx, idxName, false);

    if (desc) {
        auto cursor = catalog->getIndex(desc)->newCursor(opCtx);

        for (auto kv = cursor->seek(kMinBSONKey, true); kv; kv = cursor->next()) {
            numEntries++;
        }
    }

    return numEntries;
}

void dropIndex(OperationContext* opCtx, const NamespaceString& nss, const string& idxName) {
    Collection* coll = dbHolder().get(opCtx, nss.db())->getCollection(opCtx, nss);
    IndexDescriptor* desc = coll->getIndexCatalog()->findIndexByName(opCtx, idxName);
    ASSERT(desc);
    ASSERT_OK(coll->getIndexCatalog()->dropIndex(opCtx, desc));
}
}  // namespace

template <bool rollback, bool defaultIndexes, bool capped>
class CreateCollection {
public:
    void run() {
        string ns = "unittests.rollback_create_collection";
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        NamespaceString nss(ns);
        dropDatabase(&opCtx, nss);

        Lock::DBLock dbXLock(&opCtx, nss.db(), MODE_X);
        OldClientContext ctx(&opCtx, ns);
        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT(!collectionExists(&ctx, ns));
            auto options = capped ? BSON("capped" << true << "size" << 1000) : BSONObj();
            ASSERT_OK(userCreateNS(
                &opCtx, ctx.db(), ns, options, CollectionOptions::parseForCommand, defaultIndexes));
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

template <bool rollback, bool defaultIndexes, bool capped>
class DropCollection {
public:
    void run() {
        string ns = "unittests.rollback_drop_collection";
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        NamespaceString nss(ns);
        dropDatabase(&opCtx, nss);

        Lock::DBLock dbXLock(&opCtx, nss.db(), MODE_X);
        OldClientContext ctx(&opCtx, ns);
        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT(!collectionExists(&ctx, ns));
            auto options = capped ? BSON("capped" << true << "size" << 1000) : BSONObj();
            ASSERT_OK(userCreateNS(
                &opCtx, ctx.db(), ns, options, CollectionOptions::parseForCommand, defaultIndexes));
            uow.commit();
        }
        ASSERT(collectionExists(&ctx, ns));

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT(collectionExists(&ctx, ns));
            ASSERT_OK(ctx.db()->dropCollection(&opCtx, ns));
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

template <bool rollback, bool defaultIndexes, bool capped>
class RenameCollection {
public:
    void run() {
        NamespaceString source("unittests.rollback_rename_collection_src");
        NamespaceString target("unittests.rollback_rename_collection_dest");
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;

        dropDatabase(&opCtx, source);
        dropDatabase(&opCtx, target);

        Lock::GlobalWrite globalWriteLock(&opCtx);
        OldClientContext ctx(&opCtx, source.ns());

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT(!collectionExists(&ctx, source.ns()));
            ASSERT(!collectionExists(&ctx, target.ns()));
            auto options = capped ? BSON("capped" << true << "size" << 1000) : BSONObj();
            ASSERT_OK(userCreateNS(&opCtx,
                                   ctx.db(),
                                   source.ns(),
                                   options,
                                   CollectionOptions::parseForCommand,
                                   defaultIndexes));
            uow.commit();
        }
        ASSERT(collectionExists(&ctx, source.ns()));
        ASSERT(!collectionExists(&ctx, target.ns()));

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT_OK(renameCollection(&opCtx, source, target));
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

template <bool rollback, bool defaultIndexes, bool capped>
class RenameDropTargetCollection {
public:
    void run() {
        NamespaceString source("unittests.rollback_rename_droptarget_collection_src");
        NamespaceString target("unittests.rollback_rename_droptarget_collection_dest");
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;

        dropDatabase(&opCtx, source);
        dropDatabase(&opCtx, target);

        Lock::GlobalWrite globalWriteLock(&opCtx);
        OldClientContext ctx(&opCtx, source.ns());

        BSONObj sourceDoc = BSON("_id"
                                 << "source");
        BSONObj targetDoc = BSON("_id"
                                 << "target");

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT(!collectionExists(&ctx, source.ns()));
            ASSERT(!collectionExists(&ctx, target.ns()));
            auto options = capped ? BSON("capped" << true << "size" << 1000) : BSONObj();
            ASSERT_OK(userCreateNS(&opCtx,
                                   ctx.db(),
                                   source.ns(),
                                   options,
                                   CollectionOptions::parseForCommand,
                                   defaultIndexes));
            ASSERT_OK(userCreateNS(&opCtx,
                                   ctx.db(),
                                   target.ns(),
                                   options,
                                   CollectionOptions::parseForCommand,
                                   defaultIndexes));

            insertRecord(&opCtx, source, sourceDoc);
            insertRecord(&opCtx, target, targetDoc);

            uow.commit();
        }
        ASSERT(collectionExists(&ctx, source.ns()));
        ASSERT(collectionExists(&ctx, target.ns()));
        assertOnlyRecord(&opCtx, source, sourceDoc);
        assertOnlyRecord(&opCtx, target, targetDoc);

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&opCtx);
            BSONObjBuilder result;
            ASSERT_OK(
                dropCollection(&opCtx,
                               target,
                               result,
                               {},
                               DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
            ASSERT_OK(renameCollection(&opCtx, source, target));
            ASSERT(!collectionExists(&ctx, source.ns()));
            ASSERT(collectionExists(&ctx, target.ns()));
            assertOnlyRecord(&opCtx, target, sourceDoc);
            if (!rollback) {
                uow.commit();
            }
        }
        if (rollback) {
            ASSERT(collectionExists(&ctx, source.ns()));
            ASSERT(collectionExists(&ctx, target.ns()));
            assertOnlyRecord(&opCtx, source, sourceDoc);
            assertOnlyRecord(&opCtx, target, targetDoc);
        } else {
            ASSERT(!collectionExists(&ctx, source.ns()));
            ASSERT(collectionExists(&ctx, target.ns()));
            assertOnlyRecord(&opCtx, target, sourceDoc);
        }
    }
};

template <bool rollback, bool defaultIndexes>
class ReplaceCollection {
public:
    void run() {
        NamespaceString nss("unittests.rollback_replace_collection");
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        dropDatabase(&opCtx, nss);

        Lock::DBLock dbXLock(&opCtx, nss.db(), MODE_X);
        OldClientContext ctx(&opCtx, nss.ns());

        BSONObj oldDoc = BSON("_id"
                              << "old");
        BSONObj newDoc = BSON("_id"
                              << "new");

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT(!collectionExists(&ctx, nss.ns()));
            ASSERT_OK(userCreateNS(&opCtx,
                                   ctx.db(),
                                   nss.ns(),
                                   BSONObj(),
                                   CollectionOptions::parseForCommand,
                                   defaultIndexes));
            insertRecord(&opCtx, nss, oldDoc);
            uow.commit();
        }
        ASSERT(collectionExists(&ctx, nss.ns()));
        assertOnlyRecord(&opCtx, nss, oldDoc);

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&opCtx);
            BSONObjBuilder result;
            ASSERT_OK(
                dropCollection(&opCtx,
                               nss,
                               result,
                               {},
                               DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
            ASSERT(!collectionExists(&ctx, nss.ns()));
            ASSERT_OK(userCreateNS(&opCtx,
                                   ctx.db(),
                                   nss.ns(),
                                   BSONObj(),
                                   CollectionOptions::parseForCommand,
                                   defaultIndexes));
            ASSERT(collectionExists(&ctx, nss.ns()));
            insertRecord(&opCtx, nss, newDoc);
            assertOnlyRecord(&opCtx, nss, newDoc);
            if (!rollback) {
                uow.commit();
            }
        }
        ASSERT(collectionExists(&ctx, nss.ns()));
        if (rollback) {
            assertOnlyRecord(&opCtx, nss, oldDoc);
        } else {
            assertOnlyRecord(&opCtx, nss, newDoc);
        }
    }
};

template <bool rollback, bool defaultIndexes>
class CreateDropCollection {
public:
    void run() {
        NamespaceString nss("unittests.rollback_create_drop_collection");
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        dropDatabase(&opCtx, nss);

        Lock::DBLock dbXLock(&opCtx, nss.db(), MODE_X);
        OldClientContext ctx(&opCtx, nss.ns());

        BSONObj doc = BSON("_id"
                           << "example string");

        ASSERT(!collectionExists(&ctx, nss.ns()));
        {
            WriteUnitOfWork uow(&opCtx);

            ASSERT_OK(userCreateNS(&opCtx,
                                   ctx.db(),
                                   nss.ns(),
                                   BSONObj(),
                                   CollectionOptions::parseForCommand,
                                   defaultIndexes));
            ASSERT(collectionExists(&ctx, nss.ns()));
            insertRecord(&opCtx, nss, doc);
            assertOnlyRecord(&opCtx, nss, doc);

            BSONObjBuilder result;
            ASSERT_OK(
                dropCollection(&opCtx,
                               nss,
                               result,
                               {},
                               DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
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
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        dropDatabase(&opCtx, nss);

        Lock::DBLock dbXLock(&opCtx, nss.db(), MODE_X);
        OldClientContext ctx(&opCtx, nss.ns());

        BSONObj doc = BSON("_id"
                           << "foo");

        ASSERT(!collectionExists(&ctx, nss.ns()));
        {
            WriteUnitOfWork uow(&opCtx);

            ASSERT_OK(userCreateNS(&opCtx,
                                   ctx.db(),
                                   nss.ns(),
                                   BSONObj(),
                                   CollectionOptions::parseForCommand,
                                   defaultIndexes));
            ASSERT(collectionExists(&ctx, nss.ns()));
            insertRecord(&opCtx, nss, doc);
            assertOnlyRecord(&opCtx, nss, doc);
            uow.commit();
        }
        assertOnlyRecord(&opCtx, nss, doc);

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&opCtx);

            ASSERT_OK(truncateCollection(&opCtx, nss));
            ASSERT(collectionExists(&ctx, nss.ns()));
            assertEmpty(&opCtx, nss);

            if (!rollback) {
                uow.commit();
            }
        }
        ASSERT(collectionExists(&ctx, nss.ns()));
        if (rollback) {
            assertOnlyRecord(&opCtx, nss, doc);
        } else {
            assertEmpty(&opCtx, nss);
        }
    }
};

template <bool rollback>
class CreateIndex {
public:
    void run() {
        string ns = "unittests.rollback_create_index";
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        NamespaceString nss(ns);
        dropDatabase(&opCtx, nss);
        createCollection(&opCtx, nss);

        AutoGetDb autoDb(&opCtx, nss.db(), MODE_X);

        Collection* coll = autoDb.getDb()->getCollection(&opCtx, nss);
        IndexCatalog* catalog = coll->getIndexCatalog();

        string idxName = "a";
        BSONObj spec = BSON("ns" << ns << "key" << BSON("a" << 1) << "name" << idxName << "v"
                                 << static_cast<int>(kIndexVersion));

        // END SETUP / START TEST

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT_OK(catalog->createIndexOnEmptyCollection(&opCtx, spec));
            insertRecord(&opCtx, nss, BSON("a" << 1));
            insertRecord(&opCtx, nss, BSON("a" << 2));
            insertRecord(&opCtx, nss, BSON("a" << 3));
            if (!rollback) {
                uow.commit();
            }
        }

        if (rollback) {
            ASSERT(!indexExists(&opCtx, nss, idxName));
        } else {
            ASSERT(indexReady(&opCtx, nss, idxName));
        }
    }
};

template <bool rollback>
class DropIndex {
public:
    void run() {
        string ns = "unittests.rollback_drop_index";
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        NamespaceString nss(ns);
        dropDatabase(&opCtx, nss);
        createCollection(&opCtx, nss);

        AutoGetDb autoDb(&opCtx, nss.db(), MODE_X);

        Collection* coll = autoDb.getDb()->getCollection(&opCtx, nss);
        IndexCatalog* catalog = coll->getIndexCatalog();

        string idxName = "a";
        BSONObj spec = BSON("ns" << ns << "key" << BSON("a" << 1) << "name" << idxName << "v"
                                 << static_cast<int>(kIndexVersion));

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT_OK(catalog->createIndexOnEmptyCollection(&opCtx, spec));
            insertRecord(&opCtx, nss, BSON("a" << 1));
            insertRecord(&opCtx, nss, BSON("a" << 2));
            insertRecord(&opCtx, nss, BSON("a" << 3));
            uow.commit();
        }
        ASSERT(indexReady(&opCtx, nss, idxName));
        ASSERT_EQ(3u, getNumIndexEntries(&opCtx, nss, idxName));

        // END SETUP / START TEST

        {
            WriteUnitOfWork uow(&opCtx);

            dropIndex(&opCtx, nss, idxName);
            ASSERT(!indexExists(&opCtx, nss, idxName));

            if (!rollback) {
                uow.commit();
            }
        }
        if (rollback) {
            ASSERT(indexExists(&opCtx, nss, idxName));
            ASSERT(indexReady(&opCtx, nss, idxName));
            ASSERT_EQ(3u, getNumIndexEntries(&opCtx, nss, idxName));
        } else {
            ASSERT(!indexExists(&opCtx, nss, idxName));
        }
    }
};

template <bool rollback>
class CreateDropIndex {
public:
    void run() {
        string ns = "unittests.rollback_create_drop_index";
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        NamespaceString nss(ns);
        dropDatabase(&opCtx, nss);
        createCollection(&opCtx, nss);

        AutoGetDb autoDb(&opCtx, nss.db(), MODE_X);

        Collection* coll = autoDb.getDb()->getCollection(&opCtx, nss);
        IndexCatalog* catalog = coll->getIndexCatalog();

        string idxName = "a";
        BSONObj spec = BSON("ns" << ns << "key" << BSON("a" << 1) << "name" << idxName << "v"
                                 << static_cast<int>(kIndexVersion));

        // END SETUP / START TEST

        {
            WriteUnitOfWork uow(&opCtx);

            ASSERT_OK(catalog->createIndexOnEmptyCollection(&opCtx, spec));
            insertRecord(&opCtx, nss, BSON("a" << 1));
            insertRecord(&opCtx, nss, BSON("a" << 2));
            insertRecord(&opCtx, nss, BSON("a" << 3));
            ASSERT(indexExists(&opCtx, nss, idxName));
            ASSERT_EQ(3u, getNumIndexEntries(&opCtx, nss, idxName));

            dropIndex(&opCtx, nss, idxName);
            ASSERT(!indexExists(&opCtx, nss, idxName));

            if (!rollback) {
                uow.commit();
            }
        }

        ASSERT(!indexExists(&opCtx, nss, idxName));
    }
};

template <bool rollback>
class SetIndexHead {
public:
    void run() {
        string ns = "unittests.rollback_set_index_head";
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        NamespaceString nss(ns);
        dropDatabase(&opCtx, nss);
        createCollection(&opCtx, nss);

        AutoGetDb autoDb(&opCtx, nss.db(), MODE_X);

        Collection* coll = autoDb.getDb()->getCollection(&opCtx, nss);
        IndexCatalog* catalog = coll->getIndexCatalog();

        string idxName = "a";
        BSONObj spec = BSON("ns" << ns << "key" << BSON("a" << 1) << "name" << idxName << "v"
                                 << static_cast<int>(kIndexVersion));

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT_OK(catalog->createIndexOnEmptyCollection(&opCtx, spec));
            uow.commit();
        }

        IndexDescriptor* indexDesc = catalog->findIndexByName(&opCtx, idxName);
        invariant(indexDesc);
        const IndexCatalogEntry* ice = catalog->getEntry(indexDesc);
        invariant(ice);
        HeadManager* headManager = ice->headManager();

        const RecordId oldHead = headManager->getHead(&opCtx);
        ASSERT_EQ(oldHead, ice->head(&opCtx));

        const RecordId dummyHead(123, 456);
        ASSERT_NE(oldHead, dummyHead);

        // END SETUP / START TEST

        {
            WriteUnitOfWork uow(&opCtx);

            headManager->setHead(&opCtx, dummyHead);

            ASSERT_EQ(ice->head(&opCtx), dummyHead);
            ASSERT_EQ(headManager->getHead(&opCtx), dummyHead);

            if (!rollback) {
                uow.commit();
            }
        }

        if (rollback) {
            ASSERT_EQ(ice->head(&opCtx), oldHead);
            ASSERT_EQ(headManager->getHead(&opCtx), oldHead);
        } else {
            ASSERT_EQ(ice->head(&opCtx), dummyHead);
            ASSERT_EQ(headManager->getHead(&opCtx), dummyHead);
        }
    }
};

template <bool rollback>
class CreateCollectionAndIndexes {
public:
    void run() {
        string ns = "unittests.rollback_create_collection_and_indexes";
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        NamespaceString nss(ns);
        dropDatabase(&opCtx, nss);

        Lock::DBLock dbXLock(&opCtx, nss.db(), MODE_X);
        OldClientContext ctx(&opCtx, nss.ns());

        string idxNameA = "indexA";
        string idxNameB = "indexB";
        string idxNameC = "indexC";
        BSONObj specA = BSON("ns" << ns << "key" << BSON("a" << 1) << "name" << idxNameA << "v"
                                  << static_cast<int>(kIndexVersion));
        BSONObj specB = BSON("ns" << ns << "key" << BSON("b" << 1) << "name" << idxNameB << "v"
                                  << static_cast<int>(kIndexVersion));
        BSONObj specC = BSON("ns" << ns << "key" << BSON("c" << 1) << "name" << idxNameC << "v"
                                  << static_cast<int>(kIndexVersion));

        // END SETUP / START TEST

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT(!collectionExists(&ctx, nss.ns()));
            ASSERT_OK(userCreateNS(
                &opCtx, ctx.db(), nss.ns(), BSONObj(), CollectionOptions::parseForCommand, false));
            ASSERT(collectionExists(&ctx, nss.ns()));
            Collection* coll = ctx.db()->getCollection(&opCtx, nss);
            IndexCatalog* catalog = coll->getIndexCatalog();

            ASSERT_OK(catalog->createIndexOnEmptyCollection(&opCtx, specA));
            ASSERT_OK(catalog->createIndexOnEmptyCollection(&opCtx, specB));
            ASSERT_OK(catalog->createIndexOnEmptyCollection(&opCtx, specC));

            if (!rollback) {
                uow.commit();
            }
        }  // uow
        if (rollback) {
            ASSERT(!collectionExists(&ctx, ns));
        } else {
            ASSERT(collectionExists(&ctx, ns));
            ASSERT(indexReady(&opCtx, nss, idxNameA));
            ASSERT(indexReady(&opCtx, nss, idxNameB));
            ASSERT(indexReady(&opCtx, nss, idxNameC));
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
        add<T<false, true>>();
        add<T<true, false>>();
        add<T<true, true>>();
    }
    template <template <bool, bool, bool> class T>
    void addAll() {
        add<T<false, false, false>>();
        add<T<false, false, true>>();
        add<T<false, true, false>>();
        add<T<false, true, true>>();
        add<T<true, false, false>>();
        add<T<true, false, true>>();
        add<T<true, true, false>>();
        add<T<true, true, true>>();
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
