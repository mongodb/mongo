/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
class IndexCatalogEntryMock : public IndexCatalogEntry {
public:
    explicit IndexCatalogEntryMock(IndexDescriptor* descriptor) : _descriptor(descriptor) {}

    const std::string& getIdent() const override {
        MONGO_UNIMPLEMENTED;
    }
    std::shared_ptr<Ident> getSharedIdent() const override {
        MONGO_UNIMPLEMENTED;
    }
    void setIdent(std::shared_ptr<Ident> newIdent) override {
        MONGO_UNIMPLEMENTED;
    }

    IndexDescriptor* descriptor() override {
        return _descriptor;
    }

    const IndexDescriptor* descriptor() const override {
        return _descriptor;
    }

    IndexAccessMethod* accessMethod() const override {
        MONGO_UNIMPLEMENTED;
    }

    void setAccessMethod(std::unique_ptr<IndexAccessMethod> accessMethod) override {
        MONGO_UNIMPLEMENTED;
    }

    bool isHybridBuilding() const override {
        MONGO_UNIMPLEMENTED;
    }

    IndexBuildInterceptor* indexBuildInterceptor() const override {
        MONGO_UNIMPLEMENTED;
    }

    void setIndexBuildInterceptor(IndexBuildInterceptor* interceptor) override {
        MONGO_UNIMPLEMENTED;
    }

    const Ordering& ordering() const override {
        MONGO_UNIMPLEMENTED;
    }

    const MatchExpression* getFilterExpression() const override {
        return nullptr;
    }

    const CollatorInterface* getCollator() const override {
        return nullptr;
    }

    NamespaceString getNSSFromCatalog(OperationContext* opCtx) const override {
        MONGO_UNIMPLEMENTED;
    }

    void setIsReady(bool newIsReady) override {
        MONGO_UNIMPLEMENTED;
    }
    void setIsFrozen(bool newIsFrozen) override {
        MONGO_UNIMPLEMENTED;
    }

    void setDropped() override {
        MONGO_UNIMPLEMENTED;
    }
    bool isDropped() const override {
        MONGO_UNIMPLEMENTED;
    }

    bool isMultikey(OperationContext* opCtx, const CollectionPtr& collection) const override {
        MONGO_UNIMPLEMENTED;
    }

    MultikeyPaths getMultikeyPaths(OperationContext* opCtx,
                                   const CollectionPtr& collection) const override {
        MONGO_UNIMPLEMENTED;
    }

    void setMultikey(OperationContext* opCtx,
                     const CollectionPtr& coll,
                     const KeyStringSet& multikeyMetadataKeys,
                     const MultikeyPaths& multikeyPaths) const override {
        MONGO_UNIMPLEMENTED;
    }

    void forceSetMultikey(OperationContext* opCtx,
                          const CollectionPtr& coll,
                          bool isMultikey,
                          const MultikeyPaths& multikeyPaths) const override {
        MONGO_UNIMPLEMENTED;
    }

    bool isReady() const override {
        MONGO_UNIMPLEMENTED;
    }

    bool isPresentInMySnapshot(OperationContext* opCtx) const override {
        MONGO_UNIMPLEMENTED;
    }

    bool isReadyInMySnapshot(OperationContext* opCtx) const override {
        MONGO_UNIMPLEMENTED;
    }

    bool isFrozen() const override {
        MONGO_UNIMPLEMENTED;
    }

    bool shouldValidateDocument() const override {
        MONGO_UNIMPLEMENTED;
    }

    const UpdateIndexData& getIndexedPaths() const override {
        MONGO_UNIMPLEMENTED;
    }

private:
    IndexDescriptor* _descriptor;
};

