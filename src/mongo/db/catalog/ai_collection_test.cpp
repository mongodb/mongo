/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include <random>

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class AICollectionTest : public CatalogTestFixture {
public:
    void createCollection(const NamespaceString& collectionNamespace) {
        auto opCtx = operationContext();

        CollectionOptions options;

        AutoGetCollection autoColl(opCtx, collectionNamespace, MODE_IX);
        Database* db = autoColl.ensureDbExists();
        WriteUnitOfWork wuow(opCtx);
        ASSERT(db->createCollection(opCtx, collectionNamespace, options));
        wuow.commit();
    }

    void createIndexOnEmptyCollection(const NamespaceString& collectionNamespace,
                                      BSONObj indexKey,
                                      StringData indexName) {
        OperationContext* opCtx = operationContext();
        BSONObj indexSpec = makeIndexSpec(indexKey, indexName);

        AutoGetCollection autoColl{opCtx, collectionNamespace, MODE_X};
        WriteUnitOfWork wuow{opCtx};
        Collection* collection = autoColl.getWritableCollection();
        IndexCatalog* indexCatalog = collection->getIndexCatalog();
        ASSERT_OK(indexCatalog->createIndexOnEmptyCollection(opCtx, collection, indexSpec));
        wuow.commit();
    }

    void insertDocuments(const NamespaceString& collectionNamespace,
                         const std::vector<BSONObj>& docs) {
        OperationContext* opCtx = operationContext();

        std::vector<InsertStatement> statements{};
        statements.reserve(docs.size());
        for (const BSONObj& doc : docs) {
            statements.emplace_back(doc);
        }

        AutoGetCollection autoColl{opCtx, collectionNamespace, MODE_IX};
        const CollectionPtr& coll = autoColl.getCollection();

        WriteUnitOfWork wuow{opCtx};
        ASSERT_OK(coll->insertDocuments(opCtx, statements.begin(), statements.end(), nullptr));
        wuow.commit();
    }

    BSONObj makeIndexSpec(BSONObj indexKey, StringData indexName) {
        return BSON("v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << indexKey << "name"
                        << indexName);
    }
};

class DataGenerator {
public:
    DataGenerator() {
        _alphabet.reserve(26);
        for (char c = 97; c < 123; ++c) {
            _alphabet.push_back(c);
        }

        std::random_device rd;
        _rnd = std::mt19937{rd()};
        _charDistribution = std::uniform_int_distribution<size_t>{0, _alphabet.size() - 1};
    }

    std::vector<BSONObj> generateDocuments(size_t minLength, size_t maxLength, size_t size) {
        auto data = randomStrings(5, 25, size);

        std::vector<BSONObj> docs{};
        docs.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            BSONObj doc = BSON(GENOID << "index" << (int)i << "data" << data[i].c_str());
            docs.push_back(doc);
        }

        return docs;
    }

    std::vector<std::string> randomStrings(size_t minLength, size_t maxLength, size_t count) {
        std::vector<std::string> result;
        result.reserve(count);
        std::uniform_int_distribution<size_t> lengthDistribution(minLength, maxLength);

        for (size_t i = 0; i < count; ++i) {
            size_t length = lengthDistribution(_rnd);
            result.emplace_back(randomString(length));
        }

        return result;
    }

    std::string randomString(size_t length) {
        std::string result{};
        result.reserve(length);

        for (size_t i = 0; i < length; ++i) {
            result.push_back(randomChar());
        }

        return result;
    }

    char randomChar() {
        return _alphabet[_charDistribution(_rnd)];
    }

private:
    std::string _alphabet;

    std::mt19937 _rnd;
    std::uniform_int_distribution<size_t> _charDistribution;
};

class IndexAccessor {
public:
    IndexAccessor(OperationContext* opCtx,
                  const NamespaceString& collectionNamespace,
                  StringData indexName)
        : _opCtx{opCtx}, _autoColl{opCtx, collectionNamespace} {
        _indexDescriptor = _autoColl->getIndexCatalog()->findIndexByName(opCtx, indexName);
        ASSERT(_indexDescriptor);

        _recordStore = _autoColl.getCollection()->getRecordStore();
        ASSERT(_recordStore);
    }

