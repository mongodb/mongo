/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/idl/server_parameter_test_util.h"

namespace mongo {
namespace {
class IndexBuilderInterceptorTest : public CatalogTestFixture {
protected:
    const IndexCatalogEntry* createIndex(BSONObj spec) {
        WriteUnitOfWork wuow(operationContext());
        auto* indexCatalog = _coll->getWritableCollection(operationContext())->getIndexCatalog();
        uassertStatusOK(indexCatalog->createIndexOnEmptyCollection(
            operationContext(), _coll->getWritableCollection(operationContext()), spec));
        wuow.commit();

        return indexCatalog->getEntry(indexCatalog->findIndexByName(
            operationContext(), spec.getStringField(IndexDescriptor::kIndexNameFieldName)));
    }

    std::unique_ptr<IndexBuildInterceptor> createIndexBuildInterceptor(BSONObj spec) {
        return std::make_unique<IndexBuildInterceptor>(operationContext(),
                                                       createIndex(std::move(spec)));
    }

    std::unique_ptr<TemporaryRecordStore> getSideWritesTable(
        std::unique_ptr<IndexBuildInterceptor> interceptor) {
        // In order to get access to the interceptor's side writes table, we have to mark the table
        // as permanent and then destroy the interceptor.
        interceptor->keepTemporaryTables();
        auto sideWritesIdent = interceptor->getSideWritesTableIdent();
        interceptor.reset();

        return operationContext()
            ->getServiceContext()
            ->getStorageEngine()
            ->makeTemporaryRecordStoreFromExistingIdent(operationContext(), sideWritesIdent);
    }

    std::vector<BSONObj> getSideWritesTableContents(
        std::unique_ptr<IndexBuildInterceptor> interceptor) {
        auto table = getSideWritesTable(std::move(interceptor));

        std::vector<BSONObj> contents;
        auto cursor = table->rs()->getCursor(operationContext());
        while (auto record = cursor->next()) {
            contents.push_back(record->data.toBson().getOwned());
        }
        return contents;
    }

    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(storageInterface()->createCollection(operationContext(), _nss, {}));
        _coll.emplace(operationContext(), _nss, MODE_X);
    }

    void tearDown() override {
        _coll.reset();
        CatalogTestFixture::tearDown();
    }

private:
    NamespaceString _nss{"testDB.interceptor"};
    boost::optional<AutoGetCollection> _coll;
};

TEST_F(IndexBuilderInterceptorTest, SingleInsertIsSavedToSideWritesTable) {
    auto interceptor = createIndexBuildInterceptor(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));

    KeyString::HeapBuilder ksBuilder(KeyString::Version::kLatestVersion);
    ksBuilder.appendNumberLong(10);
    KeyString::Value keyString(ksBuilder.release());

    WriteUnitOfWork wuow(operationContext());
    int64_t numKeys = 0;
    ASSERT_OK(interceptor->sideWrite(
        operationContext(), {keyString}, {}, {}, IndexBuildInterceptor::Op::kInsert, &numKeys));
    ASSERT_EQ(1, numKeys);
    wuow.commit();

    BufBuilder bufBuilder;
    keyString.serialize(bufBuilder);
    BSONBinData serializedKeyString(bufBuilder.buf(), bufBuilder.len(), BinDataGeneral);

    auto sideWrites = getSideWritesTableContents(std::move(interceptor));
    ASSERT_EQ(1, sideWrites.size());
    ASSERT_BSONOBJ_EQ(BSON("op"
                           << "i"
                           << "key" << serializedKeyString),
                      sideWrites[0]);
}


TEST_F(IndexBuilderInterceptorTest, SingleColumnInsertIsSavedToSideWritesTable) {
    RAIIServerParameterControllerForTest controller("featureFlagColumnstoreIndexes", true);
    auto interceptor = createIndexBuildInterceptor(
        fromjson("{v: 2, name: 'columnstore', key: {'$**': 'columnstore'}}"));

    PathCellSet columnKeys;
    columnKeys.emplace_back(std::make_tuple("path", "cell", RecordId(1)));

    WriteUnitOfWork wuow(operationContext());
    int64_t numKeys = 0;
    ASSERT_OK(interceptor->sideWrite(
        operationContext(), columnKeys, IndexBuildInterceptor::Op::kInsert, &numKeys));
    ASSERT_EQ(1, numKeys);
    wuow.commit();

    BSONObjBuilder builder;
    RecordId(1).serializeToken("rid", &builder);
    BSONObj obj = builder.obj();
    BSONElement elem = obj["rid"];

    auto sideWrites = getSideWritesTableContents(std::move(interceptor));
    ASSERT_EQ(1, sideWrites.size());
    ASSERT_BSONOBJ_EQ(BSON("rid" << elem << "op"
                                 << "i"
                                 << "path"
                                 << "path"
                                 << "cell"
                                 << "cell"),
                      sideWrites[0]);
}

