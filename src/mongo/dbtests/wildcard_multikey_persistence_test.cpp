
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include <memory>

#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using namespace unittest;

static const RecordId kMetadataId = RecordId::minReserved();

static const int kIndexVersion = static_cast<int>(IndexDescriptor::kLatestIndexVersion);
static const NamespaceString kDefaultNSS{"wildcard_multikey_persistence.test"};
static const std::string kDefaultIndexName{"wildcard_multikey"};
static const BSONObj kDefaultIndexKey = fromjson("{'$**': 1}");
static const BSONObj kDefaultPathProjection;

static constexpr auto kIdField = "_id";

std::vector<InsertStatement> toInserts(std::vector<BSONObj> docs) {
    std::vector<InsertStatement> inserts(docs.size());
    std::transform(docs.cbegin(), docs.cend(), inserts.begin(), [](const BSONObj& doc) {
        return InsertStatement(doc);
    });
    return inserts;
}

class WildcardMultikeyPersistenceTestFixture : public unittest::Test {
public:
    WildcardMultikeyPersistenceTestFixture() {
        _opCtx = cc().makeOperationContext();
    }

    virtual ~WildcardMultikeyPersistenceTestFixture() {
        _opCtx.reset();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

protected:
    void assertSetupEnvironment(bool background,
                                std::vector<BSONObj> initialDocs = {},
                                BSONObj indexKey = kDefaultIndexKey,
                                BSONObj pathProjection = kDefaultPathProjection,
                                const std::string& indexName = kDefaultIndexName,
                                const NamespaceString& nss = kDefaultNSS) {
        assertRecreateCollection(nss);
        assertInsertDocuments(initialDocs, nss);
        assertCreateIndexForColl(nss, indexName, indexKey, pathProjection, background);
    }

    void assertIndexContentsEquals(std::vector<IndexKeyEntry> expectedKeys,
                                   bool expectIndexIsMultikey = true,
                                   const NamespaceString& nss = kDefaultNSS,
                                   const std::string& indexName = kDefaultIndexName) {
        // Subsequent operations must take place under a collection lock.
        AutoGetCollectionForRead autoColl(opCtx(), nss);
        auto collection = autoColl.getCollection();

        // Verify whether or not the index has been marked as multikey.
        ASSERT_EQ(expectIndexIsMultikey, getIndexDesc(collection, indexName)->isMultikey(opCtx()));

        // Obtain a cursor over the index, and confirm that the keys are present in order.
        auto indexCursor = getIndexCursor(collection, indexName);
        auto indexKey = indexCursor->seek(kMinBSONKey, true);
        try {
            for (const auto& expectedKey : expectedKeys) {
                ASSERT(indexKey);
                ASSERT_BSONOBJ_EQ(expectedKey.key, indexKey->key);
                ASSERT_EQ(expectedKey.loc, indexKey->loc);
                indexKey = indexCursor->next();
            }
            // Confirm that there are no further keys in the index.
            ASSERT(!indexKey);
        } catch (const TestAssertionFailureException& ex) {
            log() << "Writing remaining index keys to debug log:";
            while (indexKey) {
                log() << "{ key: " << indexKey->key << ", loc: " << indexKey->loc << " }";
                indexKey = indexCursor->next();
            }
            throw ex;
        }
    }

    /**
     * Verifes that the index access method associated with 'indexName' in the collection identified
     * by 'nss' reports 'expectedPaths' as the set of multikey paths.
     */
    void assertMultikeyPathSetEquals(const std::set<std::string>& expectedPaths,
                                     const NamespaceString& nss = kDefaultNSS,
                                     const std::string& indexName = kDefaultIndexName) {
        // Convert the set of std::string to a set of FieldRef.
        std::set<FieldRef> expectedFieldRefs;
        for (auto&& path : expectedPaths) {
            ASSERT_TRUE(expectedFieldRefs.emplace(path).second);
        }
        ASSERT_EQ(expectedPaths.size(), expectedFieldRefs.size());

        AutoGetCollectionForRead autoColl(opCtx(), nss);
        auto collection = autoColl.getCollection();
        auto indexAccessMethod = getIndex(collection, indexName);
        MultikeyMetadataAccessStats stats;
        auto multikeyPathSet = indexAccessMethod->getMultikeyPathSet(opCtx(), &stats);

        ASSERT(expectedFieldRefs == multikeyPathSet);
    }

    void assertRecreateCollection(const NamespaceString& nss) {
        ASSERT_OK(_storage.dropCollection(opCtx(), nss));
        ASSERT_OK(_storage.createCollection(opCtx(), nss, collOptions()));
    }

    void assertInsertDocuments(std::vector<BSONObj> docs,
                               const NamespaceString& nss = kDefaultNSS) {
        ASSERT_OK(_storage.insertDocuments(opCtx(), nss, toInserts(docs)));
    }

    void assertUpdateDocuments(std::vector<std::pair<BSONObj, BSONObj>> updates,
                               const NamespaceString& nss = kDefaultNSS) {
        for (const auto& update : updates) {
            ASSERT_OK(_storage.updateSingleton(
                opCtx(), nss, update.first, {update.second, Timestamp(0)}));
        }
    }

    void assertUpsertDocuments(std::vector<BSONObj> upserts,
                               const NamespaceString& nss = kDefaultNSS) {
        for (const auto& upsert : upserts) {
            ASSERT_OK(_storage.upsertById(opCtx(), nss, upsert[kIdField], upsert));
        }
    }

    void assertRemoveDocuments(std::vector<BSONObj> docs,
                               const NamespaceString& nss = kDefaultNSS) {
        for (const auto& doc : docs) {
            ASSERT_OK(_storage.deleteByFilter(opCtx(), nss, doc));
        }
    }

    void assertCreateIndexForColl(const NamespaceString& nss,
                                  const std::string& name,
                                  BSONObj key,
                                  BSONObj pathProjection,
                                  bool background) {
        BSONObjBuilder bob =
            std::move(BSONObjBuilder() << "ns" << nss.ns() << "name" << name << "key" << key);

        if (!pathProjection.isEmpty())
            bob << IndexDescriptor::kPathProjectionFieldName << pathProjection;

        auto indexSpec = (bob << "v" << kIndexVersion << "background" << background).obj();

        Lock::DBLock dbLock(opCtx(), nss.db(), MODE_X);
        AutoGetCollection autoColl(opCtx(), nss, MODE_X);
        auto coll = autoColl.getCollection();

        MultiIndexBlock indexer(opCtx(), coll);

        // Initialize the index builder and add all documents currently in the collection.
        ASSERT_OK(indexer.init(indexSpec, MultiIndexBlock::kNoopOnInitFn).getStatus());
        ASSERT_OK(indexer.insertAllDocumentsInCollection());

        WriteUnitOfWork wunit(opCtx());
        ASSERT_OK(
            indexer.commit(MultiIndexBlock::kNoopOnCreateEachFn, MultiIndexBlock::kNoopOnCommitFn));
        wunit.commit();
    }

    std::vector<BSONObj> makeDocs(const std::vector<std::string>& jsonObjs) {
        std::vector<BSONObj> docs(jsonObjs.size());
        std::transform(
            jsonObjs.cbegin(), jsonObjs.cend(), docs.begin(), [this](const std::string& json) {
                return fromjson(json).addField(BSON(kIdField << (_id++))[kIdField]);
            });
        return docs;
    }

    const IndexDescriptor* getIndexDesc(const Collection* collection, const StringData indexName) {
        return collection->getIndexCatalog()->findIndexByName(opCtx(), indexName);
    }

    const IndexAccessMethod* getIndex(Collection* collection, const StringData indexName) {
        return collection->getIndexCatalog()
            ->getEntry(getIndexDesc(collection, indexName))
            ->accessMethod();
    }

    std::unique_ptr<SortedDataInterface::Cursor> getIndexCursor(Collection* collection,
                                                                const StringData indexName) {
        return getIndex(collection, indexName)->newCursor(opCtx());
    }

    CollectionOptions collOptions() {
        CollectionOptions collOpts;
        collOpts.uuid = UUID::gen();
        return collOpts;
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    repl::StorageInterfaceImpl _storage;
    bool _origWildcardKnob{false};
    int _id{1};
};

TEST_F(WildcardMultikeyPersistenceTestFixture, RecordMultikeyPathsInBulkIndexBuild) {
    // Create the test collection, add some initial documents, and build a foreground $** index.
    assertSetupEnvironment(false, makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}"}));

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 'a', '': 1}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, RecordMultikeyPathsInBackgroundIndexBuild) {
    // Create the test collection, add some initial documents, and build a background $** index.
    assertSetupEnvironment(true, makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}"}));

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 'a', '': 1}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, DedupMultikeyPathsInBulkIndexBuild) {
    // Create the test collection, add some initial documents, and build a foreground $** index.
    const auto initialDocs =
        makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}", "{a: 2, b: [{c: 3}, {d: {e: [4]}}]}"});
    assertSetupEnvironment(false, initialDocs);

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 'a', '': 1}"), RecordId(1)},
                                               {fromjson("{'': 'a', '': 2}"), RecordId(2)},
                                               {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 3}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, DedupMultikeyPathsInBackgroundIndexBuild) {
    // Create the test collection, add some initial documents, and build a background $** index.
    const auto initialDocs =
        makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}", "{a: 2, b: [{c: 3}, {d: {e: [4]}}]}"});
    assertSetupEnvironment(true, initialDocs);

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 'a', '': 1}"), RecordId(1)},
                                               {fromjson("{'': 'a', '': 2}"), RecordId(2)},
                                               {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 3}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, AddAndDedupNewMultikeyPathsOnPostBuildInsertion) {
    // Create the test collection, add some initial documents, and build a $** index.
    assertSetupEnvironment(false, makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}"}));

    // Insert some more documents with a mix of new and duplicate multikey paths.
    assertInsertDocuments(makeDocs({"{a: 2, b: [{c: 3}, {d: {e: [4]}}]}", "{d: {e: {f: [5]}}}"}));

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'd.e.f'}"), kMetadataId},
                                               {fromjson("{'': 'a', '': 1}"), RecordId(1)},
                                               {fromjson("{'': 'a', '': 2}"), RecordId(2)},
                                               {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 3}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)},
                                               {fromjson("{'': 'd.e.f', '': 5}"), RecordId(3)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e", "d.e.f"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, AddAndDedupNewMultikeyPathsOnUpsert) {
    // Create the test collection, add some initial documents, and build a $** index.
    assertSetupEnvironment(false, makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}"}));

    // Upsert some new documents to add new multikey paths.
    assertUpsertDocuments(makeDocs({"{a: 2, b: [{c: 3}, {d: {e: [4]}}]}", "{d: {e: {f: [5]}}}"}));

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'd.e.f'}"), kMetadataId},
                                               {fromjson("{'': 'a', '': 1}"), RecordId(1)},
                                               {fromjson("{'': 'a', '': 2}"), RecordId(2)},
                                               {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 3}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)},
                                               {fromjson("{'': 'd.e.f', '': 5}"), RecordId(3)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e", "d.e.f"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, AddNewMultikeyPathsOnUpdate) {
    // Create the test collection, add some initial documents, and build a $** index.
    assertSetupEnvironment(false, makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}"}));

    // Update the initial document to add a new multikey path.
    assertUpdateDocuments(
        {{fromjson("{_id: 1}"), fromjson("{$push: {b: {$each: [{d: {f: [4]}}, {g: [5]}]}}}")}});

    {
        // Verify that the updated document appears as expected;
        AutoGetCollectionForRead autoColl(opCtx(), kDefaultNSS);
        Snapshotted<BSONObj> result;
        ASSERT(autoColl.getCollection()->findDoc(opCtx(), RecordId(1), &result));
        ASSERT_BSONOBJ_EQ(result.value(),
                          fromjson("{_id:1, a:1, b:[{c:2}, {d:{e:[3]}}, {d:{f:[4]}}, {g:[5]}]}"));
    }

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.f'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.g'}"), kMetadataId},
                                               {fromjson("{'': 'a', '': 1}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.f', '': 4}"), RecordId(1)},
                                               {fromjson("{'': 'b.g', '': 5}"), RecordId(1)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e", "b.d.f", "b.g"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, AddNewMultikeyPathsOnReplacement) {
    // Create the test collection, add some initial documents, and build a $** index.
    assertSetupEnvironment(false, makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}"}));

    // Update the initial document to modify all existing data keys and add a new multikey path.
    assertUpdateDocuments(
        {{fromjson("{_id: 1}"), fromjson("{a: 2, b: [{c: 3}, {d: {e: [4], f: [5]}}]}")}});

    {
        // Verify that the updated document appears as expected;
        AutoGetCollectionForRead autoColl(opCtx(), kDefaultNSS);
        Snapshotted<BSONObj> result;
        ASSERT(autoColl.getCollection()->findDoc(opCtx(), RecordId(1), &result));
        ASSERT_BSONOBJ_EQ(result.value(),
                          fromjson("{_id: 1, a: 2, b: [{c: 3}, {d: {e: [4], f: [5]}}]}"));
    }

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.f'}"), kMetadataId},
                                               {fromjson("{'': 'a', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.f', '': 5}"), RecordId(1)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e", "b.d.f"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, DoNotRemoveMultikeyPathsOnDocDeletion) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}",
                                "{a: 2, b: [{c: 3}, {d: {e: [4]}}]}",
                                "{d: {e: {f: [5]}}}"});
    assertSetupEnvironment(false, docs);

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'd.e.f'}"), kMetadataId},
                                               {fromjson("{'': 'a', '': 1}"), RecordId(1)},
                                               {fromjson("{'': 'a', '': 2}"), RecordId(2)},
                                               {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 3}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)},
                                               {fromjson("{'': 'd.e.f', '': 5}"), RecordId(3)}};

    assertIndexContentsEquals(expectedKeys);

    // Now remove all documents in the collection, and verify that only the multikey paths remain.
    assertRemoveDocuments(docs);

    expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                    {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                    {fromjson("{'': 1, '': 'd.e.f'}"), kMetadataId}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e", "d.e.f"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, OnlyIndexKeyPatternSubTreeInBulkBuild) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}",
                                "{a: 2, b: [{c: 3}, {d: {e: [4]}}]}",
                                "{d: {e: {f: [5]}}}"});
    assertSetupEnvironment(false, docs, fromjson("{'b.d.$**': 1}"));

    // Verify that the data and multikey path keys are present in the expected order. Note that
    // here, as in other tests, the partially-included subpath {b: [{c: 2}]} is projected to
    // {b: [{}]}, resulting in an index key for {b: {}}.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 'b', '': {}}"), RecordId(1)},
                                               {fromjson("{'': 'b', '': {}}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, OnlyIndexKeyPatternSubTreeInBackgroundBuild) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}",
                                "{a: 2, b: [{c: 3}, {d: {e: [4]}}]}",
                                "{d: {e: {f: [5]}}}"});
    assertSetupEnvironment(true, docs, fromjson("{'b.d.$**': 1}"));

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 'b', '': {}}"), RecordId(1)},
                                               {fromjson("{'': 'b', '': {}}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, OnlyIndexIncludedPathsInBulkBuild) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}",
                                "{a: 2, b: [{c: 3}, {d: {e: [4]}}]}",
                                "{d: {e: {f: [5]}}}"});
    assertSetupEnvironment(
        false, docs, fromjson("{'$**': 1}"), fromjson("{b: {d: {e: 1}}, 'd.e': 1}"));

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'd.e.f'}"), kMetadataId},
                                               {fromjson("{'': 'b', '': {}}"), RecordId(1)},
                                               {fromjson("{'': 'b', '': {}}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)},
                                               {fromjson("{'': 'd.e.f', '': 5}"), RecordId(3)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e", "d.e.f"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, OnlyIndexIncludedPathsInBackgroundBuild) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}",
                                "{a: 2, b: [{c: 3}, {d: {e: [4]}}]}",
                                "{d: {e: {f: [5]}}}"});
    assertSetupEnvironment(
        true, docs, fromjson("{'$**': 1}"), fromjson("{b: {d: {e: 1}}, 'd.e': 1}"));

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'd.e.f'}"), kMetadataId},
                                               {fromjson("{'': 'b', '': {}}"), RecordId(1)},
                                               {fromjson("{'': 'b', '': {}}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)},
                                               {fromjson("{'': 'd.e.f', '': 5}"), RecordId(3)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e", "d.e.f"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, OnlyIndexIncludedPathsOnUpdate) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}",
                                "{a: 2, b: [{c: 3}, {d: {e: [4]}}]}",
                                "{d: {e: {f: [5]}}}"});
    assertSetupEnvironment(
        false, docs, fromjson("{'$**': 1}"), fromjson("{b: {d: {e: 1}}, 'd.e': 1}"));

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'd.e.f'}"), kMetadataId},
                                               {fromjson("{'': 'b', '': {}}"), RecordId(1)},
                                               {fromjson("{'': 'b', '': {}}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)},
                                               {fromjson("{'': 'd.e.f', '': 5}"), RecordId(3)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e", "d.e.f"});

    // Now update RecordId(3), adding one new field 'd.e.g' within the included 'd.e' subpath and
    // one new field 'd.h' which lies outside all included subtrees.
    assertUpdateDocuments({{fromjson("{_id: 3}"), fromjson("{$set: {'d.e.g': 6, 'd.h': 7}}")}});

    {
        // Verify that the updated document appears as expected;
        AutoGetCollectionForRead autoColl(opCtx(), kDefaultNSS);
        Snapshotted<BSONObj> result;
        ASSERT(autoColl.getCollection()->findDoc(opCtx(), RecordId(3), &result));
        ASSERT_BSONOBJ_EQ(result.value(), fromjson("{_id: 3, d: {e: {f: [5], g: 6}, h: 7}}"));
    }

    // Verify that only the key {'d.e.g': 6} has been added to the index.
    expectedKeys.push_back({fromjson("{'': 'd.e.g', '': 6}"), RecordId(3)});
    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e", "d.e.f"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, DoNotIndexExcludedPathsInBulkBuild) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}",
                                "{a: 2, b: [{c: 3}, {d: {e: [4]}}]}",
                                "{d: {e: {f: [5]}}}"});
    assertSetupEnvironment(
        false, docs, fromjson("{'$**': 1}"), fromjson("{b: {d: {e: 0}}, 'd.e': 0}"));

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {
        {fromjson("{'': 1, '': 'b'}"), kMetadataId},
        {fromjson("{'': 'a', '': 1}"), RecordId(1)},
        {fromjson("{'': 'a', '': 2}"), RecordId(2)},
        {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
        {fromjson("{'': 'b.c', '': 3}"), RecordId(2)},
        {fromjson("{'': 'b.d', '': {}}"), RecordId(1)},
        {fromjson("{'': 'b.d', '': {}}"), RecordId(2)},
        {fromjson("{'': 'd', '': {}}"), RecordId(3)},
    };

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, DoNotIndexExcludedPathsInBackgroundBuild) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}",
                                "{a: 2, b: [{c: 3}, {d: {e: [4]}}]}",
                                "{d: {e: {f: [5]}}}"});
    assertSetupEnvironment(
        true, docs, fromjson("{'$**': 1}"), fromjson("{b: {d: {e: 0}}, 'd.e': 0}"));

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {
        {fromjson("{'': 1, '': 'b'}"), kMetadataId},
        {fromjson("{'': 'a', '': 1}"), RecordId(1)},
        {fromjson("{'': 'a', '': 2}"), RecordId(2)},
        {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
        {fromjson("{'': 'b.c', '': 3}"), RecordId(2)},
        {fromjson("{'': 'b.d', '': {}}"), RecordId(1)},
        {fromjson("{'': 'b.d', '': {}}"), RecordId(2)},
        {fromjson("{'': 'd', '': {}}"), RecordId(3)},
    };

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, DoNotIndexExcludedPathsOnUpdate) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}",
                                "{a: 2, b: [{c: 3}, {d: {e: [4]}}]}",
                                "{d: {e: {f: [5]}}}"});
    assertSetupEnvironment(
        false, docs, fromjson("{'$**': 1}"), fromjson("{b: {d: {e: 0}}, 'd.e': 0}"));

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 'a', '': 1}"), RecordId(1)},
                                               {fromjson("{'': 'a', '': 2}"), RecordId(2)},
                                               {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 3}"), RecordId(2)},
                                               {fromjson("{'': 'b.d', '': {}}"), RecordId(1)},
                                               {fromjson("{'': 'b.d', '': {}}"), RecordId(2)},
                                               {fromjson("{'': 'd', '': {}}"), RecordId(3)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b"});

    // Now update RecordId(3), adding one new field 'd.e.g' within the excluded 'd.e' subpath and
    // one new field 'd.h' which lies outside all excluded subtrees.
    assertUpdateDocuments({{fromjson("{_id: 3}"), fromjson("{$set: {'d.e.g': 6, 'd.h': 7}}")}});

    {
        // Verify that the updated document appears as expected;
        AutoGetCollectionForRead autoColl(opCtx(), kDefaultNSS);
        Snapshotted<BSONObj> result;
        ASSERT(autoColl.getCollection()->findDoc(opCtx(), RecordId(3), &result));
        ASSERT_BSONOBJ_EQ(result.value(), fromjson("{_id: 3, d: {e: {f: [5], g: 6}, h: 7}}"));
    }

    // The key {d: {}} is no longer present, since it will be replaced by a key for subpath 'd.h'.
    expectedKeys.back() = {fromjson("{'': 'd.h', '': 7}"), RecordId(3)};
    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, IndexIdFieldIfSpecifiedInInclusionProjection) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}",
                                "{a: 2, b: [{c: 3}, {d: {e: [4]}}]}",
                                "{d: {e: {f: [5]}}}"});
    assertSetupEnvironment(
        false, docs, fromjson("{'$**': 1}"), fromjson("{_id: 1, 'b.d.e': 1, 'd.e': 1}"));

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'b.d.e'}"), kMetadataId},
                                               {fromjson("{'': 1, '': 'd.e.f'}"), kMetadataId},
                                               {fromjson("{'': '_id', '': 1}"), RecordId(1)},
                                               {fromjson("{'': '_id', '': 2}"), RecordId(2)},
                                               {fromjson("{'': '_id', '': 3}"), RecordId(3)},
                                               {fromjson("{'': 'b', '': {}}"), RecordId(1)},
                                               {fromjson("{'': 'b', '': {}}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)},
                                               {fromjson("{'': 'd.e.f', '': 5}"), RecordId(3)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b", "b.d.e", "d.e.f"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, IndexIdFieldIfSpecifiedInExclusionProjection) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs({"{a: 1, b: [{c: 2}, {d: {e: [3]}}]}",
                                "{a: 2, b: [{c: 3}, {d: {e: [4]}}]}",
                                "{d: {e: {f: [5]}}}"});
    assertSetupEnvironment(
        false, docs, fromjson("{'$**': 1}"), fromjson("{_id: 1, 'b.d.e': 0, 'd.e': 0}"));

    // Verify that the data and multikey path keys are present in the expected order.
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 1, '': 'b'}"), kMetadataId},
                                               {fromjson("{'': '_id', '': 1}"), RecordId(1)},
                                               {fromjson("{'': '_id', '': 2}"), RecordId(2)},
                                               {fromjson("{'': '_id', '': 3}"), RecordId(3)},
                                               {fromjson("{'': 'a', '': 1}"), RecordId(1)},
                                               {fromjson("{'': 'a', '': 2}"), RecordId(2)},
                                               {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 3}"), RecordId(2)},
                                               {fromjson("{'': 'b.d', '': {}}"), RecordId(1)},
                                               {fromjson("{'': 'b.d', '': {}}"), RecordId(2)},
                                               {fromjson("{'': 'd', '': {}}"), RecordId(3)}};

    assertIndexContentsEquals(expectedKeys);
    assertMultikeyPathSetEquals({"b"});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, DoNotMarkAsMultikeyIfNoArraysInBulkBuild) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs(
        {"{a: 1, b: {c: 2, d: {e: 3}}}", "{a: 2, b: {c: 3, d: {e: 4}}}", "{d: {e: {f: 5}}}"});
    assertSetupEnvironment(false, docs, fromjson("{'$**': 1}"));

    // Verify that the data keys are present in the expected order, and the index is NOT multikey.
    const bool expectIndexIsMultikey = false;
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 'a', '': 1}"), RecordId(1)},
                                               {fromjson("{'': 'a', '': 2}"), RecordId(2)},
                                               {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 3}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)},
                                               {fromjson("{'': 'd.e.f', '': 5}"), RecordId(3)}};

    assertIndexContentsEquals(expectedKeys, expectIndexIsMultikey);
    assertMultikeyPathSetEquals({});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, DoNotMarkAsMultikeyIfNoArraysInBackgroundBuild) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs(
        {"{a: 1, b: {c: 2, d: {e: 3}}}", "{a: 2, b: {c: 3, d: {e: 4}}}", "{d: {e: {f: 5}}}"});
    assertSetupEnvironment(true, docs, fromjson("{'$**': 1}"));

    // Verify that the data keys are present in the expected order, and the index is NOT multikey.
    const bool expectIndexIsMultikey = false;
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 'a', '': 1}"), RecordId(1)},
                                               {fromjson("{'': 'a', '': 2}"), RecordId(2)},
                                               {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 3}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)},
                                               {fromjson("{'': 'd.e.f', '': 5}"), RecordId(3)}};

    assertIndexContentsEquals(expectedKeys, expectIndexIsMultikey);
    assertMultikeyPathSetEquals({});
}

