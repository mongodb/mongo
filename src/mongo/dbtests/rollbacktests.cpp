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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/record_id.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"

using mongo::unittest::assertGet;
using std::list;
using std::string;
using std::unique_ptr;

namespace RollbackTests {

namespace {
const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

void dropDatabase(OperationContext* opCtx, const NamespaceString& nss) {
    Lock::GlobalWrite globalWriteLock(opCtx);
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, nss.dbName());

    if (db) {
        databaseHolder->dropDb(opCtx, db);
    }
}
bool collectionExists(OperationContext* opCtx, OldClientContext* ctx, const string& ns) {
    return (bool)CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx,
                                                                            NamespaceString(ns));
}

void createCollection(OperationContext* opCtx, const NamespaceString& nss) {
    Lock::DBLock dbXLock(opCtx, nss.dbName(), MODE_X);
    OldClientContext ctx(opCtx, nss.ns());
    {
        WriteUnitOfWork uow(opCtx);
        ASSERT(!collectionExists(opCtx, &ctx, nss.ns()));
        CollectionOptions defaultCollectionOptions;
        ASSERT_OK(ctx.db()->userCreateNS(opCtx, nss, defaultCollectionOptions, false));
        ASSERT(collectionExists(opCtx, &ctx, nss.ns()));
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
    CollectionWriter coll(opCtx, nss);
    return coll.getWritableCollection(opCtx)->truncate(opCtx);
}

void insertRecord(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& data) {
    auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
    OpDebug* const nullOpDebug = nullptr;
    ASSERT_OK(coll->insertDocument(opCtx, InsertStatement(data), nullOpDebug, false));
}
void assertOnlyRecord(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& data) {
    auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
    auto cursor = coll->getCursor(opCtx);

    auto record = cursor->next();
    ASSERT(record);
    ASSERT_BSONOBJ_EQ(data, record->data.releaseToBson());

    ASSERT(!cursor->next());
}
void assertEmpty(OperationContext* opCtx, const NamespaceString& nss) {
    auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
    ASSERT(!coll->getCursor(opCtx)->next());
}
bool indexExists(OperationContext* opCtx, const NamespaceString& nss, const string& idxName) {
    auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
    return coll->getIndexCatalog()->findIndexByName(
               opCtx,
               idxName,
               IndexCatalog::InclusionPolicy::kReady |
                   IndexCatalog::InclusionPolicy::kUnfinished) != nullptr;
}
bool indexReady(OperationContext* opCtx, const NamespaceString& nss, const string& idxName) {
    auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
    return coll->getIndexCatalog()->findIndexByName(
               opCtx, idxName, IndexCatalog::InclusionPolicy::kReady) != nullptr;
}
size_t getNumIndexEntries(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const string& idxName) {
    size_t numEntries = 0;

    auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
    const IndexCatalog* catalog = coll->getIndexCatalog();
    auto desc = catalog->findIndexByName(opCtx, idxName, IndexCatalog::InclusionPolicy::kReady);

    if (desc) {
        auto iam = catalog->getEntry(desc)->accessMethod()->asSortedData();
        auto cursor = iam->newCursor(opCtx);
        KeyString::Builder keyString(iam->getSortedDataInterface()->getKeyStringVersion(),
                                     BSONObj(),
                                     iam->getSortedDataInterface()->getOrdering());
        for (auto kv = cursor->seek(keyString.getValueCopy()); kv; kv = cursor->next()) {
            numEntries++;
        }
    }

    return numEntries;
}

void dropIndex(OperationContext* opCtx, const NamespaceString& nss, const string& idxName) {
    CollectionWriter coll(opCtx, nss);
    auto desc =
        coll.getWritableCollection(opCtx)->getIndexCatalog()->findIndexByName(opCtx, idxName);
    ASSERT(desc);
    ASSERT_OK(coll.getWritableCollection(opCtx)->getIndexCatalog()->dropIndex(
        opCtx, coll.getWritableCollection(opCtx), desc));
}
}  // namespace

template <bool rollback, bool defaultIndexes, bool capped>
class CreateCollection {
public:
    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!getGlobalServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        string ns = "unittests.rollback_create_collection";
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        NamespaceString nss(ns);
        dropDatabase(&opCtx, nss);

