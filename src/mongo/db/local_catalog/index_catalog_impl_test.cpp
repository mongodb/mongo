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

#include "mongo/db/local_catalog/index_catalog_impl.h"

#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/ddl/list_indexes_allowed_fields.h"
#include "mongo/db/local_catalog/index_key_validate.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using IndexCatalogImplTest = CatalogTestFixture;

}  // namespace

TEST_F(IndexCatalogImplTest, WithInvalidIndexSpec) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("IndexCatalogImplTest.WithInvalidIndexSpec");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));

    IndexSpec spec;
    spec.version(1).name("x_1").addKeys(BSON("x" << 1));
    auto bson = spec.toBSON();
    BSONObjBuilder bob(bson);
    // Explicitly add an invalid spec field so that we store the wrong spec on disk.
    bob.append(IndexDescriptor::kExpireAfterSecondsFieldName, "true");
    bson = bob.obj();

    // Create an index which has an invalid on-disk format. This gets fixed whenever we return them
    // with listIndexes.
    {
        AutoGetCollection autoColl(operationContext(), nss, MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), autoColl};

        auto writableColl = writer.getWritableCollection(operationContext());
        IndexDescriptor desc{IndexNames::BTREE, bson};
        ASSERT_OK(writableColl->prepareForIndexBuild(
            operationContext(), &desc, "index-ident", boost::none));
        auto entry = writableColl->getIndexCatalog()->getWritableEntryByName(
            operationContext(), desc.indexName(), IndexCatalog::InclusionPolicy::kAll);
        writableColl->indexBuildSuccess(operationContext(), entry);
        wuow.commit();
    }

    {
        auto fixedSpec = index_key_validate::repairIndexSpec(nss, bson);
        AutoGetCollection autoColl(operationContext(), nss, MODE_X);
        // We have a spec that's fixed according to what listIndexes would output and the on-disk
        // one. These two are different, so we expect them to cause a conflict and mismatch.
        auto indexes = autoColl->getIndexCatalog()->removeExistingIndexesNoChecks(
            operationContext(), autoColl.getCollection(), {fixedSpec});
        ASSERT_FALSE(indexes.empty());
        // However, if we specify to the index catalog that we must repair the spec before
        // comparison with the given allowed fields then we should have no conflict.
        indexes = autoColl->getIndexCatalog()->removeExistingIndexesNoChecks(
            operationContext(),
            autoColl.getCollection(),
            {fixedSpec},
            IndexCatalog::RemoveExistingIndexesFlags{true, &kAllowedListIndexesFieldNames});
        ASSERT_TRUE(indexes.empty());
    }
}

}  // namespace mongo