    std::vector<BSONObj> findAll(const BSONObj& key) {
        IndexCatalogEntry* entry = _indexDescriptor->getEntry();
        ASSERT(entry);
        IndexAccessMethod* accessMethod = entry->accessMethod();
        ASSERT(accessMethod);
        SortedDataInterface* sortedDataInterface = accessMethod->getSortedDataInterface();
        ASSERT(sortedDataInterface);
        std::unique_ptr<SortedDataInterface::Cursor> indexCursor =
            sortedDataInterface->newCursor(_opCtx);
        ASSERT(indexCursor);

        indexCursor->setEndPosition(key, /* inclusive */ true);
        KeyString::Value keyString = IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
            key,
            sortedDataInterface->getKeyStringVersion(),
            sortedDataInterface->getOrdering(),
            /*forward*/ true,
            /*inclusive*/ true);
        boost::optional<IndexKeyEntry> keyEntry = indexCursor->seek(keyString);

        std::unique_ptr<SeekableRecordCursor> recordCursor = _recordStore->getCursor(_opCtx);
        std::vector<BSONObj> result{};


        while (keyEntry) {
            boost::optional<Record> record = recordCursor->seekExact(keyEntry->loc);
            ASSERT(record);
            result.emplace_back(record->data.releaseToBson());
            keyEntry = indexCursor->next();
        }
        return result;
    }

private:
    OperationContext* _opCtx;
    AutoGetCollectionForRead _autoColl;
    RecordStore* _recordStore{};
    const IndexDescriptor* _indexDescriptor{};
};

TEST_F(AICollectionTest, InsertDocument) {
    auto opCtx = operationContext();
    NamespaceString nss("test.t");

    // Create Collection
    {
        CollectionOptions options;

        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        Database* db = autoColl.ensureDbExists();
        WriteUnitOfWork wuow(opCtx);
        Collection* coll = db->createCollection(opCtx, nss, options);
        ASSERT(coll);
        wuow.commit();
    }

    // Insert Document
    {
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        const CollectionPtr& coll = autoColl.getCollection();
        WriteUnitOfWork wuow(opCtx);
        BSONObj doc = BSON("_id" << 1 << "message"
                                 << "Hello World");
        InsertStatement statement{doc};
        ASSERT_OK(coll->insertDocument(opCtx, statement, nullptr));
        wuow.commit();
    }

    // Uncommitted insert
    {
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        const CollectionPtr& coll = autoColl.getCollection();
        WriteUnitOfWork wuow(opCtx);
        BSONObj doc = BSON("_id" << 2 << "message"
                                 << "Good bye");
        InsertStatement statement{doc};
        ASSERT_OK(coll->insertDocument(opCtx, statement, nullptr));
    }

    // Read Document
    {
        AutoGetCollectionForRead autoColl(opCtx, nss);
        const CollectionPtr& coll = autoColl.getCollection();
        RecordStore* recordStore = coll->getRecordStore();
        std::unique_ptr<SeekableRecordCursor> cursor = recordStore->getCursor(opCtx);
        RecordId recordId{1};
        boost::optional<Record> record = cursor->seekExact(recordId);
        ASSERT_TRUE(record);
        LOGV2_INFO(11111, "record1", "data"_attr = record->data.toBson());
    }

    // Read Uncommitted Document
    {
        AutoGetCollectionForRead autoColl(opCtx, nss);
        const CollectionPtr& coll = autoColl.getCollection();
        RecordStore* recordStore = coll->getRecordStore();
        std::unique_ptr<SeekableRecordCursor> cursor = recordStore->getCursor(opCtx);
        RecordId recordId{2};
        boost::optional<Record> record = cursor->seekExact(recordId);
        ASSERT_FALSE(record);
    }

    // Read Index Count
    {
        AutoGetCollectionForRead autoColl(opCtx, nss);
        const CollectionPtr& coll = autoColl.getCollection();
        ASSERT_EQ(1, coll->getTotalIndexCount());
    }
}

