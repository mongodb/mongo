/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog_entry_impl.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/query/collection_query_info.h"

namespace mongo {
namespace {

class IndexSignatureTest : public CatalogTestFixture {
public:
    IndexSignatureTest() : CatalogTestFixture() {}

    StatusWith<const IndexCatalogEntry*> createIndex(BSONObj spec) {
        // Build the specified index on the collection.
        WriteUnitOfWork wuow(opCtx());
        // Get the index catalog associated with the test collection.
        auto* indexCatalog = _coll->getWritableCollection()->getIndexCatalog();
        auto status = indexCatalog->createIndexOnEmptyCollection(
            opCtx(), _coll->getWritableCollection(), spec);
        if (!status.isOK()) {
            return status.getStatus();
        }
        wuow.commit();
        // Find the index entry and return it.
        return indexCatalog->getEntry(indexCatalog->findIndexByName(
            opCtx(), spec.getStringField(IndexDescriptor::kIndexNameFieldName)));
    }

    std::unique_ptr<IndexDescriptor> makeIndexDescriptor(BSONObj spec) {
        auto keyPattern = spec.getObjectField(IndexDescriptor::kKeyPatternFieldName);
        return std::make_unique<IndexDescriptor>(IndexNames::findPluginName(keyPattern), spec);
    }

    BSONObj normalizeIndexSpec(BSONObj spec) {
        std::vector<BSONObj> specs =
            IndexBuildsCoordinator::normalizeIndexSpecs(opCtx(), coll(), {spec});
        return specs[0];
    }

    const NamespaceString& nss() const {
        return _nss;
    }

    const CollectionPtr& coll() const {
        return (*_coll).getCollection();
    }

    OperationContext* opCtx() {
        return operationContext();
    }

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(storageInterface()->createCollection(opCtx(), _nss, {}));
        _coll.emplace(opCtx(), _nss, MODE_X);
    }

