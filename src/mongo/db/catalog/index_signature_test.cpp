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
#include "mongo/db/query/collection_query_info.h"

namespace mongo {
namespace {

class IndexSignatureTest : public CatalogTestFixture {
public:
    IndexSignatureTest() : CatalogTestFixture() {}

    StatusWith<const IndexCatalogEntry*> createIndex(BSONObj spec) {
        // Get the index catalog associated with the test collection.
        auto* indexCatalog = coll()->getIndexCatalog();
        // Build the specified index on the collection.
        WriteUnitOfWork wuow(opCtx());
        auto status = indexCatalog->createIndexOnEmptyCollection(opCtx(), spec);
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
        return std::make_unique<IndexDescriptor>(
            coll(), IndexNames::findPluginName(keyPattern), spec);
    }

    const NamespaceString& nss() const {
        return _nss;
    }

    Collection* coll() const {
        return _coll->getCollection();
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

    // Helper function to add all elements in 'fields' into 'input', replacing any existing fields.
    // Returns a new object containing the added fields.
    BSONObj _addFields(BSONObj input, BSONObj fields) {
        for (auto&& field : fields) {
            input = input.addField(field);
        }
        return input;
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
        _addFields(indexSpec, fromjson("{collation: {locale: 'en_US', strength: 3}}")));
    ASSERT(collationDesc->compareIndexOptions(opCtx(), basicIndex) ==
           IndexDescriptor::Comparison::kIdentical);

    // Confirm that attempting to build this index will result in ErrorCodes::IndexAlreadyExists.
    ASSERT_EQ(createIndex(collationDesc->infoObj()), ErrorCodes::IndexAlreadyExists);

    // Now set the unique field. The indexes now compare kEquivalent, but not kIdentical. This means
    // that all signature fields which uniquely identify the index match, but other fields differ.
    auto collationUniqueDesc =
        makeIndexDescriptor(_addFields(collationDesc->infoObj(), fromjson("{unique: true}")));
    ASSERT(collationUniqueDesc->compareIndexOptions(opCtx(), basicIndex) ==
           IndexDescriptor::Comparison::kEquivalent);

    // Attempting to build the index, whether with the same name or a different name, now throws
    // IndexOptionsConflict. The error message returned with the exception specifies whether the
    // name matches an existing index.
    ASSERT_EQ(createIndex(collationUniqueDesc->infoObj()), ErrorCodes::IndexOptionsConflict);
    ASSERT_EQ(createIndex(_addFields(collationUniqueDesc->infoObj(),
                                     fromjson("{name: 'collationUnique'}"))),
              ErrorCodes::IndexOptionsConflict);

    // Now create an index spec with an entirely different collation. The two indexes compare as
    // being kDifferent; this means that both of these indexes can co-exist together.
    auto differentCollationDesc = makeIndexDescriptor(
        _addFields(collationDesc->infoObj(), fromjson("{collation: {locale: 'fr'}}")));
    ASSERT(differentCollationDesc->compareIndexOptions(opCtx(), basicIndex) ==
           IndexDescriptor::Comparison::kDifferent);

    // Verify that we can build this index alongside the existing indexes.
    ASSERT_OK(createIndex(
        _addFields(differentCollationDesc->infoObj(), fromjson("{name: 'differentCollation'}"))));
}

TEST_F(IndexSignatureTest,
       CanCreateMultipleIndexesOnSameKeyPatternWithDifferentPartialFilterExpressions) {
    // Create a basic index on {a: 1} with no partialFilterExpression.
    auto indexSpec = fromjson("{v: 2, name: 'a_1', key: {a: 1}}");
    auto* basicIndex = unittest::assertGet(createIndex(indexSpec));

    // Create an index descriptor on the same keyPattern with a partialFilterExpression. The two
    // indexes compare as kDifferent, because the partialFilterExpression is one of the fields in
    // the index's signature.
    auto partialFilterDesc = makeIndexDescriptor(_addFields(
        indexSpec, fromjson("{partialFilterExpression: {a: {$gt: 5, $lt: 10}, b: 'blah'}}")));
    ASSERT(partialFilterDesc->compareIndexOptions(opCtx(), basicIndex) ==
           IndexDescriptor::Comparison::kDifferent);

    // Verify that we can build an index with this spec alongside the original index.
    auto* partialFilterIndex = unittest::assertGet(
        createIndex(_addFields(partialFilterDesc->infoObj(), fromjson("{name: 'partialFilter'}"))));

    // Verify that partialFilterExpressions are normalized before being compared. Here, the filter
    // is expressed differently than in the existing index, but the two still compare as kIdentical.
    auto partialFilterDupeDesc = makeIndexDescriptor(
        _addFields(indexSpec,
                   fromjson("{name: 'partialFilter', partialFilterExpression: {$and: [{b: 'blah'}, "
                            "{a: {$lt: 10}}, {a: {$gt: 5}}]}}")));
    ASSERT(partialFilterDupeDesc->compareIndexOptions(opCtx(), partialFilterIndex) ==
           IndexDescriptor::Comparison::kIdentical);

    // Confirm that attempting to build this index will result in ErrorCodes::IndexAlreadyExists.
    ASSERT_EQ(createIndex(partialFilterDupeDesc->infoObj()), ErrorCodes::IndexAlreadyExists);

    // Now set the unique field. The indexes now compare kEquivalent, but not kIdentical. This means
    // that all signature fields which uniquely identify the index match, but other fields differ.
    auto partialFilterUniqueDesc =
        makeIndexDescriptor(_addFields(partialFilterDesc->infoObj(), fromjson("{unique: true}")));
    ASSERT(partialFilterUniqueDesc->compareIndexOptions(opCtx(), partialFilterIndex) ==
           IndexDescriptor::Comparison::kEquivalent);

    // Attempting to build the index, whether with the same name or a different name, now throws
    // IndexOptionsConflict. The error message returned with the exception specifies whether the
    // name matches an existing index.
    ASSERT_EQ(createIndex(_addFields(partialFilterUniqueDesc->infoObj(),
                                     fromjson("{name: 'partialFilterExpression'}"))),
              ErrorCodes::IndexOptionsConflict);
    ASSERT_EQ(createIndex(_addFields(partialFilterUniqueDesc->infoObj(),
                                     fromjson("{name: 'partialFilterUnique'}"))),
              ErrorCodes::IndexOptionsConflict);

    // Now create an index spec with an entirely different partialFilterExpression. The two indexes
    // compare as kDifferent; this means that both of these indexes can co-exist together.
    auto differentPartialFilterDesc = makeIndexDescriptor(
        _addFields(partialFilterDesc->infoObj(),
                   fromjson("{partialFilterExpression: {a: {$gt: 0, $lt: 10}, b: 'blah'}}")));
    ASSERT(differentPartialFilterDesc->compareIndexOptions(opCtx(), partialFilterIndex) ==
           IndexDescriptor::Comparison::kDifferent);

    // Verify that we can build this index alongside the existing indexes.
    ASSERT_OK(createIndex(_addFields(differentPartialFilterDesc->infoObj(),
                                     fromjson("{name: 'differentPartialFilter'}"))));
}

TEST_F(IndexSignatureTest, CannotCreateMultipleIndexesOnSameKeyPatternIfNonSignatureFieldsDiffer) {
    // Create a basic index on {a: 1} with all other options set to their default values.
    auto indexSpec = fromjson("{v: 2, name: 'a_1', key: {a: 1}}");
    auto* basicIndex = unittest::assertGet(createIndex(indexSpec));

    // TODO SERVER-47657: unique and sparse should be part of the signature.
    std::vector<BSONObj> nonSigOptions = {
        BSON(IndexDescriptor::kUniqueFieldName << true),
        BSON(IndexDescriptor::kSparseFieldName << true),
        BSON(IndexDescriptor::kExpireAfterSecondsFieldName << 10)};

    // Verify that changing each of the non-signature fields does not distinguish this index from
    // the existing index. The two are considered equivalent, and we cannot build the new index.
    for (auto&& nonSigOpt : nonSigOptions) {
        auto nonSigDesc = makeIndexDescriptor(_addFields(indexSpec, nonSigOpt));
        ASSERT(nonSigDesc->compareIndexOptions(opCtx(), basicIndex) ==
               IndexDescriptor::Comparison::kEquivalent);
        ASSERT_EQ(createIndex(nonSigDesc->infoObj()), ErrorCodes::IndexOptionsConflict);
    }

    // Build a wildcard index and confirm that 'wildcardProjection' is a non-signature field.
    // TODO SERVER-47659: wildcardProjection should be part of the signature.
    auto* wildcardIndex =
        unittest::assertGet(createIndex(fromjson("{v: 2, name: '$**_1', key: {'$**': 1}}")));
    auto nonSigWildcardDesc = makeIndexDescriptor(_addFields(
        wildcardIndex->descriptor()->infoObj(), fromjson("{wildcardProjection: {a: 1}}")));
    ASSERT(nonSigWildcardDesc->compareIndexOptions(opCtx(), wildcardIndex) ==
           IndexDescriptor::Comparison::kEquivalent);
    ASSERT_EQ(createIndex(
                  _addFields(nonSigWildcardDesc->infoObj(), fromjson("{name: 'nonSigWildcard'}"))),
              ErrorCodes::IndexOptionsConflict);
}

}  // namespace
}  // namespace mongo