TEST_F(WildcardMultikeyPersistenceTestFixture, IndexShouldBecomeMultikeyIfArrayIsCreatedByUpdate) {
    // Create the test collection, add some initial documents, and build a $** index.
    const auto docs = makeDocs(
        {"{a: 1, b: {c: 2, d: {e: 3}}}", "{a: 2, b: {c: 3, d: {e: 4}}}", "{d: {e: {f: 5}}}"});
    assertSetupEnvironment(false, docs, fromjson("{'$**': 1}"));

    // Verify that the data keys are present in the expected order, and the index is NOT multikey.
    bool expectIndexIsMultikey = false;
    std::vector<IndexKeyEntry> expectedKeys = {{fromjson("{'': 'a', '': 1}"), RecordId(1)},
                                               {fromjson("{'': 'a', '': 2}"), RecordId(2)},
                                               {fromjson("{'': 'b.c', '': 2}"), RecordId(1)},
                                               {fromjson("{'': 'b.c', '': 3}"), RecordId(2)},
                                               {fromjson("{'': 'b.d.e', '': 3}"), RecordId(1)},
                                               {fromjson("{'': 'b.d.e', '': 4}"), RecordId(2)},
                                               {fromjson("{'': 'd.e.f', '': 5}"), RecordId(3)}};

    assertIndexContentsEquals(expectedKeys, expectIndexIsMultikey);
    assertMultikeyPathSetEquals({});

    // Now perform an update that introduces an array into one of the documents...
    assertUpdateDocuments({{fromjson("{_id: 1}"), fromjson("{$set: {g: {h: []}}}")}});

    // ... and confirm that this has caused the index to become multikey.
    expectIndexIsMultikey = true;
    expectedKeys.insert(expectedKeys.begin(), {fromjson("{'': 1, '': 'g.h'}"), kMetadataId});
    expectedKeys.push_back({fromjson("{'': 'g.h', '': undefined}"), RecordId(1)});

    assertIndexContentsEquals(expectedKeys, expectIndexIsMultikey);
    assertMultikeyPathSetEquals({"g.h"});
}

}  // namespace
}  // namespace mongo
