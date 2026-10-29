// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/index_catalog_impl.h"

#include "mongo/db/index_key_validate.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/list_indexes_allowed_fields.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/barrier.h"
#include "mongo/util/time_support.h"

#include <thread>

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
        auto acq = acquireCollection(operationContext(),
                                     CollectionAcquisitionRequest::fromOpCtx(
                                         operationContext(), nss, AcquisitionPrerequisites::kWrite),
                                     MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), &acq};

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
        auto acq = acquireCollection(operationContext(),
                                     CollectionAcquisitionRequest::fromOpCtx(
                                         operationContext(), nss, AcquisitionPrerequisites::kWrite),
                                     MODE_X);
        // We have a spec that's fixed according to what listIndexes would output and the on-disk
        // one. These two are different, so we expect them to cause a conflict and mismatch.
        auto indexes = acq.getCollectionPtr()->getIndexCatalog()->removeExistingIndexesNoChecks(
            operationContext(), acq.getCollectionPtr(), {fixedSpec});
        ASSERT_FALSE(indexes.empty());
        // However, if we specify to the index catalog that we must repair the spec before
        // comparison with the given allowed fields then we should have no conflict.
        indexes = acq.getCollectionPtr()->getIndexCatalog()->removeExistingIndexesNoChecks(
            operationContext(),
            acq.getCollectionPtr(),
            {fixedSpec},
            IndexCatalog::RemoveExistingIndexesFlags{true, &kAllowedListIndexesFieldNames});
        ASSERT_TRUE(indexes.empty());
    }
}

}  // namespace mongo