        Lock::DBLock dbXLock(&opCtx, nss.dbName(), MODE_X);
        OldClientContext ctx(&opCtx, ns);
        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT(!collectionExists(&opCtx, &ctx, ns));
            auto options = capped ? BSON("capped" << true << "size" << 1000) : BSONObj();
            CollectionOptions collectionOptions =
                assertGet(CollectionOptions::parse(options, CollectionOptions::parseForCommand));
            ASSERT_OK(ctx.db()->userCreateNS(&opCtx, nss, collectionOptions, defaultIndexes));
            ASSERT(collectionExists(&opCtx, &ctx, ns));
            if (!rollback) {
                uow.commit();
            }
        }
        if (rollback) {
            ASSERT(!collectionExists(&opCtx, &ctx, ns));
        } else {
            ASSERT(collectionExists(&opCtx, &ctx, ns));
        }
    }
};

template <bool rollback, bool defaultIndexes, bool capped>
class DropCollection {
public:
    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!getGlobalServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

        string ns = "unittests.rollback_drop_collection";
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        NamespaceString nss(ns);
        dropDatabase(&opCtx, nss);

        Lock::DBLock dbXLock(&opCtx, nss.dbName(), MODE_X);
        OldClientContext ctx(&opCtx, ns);
        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT(!collectionExists(&opCtx, &ctx, ns));
            auto options = capped ? BSON("capped" << true << "size" << 1000) : BSONObj();
            CollectionOptions collectionOptions =
                assertGet(CollectionOptions::parse(options, CollectionOptions::parseForCommand));
            ASSERT_OK(ctx.db()->userCreateNS(&opCtx, nss, collectionOptions, defaultIndexes));
            uow.commit();
        }
        ASSERT(collectionExists(&opCtx, &ctx, ns));

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT(collectionExists(&opCtx, &ctx, ns));
            ASSERT_OK(ctx.db()->dropCollection(&opCtx, NamespaceString(ns)));
            ASSERT(!collectionExists(&opCtx, &ctx, ns));
            if (!rollback) {
                uow.commit();
            }
        }
        if (rollback) {
            ASSERT(collectionExists(&opCtx, &ctx, ns));
        } else {
            ASSERT(!collectionExists(&opCtx, &ctx, ns));
        }
    }
};

template <bool rollback, bool defaultIndexes, bool capped>
class RenameCollection {
public:
    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!getGlobalServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

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
            ASSERT(!collectionExists(&opCtx, &ctx, source.ns()));
            ASSERT(!collectionExists(&opCtx, &ctx, target.ns()));
            auto options = capped ? BSON("capped" << true << "size" << 1000) : BSONObj();
            CollectionOptions collectionOptions =
                assertGet(CollectionOptions::parse(options, CollectionOptions::parseForCommand));
            ASSERT_OK(ctx.db()->userCreateNS(&opCtx, source, collectionOptions, defaultIndexes));
            uow.commit();
        }
        ASSERT(collectionExists(&opCtx, &ctx, source.ns()));
        ASSERT(!collectionExists(&opCtx, &ctx, target.ns()));

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT_OK(renameCollection(&opCtx, source, target));
            ASSERT(!collectionExists(&opCtx, &ctx, source.ns()));
            ASSERT(collectionExists(&opCtx, &ctx, target.ns()));
            if (!rollback) {
                uow.commit();
            }
        }
        if (rollback) {
            ASSERT(collectionExists(&opCtx, &ctx, source.ns()));
            ASSERT(!collectionExists(&opCtx, &ctx, target.ns()));
        } else {
            ASSERT(!collectionExists(&opCtx, &ctx, source.ns()));
            ASSERT(collectionExists(&opCtx, &ctx, target.ns()));
        }
    }
};