    void tearDown() override {
        _coll.reset();
        CatalogTestFixture::tearDown();
    }

private:
    boost::optional<AutoGetCollection> _coll;
    NamespaceString _nss{"fooDB.barColl"};
};

TEST_F(IndexSignatureTest, CanCreateMultipleIndexesOnSameKeyPatternWithDifferentCollations) {
    // Create an index on {a: 1} with an 'en_US' collation.
    auto indexSpec = fromjson("{v: 2, name: 'a_1', key: {a: 1}, collation: {locale: 'en_US'}}");
    auto* basicIndex = unittest::assertGet(createIndex(indexSpec));

    // Create an index descriptor on the same keyPattern whose collation has an explicit strength.
    // The two indexes compare as kIdentical, because the collation comparison is performed using
    // the parsed collator rather than the static collation object from the BSON index specs.
    auto collationDesc = makeIndexDescriptor(
        indexSpec.addFields(fromjson("{collation: {locale: 'en_US', strength: 3}}")));
    ASSERT(collationDesc->compareIndexOptions(opCtx(), coll()->ns(), basicIndex) ==
           IndexDescriptor::Comparison::kIdentical);

    // Confirm that attempting to build this index will result in ErrorCodes::IndexAlreadyExists.
    ASSERT_EQ(createIndex(collationDesc->infoObj()), ErrorCodes::IndexAlreadyExists);

    // Now add storage engine option. The indexes now compare kEquivalent, but not kIdentical. This
    // means that all signature fields which uniquely identify the index match, but other fields
    // differ.
    auto collationStorageEngineDesc =
        makeIndexDescriptor(indexSpec.addFields(fromjson("{storageEngine: {wiredTiger: {}}}")));
    ASSERT(collationStorageEngineDesc->compareIndexOptions(opCtx(), coll()->ns(), basicIndex) ==
           IndexDescriptor::Comparison::kEquivalent);

    // Attempting to build the index, whether with the same name or a different name, now throws
    // IndexOptionsConflict. The error message returned with the exception specifies whether the
    // name matches an existing index.
    ASSERT_EQ(createIndex(collationStorageEngineDesc->infoObj()), ErrorCodes::IndexOptionsConflict);
    ASSERT_EQ(createIndex(collationStorageEngineDesc->infoObj().addFields(
                  fromjson("{name: 'collationStorageEngine'}"))),
              ErrorCodes::IndexOptionsConflict);

    // Now create an index spec with an entirely different collation. The two indexes compare as
    // being kDifferent; this means that both of these indexes can co-exist together.
    auto differentCollationDesc = makeIndexDescriptor(
        collationDesc->infoObj().addFields(fromjson("{collation: {locale: 'fr'}}")));
    ASSERT(differentCollationDesc->compareIndexOptions(opCtx(), coll()->ns(), basicIndex) ==
           IndexDescriptor::Comparison::kDifferent);

    // Verify that we can build this index alongside the existing indexes.
    ASSERT_OK(createIndex(
        differentCollationDesc->infoObj().addFields(fromjson("{name: 'differentCollation'}"))));
}

TEST_F(IndexSignatureTest,
       CanCreateMultipleIndexesOnSameKeyPatternWithDifferentPartialFilterExpressions) {
    // Create a basic index on {a: 1} with no partialFilterExpression.
    auto indexSpec = fromjson("{v: 2, name: 'a_1', key: {a: 1}}");
    auto* basicIndex = unittest::assertGet(createIndex(indexSpec));

    // Create an index descriptor on the same keyPattern with a partialFilterExpression. The two
    // indexes compare as kDifferent, because the partialFilterExpression is one of the fields in
    // the index's signature.
    auto partialFilterDesc = makeIndexDescriptor(indexSpec.addFields(
        fromjson("{partialFilterExpression: {a: {$gt: 5, $lt: 10}, b: 'blah'}}")));
    ASSERT(partialFilterDesc->compareIndexOptions(opCtx(), coll()->ns(), basicIndex) ==
           IndexDescriptor::Comparison::kDifferent);

    // Verify that we can build an index with this spec alongside the original index.
    auto* partialFilterIndex = unittest::assertGet(
        createIndex(partialFilterDesc->infoObj().addFields(fromjson("{name: 'partialFilter'}"))));

    // Verify that partialFilterExpressions are normalized before being compared. Here, the filter
    // is expressed differently than in the existing index, but the two still compare as kIdentical.
    auto partialFilterDupeDesc = makeIndexDescriptor(indexSpec.addFields(
        fromjson("{name: 'partialFilter', partialFilterExpression: {$and: [{b: 'blah'}, "
                 "{a: {$lt: 10}}, {a: {$gt: 5}}]}}")));
    ASSERT(partialFilterDupeDesc->compareIndexOptions(opCtx(), coll()->ns(), partialFilterIndex) ==
           IndexDescriptor::Comparison::kIdentical);

    // Confirm that attempting to build this index will result in ErrorCodes::IndexAlreadyExists.
    ASSERT_EQ(createIndex(partialFilterDupeDesc->infoObj()), ErrorCodes::IndexAlreadyExists);

    // Now add the storage engine field. The indexes now compare kEquivalent, but not kIdentical.
    // This means that all signature fields which uniquely identify the index match, but other
    // fields differ.
    auto partialFilterStorageEngineDesc = makeIndexDescriptor(
        partialFilterDesc->infoObj().addFields(fromjson("{storageEngine: {wiredTiger: {}}}")));
    ASSERT(partialFilterStorageEngineDesc->compareIndexOptions(
               opCtx(), coll()->ns(), partialFilterIndex) ==
           IndexDescriptor::Comparison::kEquivalent);

    // Attempting to build the index, whether with the same name or a different name, now throws
    // IndexOptionsConflict. The error message returned with the exception specifies whether the
    // name matches an existing index.
    ASSERT_EQ(createIndex(partialFilterStorageEngineDesc->infoObj().addFields(
                  fromjson("{name: 'partialFilter'}"))),
              ErrorCodes::IndexOptionsConflict);
    ASSERT_EQ(createIndex(partialFilterStorageEngineDesc->infoObj().addFields(
                  fromjson("{name: 'partialFilterStorageEngine'}"))),
              ErrorCodes::IndexOptionsConflict);

    // Now create an index spec with an entirely different partialFilterExpression. The two indexes
    // compare as kDifferent; this means that both of these indexes can co-exist together.
    auto differentPartialFilterDesc = makeIndexDescriptor(partialFilterDesc->infoObj().addFields(
        fromjson("{partialFilterExpression: {a: {$gt: 0, $lt: 10}, b: 'blah'}}")));
    ASSERT(differentPartialFilterDesc->compareIndexOptions(
               opCtx(), coll()->ns(), partialFilterIndex) ==
           IndexDescriptor::Comparison::kDifferent);

    // Verify that we can build this index alongside the existing indexes.
    ASSERT_OK(createIndex(differentPartialFilterDesc->infoObj().addFields(
        fromjson("{name: 'differentPartialFilter'}"))));
}

TEST_F(IndexSignatureTest, CannotCreateMultipleIndexesOnSameKeyPatternIfNonSignatureFieldsDiffer) {
    // Create a basic index on {a: 1} with all other options set to their default values.
    auto indexSpec = fromjson("{v: 2, name: 'a_1', key: {a: 1}}");
    auto* basicIndex = unittest::assertGet(createIndex(indexSpec));

    std::vector<BSONObj> nonSigOptions = {
        BSON(IndexDescriptor::kStorageEngineFieldName << BSON("wiredTiger"_sd << BSONObj())),
        BSON(IndexDescriptor::kExpireAfterSecondsFieldName << 10)};

    // Verify that changing each of the non-signature fields does not distinguish this index from
    // the existing index. The two are considered equivalent, and we cannot build the new index.
    for (auto&& nonSigOpt : nonSigOptions) {
        auto nonSigDesc = makeIndexDescriptor(indexSpec.addFields(nonSigOpt));
        ASSERT(nonSigDesc->compareIndexOptions(opCtx(), coll()->ns(), basicIndex) ==
               IndexDescriptor::Comparison::kEquivalent);
        ASSERT_EQ(createIndex(nonSigDesc->infoObj()), ErrorCodes::IndexOptionsConflict);
    }
}

TEST_F(IndexSignatureTest, CanCreateMultipleIndexesOnSameKeyPatternWithDifferentUniqueProperty) {
    // Create an index on {a: 1}.
    auto indexSpec = fromjson("{v: 2, name: 'a_1', key: {a: 1}}");
    auto* basicIndex = unittest::assertGet(createIndex(indexSpec));

    auto basicIndexDesc = makeIndexDescriptor(indexSpec);

    // Create an index on the same key pattern with the 'unique' field explicitly set to false.
    // The two indexes compare as kIdentical.
    auto uniqueFalseDesc = makeIndexDescriptor(
        basicIndexDesc->infoObj().addFields(fromjson("{name: 'a_1', unique: false}")));
    ASSERT(uniqueFalseDesc->compareIndexOptions(opCtx(), coll()->ns(), basicIndex) ==
           IndexDescriptor::Comparison::kIdentical);

    // Verify that creating an index with the same name and 'unique' = false fails with
    // IndexAlreadyExists error code.
    ASSERT_EQ(createIndex(uniqueFalseDesc->infoObj()), ErrorCodes::IndexAlreadyExists);

    // Now set the unique field. The indexes now compare kDifferent. This means that the index
    // signature does not match and we can create this index.
    auto uniqueDesc = makeIndexDescriptor(
        basicIndexDesc->infoObj().addFields(fromjson("{unique: true, name: 'a_unique'}")));
    ASSERT(uniqueDesc->compareIndexOptions(opCtx(), coll()->ns(), basicIndex) ==
           IndexDescriptor::Comparison::kDifferent);

    // Verify that we can build this index alongside the existing index since its 'unique' option
    // differs.
    auto* uniqueIndex = unittest::assertGet(createIndex(uniqueDesc->infoObj()));

    // Now create another unique index. The indexes now compare kIdentical.
    auto uniqueDesc2 = makeIndexDescriptor(
        basicIndexDesc->infoObj().addFields(fromjson("{unique: true, name: 'a_unique2'}")));
    ASSERT(uniqueDesc2->compareIndexOptions(opCtx(), coll()->ns(), uniqueIndex) ==
           IndexDescriptor::Comparison::kIdentical);

    // Verify that creating another unique index with a different name fails with
    // IndexOptionsConflict error.
    ASSERT_EQ(createIndex(uniqueDesc2->infoObj()), ErrorCodes::IndexOptionsConflict);

    // Now add a non-signature index option to the unique index and two indexes compare kEquivalent.
    auto uniqueStorageEngineDesc = makeIndexDescriptor(
        uniqueDesc->infoObj().addFields(fromjson("{storageEngine: {wiredTiger: {}}}")));
    ASSERT(uniqueStorageEngineDesc->compareIndexOptions(opCtx(), coll()->ns(), uniqueIndex) ==
           IndexDescriptor::Comparison::kEquivalent);

    // Verify that creating another unique index with a non-signature option fails with
    // IndexOptionsConflict error.
    ASSERT_EQ(createIndex(uniqueStorageEngineDesc->infoObj()), ErrorCodes::IndexOptionsConflict);
}

TEST_F(IndexSignatureTest, CanCreateMultipleIndexesOnSameKeyPatternWithDifferentSparseProperty) {
    // Create an index on {a: 1}.
    auto indexSpec = fromjson("{v: 2, name: 'a_1', key: {a: 1}}");
    auto* basicIndex = unittest::assertGet(createIndex(indexSpec));

    auto basicIndexDesc = makeIndexDescriptor(indexSpec);

    // Create an index on the same key pattern with the 'sparse' field explicitly set to false.
    // The two indexes compare as kIdentical.
    auto sparseFalseDesc = makeIndexDescriptor(
        basicIndexDesc->infoObj().addFields(fromjson("{name: 'a_1', unique: false}")));
    ASSERT(sparseFalseDesc->compareIndexOptions(opCtx(), coll()->ns(), basicIndex) ==
           IndexDescriptor::Comparison::kIdentical);

    // Verify that creating an index with the same name and 'sparse' = false fails with
    // IndexAlreadyExists error code.
    ASSERT_EQ(createIndex(sparseFalseDesc->infoObj()), ErrorCodes::IndexAlreadyExists);

    // Now set the sparse field. The indexes now compare kDifferent. This means that the index
    // signature does not match and we can create this index.
    auto sparseDesc = makeIndexDescriptor(
        basicIndexDesc->infoObj().addFields(fromjson("{sparse: true, name: 'a_sparse'}")));
    ASSERT(sparseDesc->compareIndexOptions(opCtx(), coll()->ns(), basicIndex) ==
           IndexDescriptor::Comparison::kDifferent);

    // Verify that we can build this index alongside the existing index since its 'sparse' option
    // differs.
    auto* sparseIndex = unittest::assertGet(createIndex(sparseDesc->infoObj()));

    // Now create another sparse index. The indexes now compare kIdentical.
    auto sparseDesc2 = makeIndexDescriptor(
        basicIndexDesc->infoObj().addFields(fromjson("{sparse: true, name: 'a_sparse2'}")));
    ASSERT(sparseDesc2->compareIndexOptions(opCtx(), coll()->ns(), sparseIndex) ==
           IndexDescriptor::Comparison::kIdentical);

    // Verify that creating another sparse index with a different name fails with
    // IndexOptionsConflict error.
    ASSERT_EQ(createIndex(sparseDesc2->infoObj()), ErrorCodes::IndexOptionsConflict);

    // Now add a non-signature index option to the sparse index and two indexes compare kEquivalent.
    auto sparseStorageEngineDesc = makeIndexDescriptor(
        sparseDesc->infoObj().addFields(fromjson("{storageEngine: {wiredTiger: {}}}")));
    ASSERT(sparseStorageEngineDesc->compareIndexOptions(opCtx(), coll()->ns(), sparseIndex) ==
           IndexDescriptor::Comparison::kEquivalent);

    // Verify that creating another sparse index with a non-signature option fails with
    // IndexOptionsConflict error.
    ASSERT_EQ(createIndex(sparseStorageEngineDesc->infoObj()), ErrorCodes::IndexOptionsConflict);
}

TEST_F(IndexSignatureTest, NormalizeOnlyWildcardAllKeyPattern) {
    auto wcAInclusionSpec = fromjson("{v: 2, key: {'a.$**': 1}}");

    // Verifies that the path projection is not normalized.
    auto wcAInclusionDesc = makeIndexDescriptor(wcAInclusionSpec);
    auto wcAInclusionSpecAfterNormalization = normalizeIndexSpec(wcAInclusionSpec);
    ASSERT_TRUE(SimpleBSONObjComparator::kInstance.evaluate(wcAInclusionSpec ==
                                                            wcAInclusionSpecAfterNormalization));

    // Verifies that the path projection is not normalized for the created index catalog entry.
    auto* wcAInclusionIndex = unittest::assertGet(
        createIndex(wcAInclusionSpec.addFields(fromjson("{name: 'wc_a_all'}"))));
    ASSERT_TRUE(wcAInclusionIndex->descriptor()->normalizedPathProjection().isEmpty());
}

TEST_F(IndexSignatureTest,
       CanCreateMultipleIndexesOnSameKeyPatternWithDifferentWildcardProjections) {
    // Creates a base wildcard index to verify 'wildcardProjection' option is part of index
    // signature.
    auto* wildcardIndex =
        unittest::assertGet(createIndex(fromjson("{v: 2, name: 'wc_all', key: {'$**': 1}}")));

    // Verifies that another wildcard index with empty wildcardProjection compares identical
    // to 'wildcardIndex' after normalizing the index spec.
    auto anotherWcAllSpec = normalizeIndexSpec(fromjson("{v: 2, key: {'$**': 1}}"));
    auto anotherWcAllProjDesc = makeIndexDescriptor(anotherWcAllSpec);
    ASSERT(anotherWcAllProjDesc->compareIndexOptions(opCtx(), coll()->ns(), wildcardIndex) ==
           IndexDescriptor::Comparison::kIdentical);
    ASSERT_EQ(createIndex(anotherWcAllProjDesc->infoObj().addFields(fromjson("{name: 'wc_all'}"))),
              ErrorCodes::IndexAlreadyExists);
    ASSERT_EQ(
        createIndex(anotherWcAllProjDesc->infoObj().addFields(fromjson("{name: 'wc_all_1'}"))),
        ErrorCodes::IndexOptionsConflict);

    // Verifies that an index with non-empty value for 'wildcardProjection' option compares
    // different from the base wildcard index and thus can be created.
    auto wcProjADesc =
        makeIndexDescriptor(normalizeIndexSpec(wildcardIndex->descriptor()->infoObj().addFields(
            fromjson("{wildcardProjection: {a: 1}}"))));
    ASSERT(wcProjADesc->compareIndexOptions(opCtx(), coll()->ns(), wildcardIndex) ==
           IndexDescriptor::Comparison::kDifferent);
    auto* wcProjAIndex = unittest::assertGet(
        createIndex(wcProjADesc->infoObj().addFields(fromjson("{name: 'wc_a'}"))));

    // Verifies that an index with the same value for 'wildcardProjection' option as the
    // wcProjAIndex compares identical.
    auto anotherWcProjADesc =
        makeIndexDescriptor(normalizeIndexSpec(wildcardIndex->descriptor()->infoObj().addFields(
            fromjson("{wildcardProjection: {a: 1}}"))));
    ASSERT(anotherWcProjADesc->compareIndexOptions(opCtx(), coll()->ns(), wcProjAIndex) ==
           IndexDescriptor::Comparison::kIdentical);

    // Verifies that creating an index with the same value for 'wildcardProjection' option and the
    // same name fails with IndexAlreadyExists error.
    ASSERT_EQ(createIndex(anotherWcProjADesc->infoObj().addFields(fromjson("{name: 'wc_a'}"))),
              ErrorCodes::IndexAlreadyExists);
    // Verifies that creating an index with the same value for 'wildcardProjection' option and a
    // different name fails with IndexOptionsConflict error.
    ASSERT_EQ(createIndex(anotherWcProjADesc->infoObj().addFields(fromjson("{name: 'wc_a_1'}"))),
              ErrorCodes::IndexOptionsConflict);

    // Verifies that an index with a different value for 'wildcardProjection' option compares
    // different from the base wildcard index or 'wc_a' and thus can be created.
    auto wcProjABDesc =
        makeIndexDescriptor(normalizeIndexSpec(wildcardIndex->descriptor()->infoObj().addFields(
            fromjson("{wildcardProjection: {a: 1, b: 1}}"))));
    ASSERT(wcProjABDesc->compareIndexOptions(opCtx(), coll()->ns(), wildcardIndex) ==
           IndexDescriptor::Comparison::kDifferent);
    ASSERT(wcProjABDesc->compareIndexOptions(opCtx(), coll()->ns(), wcProjAIndex) ==
           IndexDescriptor::Comparison::kDifferent);
    auto* wcProjABIndex = unittest::assertGet(
        createIndex(wcProjABDesc->infoObj().addFields(fromjson("{name: 'wc_a_b'}"))));

    // Verifies that an index with sub fields for 'wildcardProjection' option compares
    // different from the base wildcard index or 'wc_a' or 'wc_a_b' and thus can be created.
    auto wcProjASubBCDesc =
        makeIndexDescriptor(normalizeIndexSpec(wildcardIndex->descriptor()->infoObj().addFields(
            fromjson("{wildcardProjection: {a: {b: 1, c: 1}}}"))));
    ASSERT(wcProjASubBCDesc->compareIndexOptions(opCtx(), coll()->ns(), wildcardIndex) ==
           IndexDescriptor::Comparison::kDifferent);
    ASSERT(wcProjASubBCDesc->compareIndexOptions(opCtx(), coll()->ns(), wcProjAIndex) ==
           IndexDescriptor::Comparison::kDifferent);
    ASSERT(wcProjASubBCDesc->compareIndexOptions(opCtx(), coll()->ns(), wcProjABIndex) ==
           IndexDescriptor::Comparison::kDifferent);
    auto* wcProjASubBCIndex = unittest::assertGet(
        createIndex(wcProjASubBCDesc->infoObj().addFields(fromjson("{name: 'wc_a_sub_b_c'}"))));

    // Verifies that two indexes with the same projection in different order compares identical.
    auto wcProjASubCBDesc =
        makeIndexDescriptor(normalizeIndexSpec(wildcardIndex->descriptor()->infoObj().addFields(
            fromjson("{wildcardProjection: {a: {c: 1, b: 1}}}"))));
    ASSERT(wcProjASubCBDesc->compareIndexOptions(opCtx(), coll()->ns(), wcProjASubBCIndex) ==
           IndexDescriptor::Comparison::kIdentical);
    // Verifies that two indexes with the same projection in different order can not be created.
    ASSERT_EQ(
        createIndex(wcProjASubCBDesc->infoObj().addFields(fromjson("{name: 'wc_a_sub_b_c'}"))),
        ErrorCodes::IndexAlreadyExists);
    ASSERT_EQ(
        createIndex(wcProjASubCBDesc->infoObj().addFields(fromjson("{name: 'wc_a_sub_c_b'}"))),
        ErrorCodes::IndexOptionsConflict);

    // Verifies that an index with the same value for 'wildcardProjection' option and an
    // non-signature index option compares equivalent as the 'wcProjAIndex'
    auto wcProjAWithNonSigDesc = makeIndexDescriptor(
        anotherWcProjADesc->infoObj().addFields(fromjson("{storageEngine: {wiredTiger: {}}}")));
    ASSERT(wcProjAWithNonSigDesc->compareIndexOptions(opCtx(), coll()->ns(), wcProjAIndex) ==
           IndexDescriptor::Comparison::kEquivalent);

    // Verifies that an index with the same value for 'wildcardProjection' option, non-signature
    // index option, and the same name fails with IndexOptionsConflict error.
    ASSERT_EQ(createIndex(wcProjAWithNonSigDesc->infoObj().addFields(fromjson("{name: 'wc_a'}"))),
              ErrorCodes::IndexOptionsConflict);
    // Verifies that an index with the same value for 'wildcardProjection' option, non-signature
    // index option, and a different name fails with IndexOptionsConflict error too.
    ASSERT_EQ(
        createIndex(wcProjAWithNonSigDesc->infoObj().addFields(fromjson("{name: 'wc_a_nonsig'}"))),
        ErrorCodes::IndexOptionsConflict);
}

}  // namespace
}  // namespace mongo