TEST_F(IndexBuilderInterceptorTest, SingleColumnDeleteIsSavedToSideWritesTable) {
    RAIIServerParameterControllerForTest controller("featureFlagColumnstoreIndexes", true);
    auto interceptor = createIndexBuildInterceptor(
        fromjson("{v: 2, name: 'columnstore', key: {'$**': 'columnstore'}}"));

    PathCellSet columnKeys;
    columnKeys.emplace_back(std::make_tuple("path", "", RecordId(1)));

    WriteUnitOfWork wuow(operationContext());
    int64_t numKeys = 0;
    ASSERT_OK(interceptor->sideWrite(
        operationContext(), columnKeys, IndexBuildInterceptor::Op::kDelete, &numKeys));
    ASSERT_EQ(1, numKeys);
    wuow.commit();

    BSONObjBuilder builder;
    RecordId(1).serializeToken("rid", &builder);
    BSONObj obj = builder.obj();
    BSONElement elem = obj["rid"];

    auto sideWrites = getSideWritesTableContents(std::move(interceptor));
    ASSERT_EQ(1, sideWrites.size());
    ASSERT_BSONOBJ_EQ(BSON("rid" << elem << "op"
                                 << "d"
                                 << "path"
                                 << "path"
                                 << "cell"
                                 << ""),
                      sideWrites[0]);
}

TEST_F(IndexBuilderInterceptorTest, SingleColumnUpdateIsSavedToSideWritesTable) {
    RAIIServerParameterControllerForTest controller("featureFlagColumnstoreIndexes", true);
    auto interceptor = createIndexBuildInterceptor(
        fromjson("{v: 2, name: 'columnstore', key: {'$**': 'columnstore'}}"));

    // create path + cell + rid
    PathCellSet columnKeys;
    columnKeys.emplace_back(std::make_tuple("path", "cell", RecordId(1)));

    WriteUnitOfWork wuow(operationContext());
    int64_t numKeys = 0;
    ASSERT_OK(interceptor->sideWrite(
        operationContext(), columnKeys, IndexBuildInterceptor::Op::kUpdate, &numKeys));
    ASSERT_EQ(1, numKeys);
    wuow.commit();

    BSONObjBuilder builder;
    RecordId(1).serializeToken("rid", &builder);
    BSONObj obj = builder.obj();
    BSONElement elem = obj["rid"];

    auto sideWrites = getSideWritesTableContents(std::move(interceptor));
    ASSERT_EQ(1, sideWrites.size());
    ASSERT_BSONOBJ_EQ(BSON("rid" << elem << "op"
                                 << "u"
                                 << "path"
                                 << "path"
                                 << "cell"
                                 << "cell"),
                      sideWrites[0]);
}

TEST_F(IndexBuilderInterceptorTest, MultipleColumnInsertsAreSavedToSideWritesTable) {
    RAIIServerParameterControllerForTest controller("featureFlagColumnstoreIndexes", true);
    auto interceptor = createIndexBuildInterceptor(
        fromjson("{v: 2, name: 'columnstore', key: {'$**': 'columnstore'}}"));

    PathCellSet columnKeys;
    columnKeys.emplace_back(std::make_tuple("path", "cell", RecordId(1)));
    columnKeys.emplace_back(std::make_tuple("path1", "cell1", RecordId(1)));
    columnKeys.emplace_back(std::make_tuple("path2", "cell2", RecordId(2)));
    columnKeys.emplace_back(std::make_tuple("path3", "cell3", RecordId(2)));

    WriteUnitOfWork wuow(operationContext());
    int64_t numKeys = 0;

    ASSERT_OK(interceptor->sideWrite(
        operationContext(), columnKeys, IndexBuildInterceptor::Op::kInsert, &numKeys));
    ASSERT_EQ(4, numKeys);
    wuow.commit();

    BSONObjBuilder builder;
    RecordId(1).serializeToken("rid", &builder);
    BSONObj obj = builder.obj();
    BSONElement elem1 = obj["rid"];

    BSONObjBuilder builder2;
    RecordId(2).serializeToken("rid", &builder2);
    BSONObj obj2 = builder2.obj();
    BSONElement elem2 = obj2["rid"];

    auto sideWrites = getSideWritesTableContents(std::move(interceptor));
    ASSERT_EQ(4, sideWrites.size());
    ASSERT_BSONOBJ_EQ(BSON("rid" << elem1 << "op"
                                 << "i"
                                 << "path"
                                 << "path"
                                 << "cell"
                                 << "cell"),
                      sideWrites[0]);
    ASSERT_BSONOBJ_EQ(BSON("rid" << elem1 << "op"
                                 << "i"
                                 << "path"
                                 << "path1"
                                 << "cell"
                                 << "cell1"),
                      sideWrites[1]);
    ASSERT_BSONOBJ_EQ(BSON("rid" << elem2 << "op"
                                 << "i"
                                 << "path"
                                 << "path2"
                                 << "cell"
                                 << "cell2"),
                      sideWrites[2]);
    ASSERT_BSONOBJ_EQ(BSON("rid" << elem2 << "op"
                                 << "i"
                                 << "path"
                                 << "path3"
                                 << "cell"
                                 << "cell3"),
                      sideWrites[3]);
}