TEST_F(AICollectionTest, MakeDatabase) {
    OperationContext* opCtx = operationContext();
    NamespaceString nss("test.coll");
    createCollection(nss);

    StringData indexOnDataName{"indexOnData"};

    // Create index
    {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        WriteUnitOfWork wuow(opCtx);
        Collection* collection = autoColl.getWritableCollection();
        IndexCatalog* indexCatalog = collection->getIndexCatalog();
        BSONObj indexSpec = makeIndexSpec(BSON("data" << 1), indexOnDataName);
        ASSERT_OK(indexCatalog->createIndexOnEmptyCollection(opCtx, collection, indexSpec));
        wuow.commit();
    }

    constexpr size_t numRecords = 10;
    DataGenerator gen{};
    auto docs = gen.generateDocuments(5, 25, numRecords);
    insertDocuments(nss, docs);

    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx, /*stableCheckpoint*/ false);

    // Read documents
    {
        AutoGetCollectionForRead autoColl(opCtx, nss);
        const CollectionPtr& coll = autoColl.getCollection();

        RecordStore* recordStore = coll->getRecordStore();
        std::unique_ptr<SeekableRecordCursor> cursor = recordStore->getCursor(opCtx);

        boost::optional<Record> record;

        while (record = cursor->next(), record) {
            BSONObj doc = record->data.toBson();
            ASSERT_TRUE(doc.hasField("index"));
            ASSERT_TRUE(doc.hasField("data"));
            int index = doc["index"].numberInt();
            ASSERT_EQ(docs[index]["data"].str(), doc["data"].str());
        }
    }

    // Read by index
    {
        AutoGetCollectionForRead autoColl(opCtx, nss);
        const CollectionPtr& coll = autoColl.getCollection();
        const IndexDescriptor* indexDesc =
            coll->getIndexCatalog()->findIndexByName(opCtx, indexOnDataName);
        ASSERT(indexDesc);
        IndexCatalogEntry* indexEntry = indexDesc->getEntry();
        ASSERT(indexEntry);
        ASSERT_TRUE(indexEntry->isReadyInMySnapshot(opCtx));
        IndexAccessMethod* accessMethod = indexEntry->accessMethod();
        ASSERT(accessMethod);
        SortedDataInterface* sortedDataInterface = accessMethod->getSortedDataInterface();
        ASSERT(sortedDataInterface);
        std::unique_ptr<SortedDataInterface::Cursor> cursor = sortedDataInterface->newCursor(opCtx);
        ASSERT(cursor);
        std::string dataKeyValue = docs[docs.size() * 3 / 4]["data"].str();
        BSONObj key = BSON("data" << dataKeyValue.c_str());
        cursor->setEndPosition(key, /* inclusive */ true);
        KeyString::Value keyString = IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
            key,
            sortedDataInterface->getKeyStringVersion(),
            sortedDataInterface->getOrdering(),
            /*forward*/ true,
            /*inclusive*/ true);
        boost::optional<IndexKeyEntry> indexKeyEntry = cursor->seek(keyString);
        ASSERT(indexKeyEntry);

        RecordStore* recordStore = coll->getRecordStore();
        std::unique_ptr<SeekableRecordCursor> recordCursor = recordStore->getCursor(opCtx);
        boost::optional<Record> record = recordCursor->seekExact(indexKeyEntry->loc);
        ASSERT(record);
        BSONObj obj = record->data.toBson();
        ASSERT_EQ(dataKeyValue, obj["data"].str());
    }
}

TEST_F(AICollectionTest, IndexAccessor) {
    OperationContext* opCtx = operationContext();
    NamespaceString nss("test.coll");
    StringData indexOnDataName{"indexOnData"};
    createCollection(nss);
    createIndexOnEmptyCollection(nss, BSON("data" << 1), indexOnDataName);

    constexpr size_t numRecords = 1000;
    DataGenerator gen{};
    auto docs = gen.generateDocuments(5, 25, numRecords);
    insertDocuments(nss, docs);

    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx, /*stableCheckpoint*/ false);

    IndexAccessor index{opCtx, nss, indexOnDataName};

    std::string dataKeyValue = docs[docs.size() * 5 / 7]["data"].str();
    auto result = index.findAll(BSON("data" << dataKeyValue.c_str()));
    ASSERT_LTE(1, result.size());
    for (const auto& doc : result) {
        ASSERT_EQ(dataKeyValue, doc["data"].str());
    }
}
}  // namespace mongo
