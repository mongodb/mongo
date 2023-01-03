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
#include "mongo/db/index/columns_access_method.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

using namespace mongo;

namespace {
/**
 * This test fixture provides methods to insert documents into a column store index and to scan a
 * column from the index via its IndexAccessMethod.
 */
class ColumnsAccessMethodTest : public CatalogTestFixture {
public:
protected:
    ColumnsAccessMethodTest()
        : _pooledBuilder(KeyString::HeapBuilder::kHeapAllocatorDefaultBytes) {}

    const CollectionPtr& collection() const {
        return _coll->getCollection();
    }

    void columnStoreIndexInsert(const BSONObj& obj) {
        int64_t keysInserted;
        int64_t rowId = ++_lastRowId;

        WriteUnitOfWork wuow(operationContext());
        Status status = _accessMethod->insert(
            operationContext(),
            _pooledBuilder,
            collection(),
            std::vector<BsonRecord>{BsonRecord{RecordId(rowId), Timestamp(1, rowId), &obj}},
            {},
            &keysInserted);
        ASSERT(status.isOK());
        wuow.commit();
    }

    void openCursor(StringData path) {
        _cursor = _accessMethod->storage()->newCursor(operationContext(), path);
        _cursorNeedsSeek = true;
    }

    bool advanceDocument() {
        if (_cursorNeedsSeek) {
            _cell = _cursor->seekAtOrPast(ColumnStore::kNullRowId);
            _cursorNeedsSeek = false;
        } else {
            _cell = _cursor->next();
        }

        if (_cell) {
            _splitCellView = SplitCellView::parse(_cell->value);
            return true;
        } else {
            return false;
        }
    }

    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(storageInterface()->createCollection(operationContext(), _nss, {}));
        _coll.emplace(operationContext(), _nss, MODE_X);

        auto ice =
            createIndex(fromjson("{v: 2, name: 'columnstore', key: {'$**': 'columnstore'}}"));
        _accessMethod = dynamic_cast<ColumnStoreAccessMethod*>(ice->accessMethod());
        ASSERT(_accessMethod);

        _lastRowId = 0;
    }

    void tearDown() override {
        _cursor.reset();
        _coll.reset();
        _accessMethod = nullptr;
        _pooledBuilder.freeUnused();
        CatalogTestFixture::tearDown();
    }

    /**
     * Test that the ColumnStoreAccessMethod correctly reads the data for a given 'path' that is
     * expected to have only NumberInt32 values. The test iterates each "cell" (which contains all
     * values at the target path for one document) and asserts that the values in the cell match an
     * expected list of values.
     */
    void checkIntsAtPath(StringData path, const std::vector<std::vector<int32_t>>& expectedInts) {
        openCursor(path);
        for (auto&& expectedCell : expectedInts) {
            ASSERT(advanceDocument());

            // Convert the expected elements to "cell" format.
            BSONArrayBuilder bab;
            for (auto val : expectedCell) {
                bab.append(val);
            }
            auto elements = bab.done();

            std::vector<BSONElement> elementsVector;
            elements.elems(elementsVector);
            column_keygen::UnencodedCellView unencodedCell{
                elementsVector, "", false, false, false, false};

            BufBuilder encodedExpectedInts;
            writeEncodedCell(unencodedCell, &encodedExpectedInts);

            // The cell-formatted array of ints should match the values portion (i.e., not including
            // the flags and arrayInfo) of the cell retrieved from the column store. We compare the
            // hex-encoded bytes because they produce more helpful debug output when there is a
            // mismatch.
            StringData observedCell(_splitCellView.firstValuePtr, _splitCellView.arrInfo.rawData());
            ASSERT_EQ(hexblob::encode(encodedExpectedInts.buf(), encodedExpectedInts.len()),
                      hexblob::encode(observedCell));
        }
        ASSERT(!advanceDocument());
    }

private:
    const IndexCatalogEntry* createIndex(BSONObj spec) {
        WriteUnitOfWork wuow(operationContext());
        auto* indexCatalog = _coll->getWritableCollection(operationContext())->getIndexCatalog();
        uassertStatusOK(indexCatalog->createIndexOnEmptyCollection(
            operationContext(), _coll->getWritableCollection(operationContext()), spec));
        wuow.commit();

        return indexCatalog->getEntry(indexCatalog->findIndexByName(
            operationContext(), spec.getStringField(IndexDescriptor::kIndexNameFieldName)));
    }

    NamespaceString _nss{"testDB.columns"};
    boost::optional<AutoGetCollection> _coll;
    ColumnStoreAccessMethod* _accessMethod;

    int64_t _lastRowId;
    SharedBufferFragmentBuilder _pooledBuilder;

    std::unique_ptr<ColumnStore::ColumnCursor> _cursor;
    bool _cursorNeedsSeek;
    boost::optional<FullCellView> _cell;
    SplitCellView _splitCellView;
};

TEST_F(ColumnsAccessMethodTest, Dollars) {
    columnStoreIndexInsert(BSON("$a" << BSON("b" << 1) << "b" << BSON("$a" << 10) << "$$" << 100));
    columnStoreIndexInsert(BSON("$a" << BSON_ARRAY(BSON("b" << 2) << BSON("b" << 3)) << "b"
                                     << BSON_ARRAY(BSON("$a" << 11) << BSON("$a" << 12))));
    columnStoreIndexInsert(BSON("$a"
                                << BSON_ARRAY(BSON("b" << 4) << BSON("c" << -1) << BSON("b" << 5))
                                << "b" << BSON_ARRAY(BSON("$a" << 13) << BSON("$a" << 14))));

    checkIntsAtPath("$$", {{100}});
    checkIntsAtPath("$a.b", {{1}, {2, 3}, {4, 5}});
    checkIntsAtPath("b.$a", {{10}, {11, 12}, {13, 14}});
}

TEST_F(ColumnsAccessMethodTest, EmptyFieldNames) {
    columnStoreIndexInsert(BSON("" << 1));
    columnStoreIndexInsert(BSON("" << BSON("" << 2)));
    columnStoreIndexInsert(BSON("" << BSON("" << BSON("" << 3))));
    columnStoreIndexInsert(
        BSON("" << BSON_ARRAY(BSON("" << BSON("" << 4)) << BSON("" << BSON("" << 5)))));

    checkIntsAtPath("", {{1}, {}, {}, {}});
    checkIntsAtPath(".", {{2}, {}, {}});
    checkIntsAtPath("..", {{3}, {4, 5}});
}

TEST_F(ColumnsAccessMethodTest, Numeric) {
    columnStoreIndexInsert(BSON("0" << BSON_ARRAY(10 << 11)));
    columnStoreIndexInsert(BSON("0" << BSON("0" << 20 << "1" << 21)));

    checkIntsAtPath("0", {{10, 11}, {}});
    checkIntsAtPath("0.0", {{20}});
    checkIntsAtPath("0.1", {{21}});
}
}  // namespace