std::unique_ptr<IndexDescriptor> makeIndexDescriptor(StringData indexName,
                                                     BSONObj keyPattern,
                                                     BSONObj wildcardProjection) {
    auto indexSpec = BSON(IndexDescriptor::kIndexVersionFieldName
                          << 2 << IndexDescriptor::kIndexNameFieldName << indexName
                          << IndexDescriptor::kKeyPatternFieldName << keyPattern);
    if (!wildcardProjection.isEmpty()) {
        indexSpec = indexSpec.addFields(
            BSON(IndexDescriptor::kWildcardProjectionFieldName << wildcardProjection));
    }
    const auto& accessMethodName = IndexNames::findPluginName(keyPattern);
    return std::make_unique<IndexDescriptor>(accessMethodName, std::move(indexSpec));
}
}  // namespace

TEST(CollectionQueryInfoTest, computeUpdateIndexDataForCompoundWildcardIndex) {
    RAIIServerParameterControllerForTest controller("featureFlagCompoundWildcardIndexes", true);
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test"_sd);
    CollectionOptions collOptions{};
    DevNullKVEngine engine{};
    auto sortedDataInterface =
        engine.getSortedDataInterface(nullptr, nss, collOptions, "wildcardIndent", nullptr);

    auto indexDescriptor = makeIndexDescriptor(
        "wildcardIndex", BSON("a" << 1 << "b" << 1 << "$**" << 1), BSON("c" << 1 << "_id" << 1));

    ASSERT_EQ(IndexNames::WILDCARD, indexDescriptor->getAccessMethodName());

    IndexCatalogEntryMock indexCatalogEntry{indexDescriptor.get()};

    WildcardAccessMethod accessMethod{&indexCatalogEntry, std::move(sortedDataInterface)};
    UpdateIndexData outData{};
    CollectionQueryInfo::computeUpdateIndexData(&indexCatalogEntry, &accessMethod, &outData);

    // Asserting that expected fields are included.
    ASSERT_TRUE(outData.mightBeIndexed(FieldRef{"a"_sd}));
    ASSERT_TRUE(outData.mightBeIndexed(FieldRef{"b"_sd}));
    ASSERT_TRUE(outData.mightBeIndexed(FieldRef{"c"_sd}));
    ASSERT_TRUE(outData.mightBeIndexed(FieldRef{"_id"_sd}));

    // Asserting that unexpected fields are not included.
    ASSERT_FALSE(outData.mightBeIndexed(FieldRef{"d"_sd}));
    ASSERT_FALSE(outData.mightBeIndexed(FieldRef{"$**"_sd}));
}

TEST(CollectionQueryInfoTest, computeUpdateIndexDataForCompoundWildcardIndex_ExcludeCase) {
    RAIIServerParameterControllerForTest controller("featureFlagCompoundWildcardIndexes", true);
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test"_sd);
    CollectionOptions collOptions{};
    DevNullKVEngine engine{};
    auto sortedDataInterface =
        engine.getSortedDataInterface(nullptr, nss, collOptions, "wildcardIndent", nullptr);

    auto indexDescriptor = makeIndexDescriptor(
        "wildcardIndex", BSON("a" << 1 << "b" << 1 << "$**" << 1), BSON("c" << 0));

    ASSERT_EQ(IndexNames::WILDCARD, indexDescriptor->getAccessMethodName());

    IndexCatalogEntryMock indexCatalogEntry{indexDescriptor.get()};

    WildcardAccessMethod accessMethod{&indexCatalogEntry, std::move(sortedDataInterface)};
    UpdateIndexData outData{};
    CollectionQueryInfo::computeUpdateIndexData(&indexCatalogEntry, &accessMethod, &outData);

    // When wildcardProjection has exclusion, everything is "indexed", since we don't know for sure,
    // which fields are indexed.
    ASSERT_TRUE(outData.mightBeIndexed(FieldRef{"a"_sd}));
    ASSERT_TRUE(outData.mightBeIndexed(FieldRef{"b"_sd}));
    ASSERT_TRUE(outData.mightBeIndexed(FieldRef{"c"_sd}));
    ASSERT_TRUE(outData.mightBeIndexed(FieldRef{"d"_sd}));
    ASSERT_TRUE(outData.mightBeIndexed(FieldRef{"_id"_sd}));
}
}  // namespace mongo
