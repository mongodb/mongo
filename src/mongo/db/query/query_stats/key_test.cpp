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

#include "mongo/db/query/query_stats/key.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/local_catalog/collection_type.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo::query_stats {

namespace {
static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");


struct DummyShapeSpecificComponents : public query_shape::CmdSpecificShapeComponents {
    DummyShapeSpecificComponents() {};
    void HashValue(absl::HashState state) const override {}
    size_t size() const final {
        return sizeof(DummyShapeSpecificComponents);
    }
};

class DummyShape : public query_shape::Shape {
public:
    DummyShape(NamespaceStringOrUUID nssOrUUID,
               BSONObj collation,
               DummyShapeSpecificComponents dummyComponents)
        : Shape(nssOrUUID, collation) {
        components = dummyComponents;
    }

    const query_shape::CmdSpecificShapeComponents& specificComponents() const final {
        return components;
    }

    void appendCmdSpecificShapeComponents(BSONObjBuilder&,
                                          OperationContext*,
                                          const SerializationOptions& opts) const final {}
    DummyShapeSpecificComponents components;
};

struct DummyKeyComponents : public SpecificKeyComponents {
    DummyKeyComponents() {};

    void HashValue(absl::HashState state) const override {}
    size_t size() const override {
        return sizeof(DummyKeyComponents);
    }
};

class DummyKey : public Key {
public:
    DummyKey(OperationContext* opCtx,
             std::unique_ptr<query_shape::Shape> queryShape,
             boost::optional<BSONObj> hint,
             boost::optional<repl::ReadConcernArgs> readConcern,
             bool maxTimeMS,
             query_shape::CollectionType collectionType,
             DummyKeyComponents dummyComponents)
        : Key(opCtx, std::move(queryShape), hint, readConcern, maxTimeMS, collectionType) {
        components = dummyComponents;
    }
    const SpecificKeyComponents& specificComponents() const override {
        return components;
    };
    void appendCommandSpecificComponents(BSONObjBuilder& bob,
                                         const SerializationOptions& opts) const override {};
    DummyKeyComponents components;
};
class UniversalKeyTest : public ServiceContextTest {};

TEST_F(UniversalKeyTest, SizeOfUniversalComponents) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    // Make shape for testing.
    auto collation = BSONObj{};
    auto innerComponents = std::make_unique<DummyShapeSpecificComponents>();
    auto shape = std::make_unique<DummyShape>(kDefaultTestNss, collation, *innerComponents);

    // Gather sizes and create universalComponents.
    const auto shapeSize = shape->size();
    auto clientMetadata = ClientMetadata::get(expCtx->getOperationContext()->getClient());
    const auto clientMetadataSize =
        static_cast<size_t>(clientMetadata ? clientMetadata->documentWithoutMongosInfo().objsize()
                                           : BSONObj().objsize());
    auto apiParams =
        std::make_unique<APIParameters>(APIParameters::get(expCtx->getOperationContext()));
    const auto apiParamsSize = static_cast<size_t>(
        apiParams ? sizeof(*apiParams) + shape_helpers::optionalSize(apiParams->getAPIVersion())
                  : 0);
    auto universalComponents =
        std::make_unique<UniversalKeyComponents>(std::move(shape),
                                                 clientMetadata,
                                                 BSONObj(),
                                                 BSONObj(),
                                                 BSONObj(),
                                                 BSONObj(),
                                                 BSONObj(),
                                                 std::move(apiParams),
                                                 query_shape::CollectionType::kUnknown,
                                                 true);

    const auto minimumUniversalKeyComponentSize = sizeof(std::unique_ptr<query_shape::Shape>) +
        (6 * sizeof(BSONObj)) + sizeof(std::unique_ptr<APIParameters>) + sizeof(BSONElement) +
        sizeof(query_shape::CollectionType) + sizeof(TenantId) + sizeof(unsigned long) +
        1 /*HasField*/;
    ASSERT_GTE(sizeof(UniversalKeyComponents), minimumUniversalKeyComponentSize);
    ASSERT_LTE(sizeof(UniversalKeyComponents), minimumUniversalKeyComponentSize + 8 /*padding*/);

    ASSERT_GT(universalComponents->size(),
              sizeof(UniversalKeyComponents) + shapeSize + clientMetadataSize + apiParamsSize);
    ASSERT_LTE(universalComponents->size(),
               sizeof(UniversalKeyComponents) + shapeSize + clientMetadataSize +
                   (5 * static_cast<size_t>(BSONObj().objsize())) + apiParamsSize);
}

TEST_F(UniversalKeyTest, SizeOfSpecificComponents) {
    auto innerComponents = std::make_unique<DummyShapeSpecificComponents>();
    auto keyComponents = std::make_unique<DummyKeyComponents>();

    ASSERT_EQ(keyComponents->size(), sizeof(SpecificKeyComponents));
    ASSERT_EQ(sizeof(SpecificKeyComponents), sizeof(void*) /*vtable ptr*/);
}

TEST_F(UniversalKeyTest, SizeOfKey) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    auto collation = BSONObj{};
    auto innerComponents = std::make_unique<DummyShapeSpecificComponents>();
    auto shape = std::make_unique<DummyShape>(kDefaultTestNss, collation, *innerComponents);

    auto keyComponents = std::make_unique<DummyKeyComponents>();

    auto key = std::make_unique<DummyKey>(expCtx->getOperationContext(),
                                          std::move(shape),
                                          BSONObj(),
                                          repl::ReadConcernArgs(),
                                          false,
                                          query_shape::CollectionType::kUnknown,
                                          *keyComponents);
    ASSERT_EQ(innerComponents->size(), key->specificComponents().size());
    ASSERT_EQ(sizeof(Key), sizeof(UniversalKeyComponents) + sizeof(void*));
    ASSERT_EQ(key->size(),
              sizeof(Key) + key->universalComponents().size() + key->specificComponents().size());
}
}  // namespace
}  // namespace mongo::query_stats