TEST_F(IndexBuilderInterceptorTest, MultipleColumnSideWritesAreSavedToSideWritesTable) {
    RAIIServerParameterControllerForTest controller("featureFlagColumnstoreIndexes", true);
    auto interceptor = createIndexBuildInterceptor(
        fromjson("{v: 2, name: 'columnstore', key: {'$**': 'columnstore'}}"));

    WriteUnitOfWork wuow(operationContext());
    int64_t numKeys = 0;

    PathCellSet columnKeys;
    columnKeys.emplace_back(std::make_tuple("path", "cell", RecordId(1)));
    ASSERT_OK(interceptor->sideWrite(
        operationContext(), columnKeys, IndexBuildInterceptor::Op::kInsert, &numKeys));
    ASSERT_EQ(1, numKeys);

    PathCellSet columnKeys2;
    columnKeys2.emplace_back(std::make_tuple("path", "", RecordId(1)));
    ASSERT_OK(interceptor->sideWrite(
        operationContext(), columnKeys2, IndexBuildInterceptor::Op::kDelete, &numKeys));
    ASSERT_EQ(1, numKeys);


    PathCellSet columnKeys3;
    columnKeys3.emplace_back(std::make_tuple("path1", "cell1", RecordId(2)));
    ASSERT_OK(interceptor->sideWrite(
        operationContext(), columnKeys3, IndexBuildInterceptor::Op::kUpdate, &numKeys));
    ASSERT_EQ(1, numKeys);

    PathCellSet columnKeys4;
    columnKeys4.emplace_back(std::make_tuple("path2", "cell2", RecordId(2)));
    ASSERT_OK(interceptor->sideWrite(
        operationContext(), columnKeys4, IndexBuildInterceptor::Op::kInsert, &numKeys));
    ASSERT_EQ(1, numKeys);
    wuow.commit();

    BSONObjBuilder builder;
    RecordId(1).serializeToken("rid", &builder);
    BSONObj obj = builder.obj();
    BSONElement elem1 = obj["rid"];

    BSONObjBuilder builder2;
    RecordId(2).serializeToken("rid", &builder2);
    BSONObj obj2 = builder2.obj();
    BSONElement elem2 = obj2["rid"];

    auto sideWrites = getSideWritesTableContents(std::move(interceptor));
    ASSERT_EQ(4, sideWrites.size());
    ASSERT_BSONOBJ_EQ(BSON("rid" << elem1 << "op"
                                 << "i"
                                 << "path"
                                 << "path"
                                 << "cell"
                                 << "cell"),
                      sideWrites[0]);
    ASSERT_BSONOBJ_EQ(BSON("rid" << elem1 << "op"
                                 << "d"
                                 << "path"
                                 << "path"
                                 << "cell"
                                 << ""),
                      sideWrites[1]);
    ASSERT_BSONOBJ_EQ(BSON("rid" << elem2 << "op"
                                 << "u"
                                 << "path"
                                 << "path1"
                                 << "cell"
                                 << "cell1"),
                      sideWrites[2]);
    ASSERT_BSONOBJ_EQ(BSON("rid" << elem2 << "op"
                                 << "i"
                                 << "path"
                                 << "path2"
                                 << "cell"
                                 << "cell2"),
                      sideWrites[3]);
}

}  // namespace
}  // namespace mongo