template <bool rollback, bool defaultIndexes, bool capped>
class RenameDropTargetCollection {
public:
    void run() {
        // Skip the test if the storage engine doesn't support capped collections.
        if (!getGlobalServiceContext()->getStorageEngine()->supportsCappedCollections()) {
            return;
        }

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
            ASSERT(!collectionExists(&opCtx, &ctx, source.ns()));
            ASSERT(!collectionExists(&opCtx, &ctx, target.ns()));
            auto options = capped ? BSON("capped" << true << "size" << 1000) : BSONObj();
            CollectionOptions collectionOptions =
                assertGet(CollectionOptions::parse(options, CollectionOptions::parseForCommand));
            auto db = ctx.db();
            ASSERT_OK(db->userCreateNS(&opCtx, source, collectionOptions, defaultIndexes));
            ASSERT_OK(db->userCreateNS(&opCtx, target, collectionOptions, defaultIndexes));

            insertRecord(&opCtx, source, sourceDoc);
            insertRecord(&opCtx, target, targetDoc);

            uow.commit();
        }
        ASSERT(collectionExists(&opCtx, &ctx, source.ns()));
        ASSERT(collectionExists(&opCtx, &ctx, target.ns()));
        assertOnlyRecord(&opCtx, source, sourceDoc);
        assertOnlyRecord(&opCtx, target, targetDoc);

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT_OK(dropCollectionForApplyOps(
                &opCtx,
                target,
                {},
                DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
            ASSERT_OK(renameCollection(&opCtx, source, target));
            ASSERT(!collectionExists(&opCtx, &ctx, source.ns()));
            ASSERT(collectionExists(&opCtx, &ctx, target.ns()));
            assertOnlyRecord(&opCtx, target, sourceDoc);
            if (!rollback) {
                uow.commit();
            }
        }
        if (rollback) {
            ASSERT(collectionExists(&opCtx, &ctx, source.ns()));
            ASSERT(collectionExists(&opCtx, &ctx, target.ns()));
            assertOnlyRecord(&opCtx, source, sourceDoc);
            assertOnlyRecord(&opCtx, target, targetDoc);
        } else {
            ASSERT(!collectionExists(&opCtx, &ctx, source.ns()));
            ASSERT(collectionExists(&opCtx, &ctx, target.ns()));
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

        Lock::DBLock dbXLock(&opCtx, nss.dbName(), MODE_X);
        OldClientContext ctx(&opCtx, nss.ns());

        BSONObj oldDoc = BSON("_id"
                              << "old");
        BSONObj newDoc = BSON("_id"
                              << "new");

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT(!collectionExists(&opCtx, &ctx, nss.ns()));
            CollectionOptions collectionOptions =
                assertGet(CollectionOptions::parse(BSONObj(), CollectionOptions::parseForCommand));
            ASSERT_OK(ctx.db()->userCreateNS(&opCtx, nss, collectionOptions, defaultIndexes));
            insertRecord(&opCtx, nss, oldDoc);
            uow.commit();
        }
        ASSERT(collectionExists(&opCtx, &ctx, nss.ns()));
        assertOnlyRecord(&opCtx, nss, oldDoc);

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT_OK(dropCollectionForApplyOps(
                &opCtx,
                nss,
                {},
                DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
            ASSERT(!collectionExists(&opCtx, &ctx, nss.ns()));
            CollectionOptions collectionOptions =
                assertGet(CollectionOptions::parse(BSONObj(), CollectionOptions::parseForCommand));
            ASSERT_OK(ctx.db()->userCreateNS(&opCtx, nss, collectionOptions, defaultIndexes));
            ASSERT(collectionExists(&opCtx, &ctx, nss.ns()));
            insertRecord(&opCtx, nss, newDoc);
            assertOnlyRecord(&opCtx, nss, newDoc);
            if (!rollback) {
                uow.commit();
            }
        }
        ASSERT(collectionExists(&opCtx, &ctx, nss.ns()));
        if (rollback) {
            assertOnlyRecord(&opCtx, nss, oldDoc);
        } else {
            assertOnlyRecord(&opCtx, nss, newDoc);
        }
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

        Lock::DBLock dbXLock(&opCtx, nss.dbName(), MODE_X);
        OldClientContext ctx(&opCtx, nss.ns());

        BSONObj doc = BSON("_id"
                           << "foo");

        ASSERT(!collectionExists(&opCtx, &ctx, nss.ns()));
        {
            WriteUnitOfWork uow(&opCtx);

            CollectionOptions collectionOptions =
                assertGet(CollectionOptions::parse(BSONObj(), CollectionOptions::parseForCommand));
            ASSERT_OK(ctx.db()->userCreateNS(&opCtx, nss, collectionOptions, defaultIndexes));
            ASSERT(collectionExists(&opCtx, &ctx, nss.ns()));
            insertRecord(&opCtx, nss, doc);
            assertOnlyRecord(&opCtx, nss, doc);
            uow.commit();
        }
        assertOnlyRecord(&opCtx, nss, doc);

        // END OF SETUP / START OF TEST

        {
            WriteUnitOfWork uow(&opCtx);

            ASSERT_OK(truncateCollection(&opCtx, nss));
            ASSERT(collectionExists(&opCtx, &ctx, nss.ns()));
            assertEmpty(&opCtx, nss);

            if (!rollback) {
                uow.commit();
            }
        }
        ASSERT(collectionExists(&opCtx, &ctx, nss.ns()));
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

        CollectionWriter coll(&opCtx, nss);

        string idxName = "a";
        BSONObj spec = BSON("key" << BSON("a" << 1) << "name" << idxName << "v"
                                  << static_cast<int>(kIndexVersion));

        // END SETUP / START TEST

        {
            WriteUnitOfWork uow(&opCtx);
            IndexCatalog* catalog = coll.getWritableCollection(&opCtx)->getIndexCatalog();
            ASSERT_OK(catalog->createIndexOnEmptyCollection(
                &opCtx, coll.getWritableCollection(&opCtx), spec));
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

        CollectionWriter coll(&opCtx, nss);

        string idxName = "a";
        BSONObj spec = BSON("key" << BSON("a" << 1) << "name" << idxName << "v"
                                  << static_cast<int>(kIndexVersion));

        {
            WriteUnitOfWork uow(&opCtx);
            IndexCatalog* catalog = coll.getWritableCollection(&opCtx)->getIndexCatalog();
            ASSERT_OK(catalog->createIndexOnEmptyCollection(
                &opCtx, coll.getWritableCollection(&opCtx), spec));
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
        CollectionWriter coll(&opCtx, nss);

        string idxName = "a";
        BSONObj spec = BSON("key" << BSON("a" << 1) << "name" << idxName << "v"
                                  << static_cast<int>(kIndexVersion));

        // END SETUP / START TEST

        {
            WriteUnitOfWork uow(&opCtx);
            IndexCatalog* catalog = coll.getWritableCollection(&opCtx)->getIndexCatalog();

            ASSERT_OK(catalog->createIndexOnEmptyCollection(
                &opCtx, coll.getWritableCollection(&opCtx), spec));
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
class CreateCollectionAndIndexes {
public:
    void run() {
        string ns = "unittests.rollback_create_collection_and_indexes";
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        NamespaceString nss(ns);
        dropDatabase(&opCtx, nss);

        Lock::DBLock dbXLock(&opCtx, nss.dbName(), MODE_X);
        OldClientContext ctx(&opCtx, nss.ns());

        string idxNameA = "indexA";
        string idxNameB = "indexB";
        string idxNameC = "indexC";
        BSONObj specA = BSON("key" << BSON("a" << 1) << "name" << idxNameA << "v"
                                   << static_cast<int>(kIndexVersion));
        BSONObj specB = BSON("key" << BSON("b" << 1) << "name" << idxNameB << "v"
                                   << static_cast<int>(kIndexVersion));
        BSONObj specC = BSON("key" << BSON("c" << 1) << "name" << idxNameC << "v"
                                   << static_cast<int>(kIndexVersion));

        // END SETUP / START TEST

        {
            WriteUnitOfWork uow(&opCtx);
            ASSERT(!collectionExists(&opCtx, &ctx, nss.ns()));
            CollectionOptions collectionOptions =
                assertGet(CollectionOptions::parse(BSONObj(), CollectionOptions::parseForCommand));
            ASSERT_OK(ctx.db()->userCreateNS(&opCtx, nss, collectionOptions, false));
            ASSERT(collectionExists(&opCtx, &ctx, nss.ns()));
            CollectionWriter coll(&opCtx, nss);
            auto writableColl = coll.getWritableCollection(&opCtx);
            IndexCatalog* catalog = writableColl->getIndexCatalog();

            ASSERT_OK(catalog->createIndexOnEmptyCollection(&opCtx, writableColl, specA));
            ASSERT_OK(catalog->createIndexOnEmptyCollection(&opCtx, writableColl, specB));
            ASSERT_OK(catalog->createIndexOnEmptyCollection(&opCtx, writableColl, specC));

            if (!rollback) {
                uow.commit();
            }
        }  // uow
        if (rollback) {
            ASSERT(!collectionExists(&opCtx, &ctx, ns));
        } else {
            ASSERT(collectionExists(&opCtx, &ctx, ns));
            ASSERT(indexReady(&opCtx, nss, idxNameA));
            ASSERT(indexReady(&opCtx, nss, idxNameB));
            ASSERT(indexReady(&opCtx, nss, idxNameC));
        }
    }
};


class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("rollback") {}

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
        addAll<TruncateCollection>();
        addAll<CreateIndex>();
        addAll<DropIndex>();
        addAll<CreateDropIndex>();
        addAll<CreateCollectionAndIndexes>();
    }
};

OldStyleSuiteInitializer<All> all;

}  // namespace RollbackTests
