/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/catalog/index_catalog_impl.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest;

namespace mongo {
namespace {

using IndexCatalogImplTest = CatalogTestFixture;

}

TEST_F(IndexCatalogImplTest, IndexRebuildHandlesTransientEbusy) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("IndexCatalogImplTest.RebuildForRecovery");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));
    AutoGetCollection autoColl(operationContext(), nss, MODE_X);
    WriteUnitOfWork wuow(operationContext());
    auto collWriter = autoColl.getWritableCollection(operationContext());

    // Ensure we have 1 in-progress index build.
    IndexSpec spec;
    spec.version(1).name("x_1").addKeys(BSON("x" << 1));
    auto desc = IndexDescriptor(IndexNames::BTREE, spec.toBSON());
    ASSERT_OK(collWriter->prepareForIndexBuild(operationContext(), &desc, boost::none, false));
    IndexCatalogEntry* entry = collWriter->getIndexCatalog()->createIndexEntry(
        operationContext(), collWriter, std::move(desc), CreateIndexEntryFlags::kNone);
    ASSERT_FALSE(entry->isReady());

    // Make the index's underlying table busy.
    //
    // In production there may be transient operations that cause this. In tests we simulate
    // that with an open cursor that we close asynchronously with a delay.
    std::unique_ptr<SortedDataInterface::Cursor> cursor =
        entry->accessMethod()->asSortedData()->newCursor(operationContext());
    stdx::thread async_close_cursor([&] {
        sleepmillis(1);
        cursor.reset();
    });

    // Start rebuilding the index while the underlying table is busy. It will become
    // not-busy asynchronously.
    ASSERT_OK(collWriter->getIndexCatalog()->resetUnfinishedIndexForRecovery(
        operationContext(), collWriter, entry));

    async_close_cursor.join();
}

}  // namespace mongo
