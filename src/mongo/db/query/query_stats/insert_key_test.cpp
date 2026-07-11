// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/db/query/query_bm_constants.h"
#include "mongo/db/query/query_shape/insert_cmd_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_stats/write_key.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::query_stats {
namespace {

using write_ops::InsertCommandRequest;
using namespace std::literals::string_view_literals;

class InsertKeyTest : public ServiceContextTest {
public:
    void setUp() override {
        _opCtx = makeOperationContext();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    std::unique_ptr<InsertKey> makeInsertKey(std::string_view cmd) {
        auto icr = InsertCommandRequest::parseOwned(fromjson(cmd));
        auto shape = std::make_unique<query_shape::InsertCmdShape>(icr);
        return std::make_unique<InsertKey>(
            opCtx(), icr, std::move(shape), query_benchmark_constants::kCollectionType);
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(InsertKeyTest, DefaultInsertCmdComponents) {
    // Create a request with default values.
    auto icr = InsertCommandRequest::parseOwned(fromjson(R"({
        insert: "testColl",
        documents: [ { x: 1 } ],
        "$db": "testDB"
    })"sv));

    InsertCmdComponents components(icr);

    BSONObjBuilder bob;
    components.appendTo(
        bob, query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(fromjson(R"({ ordered: true, bypassDocumentValidation: false })"), bob.obj());
}

TEST_F(InsertKeyTest, InsertCmdComponentsWithExplicitValues) {
    // Create a request with all optional values set explicitly.
    auto icr = InsertCommandRequest::parseOwned(fromjson(R"({
        insert: "testColl",
        documents: [ { x: 1 } ],
        ordered: false,
        bypassDocumentValidation: true,
        "$db": "testDB"
    })"sv));

    InsertCmdComponents components(icr);

    BSONObjBuilder bob;
    components.appendTo(
        bob, query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(fromjson(R"({ ordered: false, bypassDocumentValidation: true })"), bob.obj());
}

TEST_F(InsertKeyTest, SizeOfInsertCmdComponents) {
    auto icr = InsertCommandRequest::parseOwned(fromjson(R"({
        insert: "testColl",
        documents: [ { x: 1 } ],
        "$db": "testDB"
    })"sv));

    InsertCmdComponents components(icr);

    const auto minimumSize = sizeof(SpecificKeyComponents) + 2 /* size for bools */;
    ASSERT_GTE(components.size(), minimumSize);
    ASSERT_LTE(components.size(), minimumSize + 8 /* padding */);
}

TEST_F(InsertKeyTest, InsertKeySize) {
    auto key1 = makeInsertKey(R"({
        insert: "testColl",
        documents: [ { x: 1 } ],
        "$db": "testDB"
    })");

    auto key2 = makeInsertKey(R"({
        insert: "testColl",
        documents: [ { x: 1, y: 2, z: 3 } ],
        "$db": "testDB"
    })");

    // Shape is always a fixed placeholder regardless of document contents, so sizes should match.
    ASSERT_EQ(key1->size(), key2->size());
}

TEST_F(InsertKeyTest, InsertKeyShapeDocumentsIsAlwaysPlaceholder) {
    // Verify that the shape always represents documents as a fixed placeholder,
    // regardless of the actual document contents.
    auto icr1 = InsertCommandRequest::parseOwned(fromjson(R"({
        insert: "testColl",
        documents: [ { a: 1 } ],
        "$db": "testDB"
    })"sv));
    auto icr2 = InsertCommandRequest::parseOwned(fromjson(R"({
        insert: "testColl",
        documents: [ { b: "hello", c: true }, { d: 42 } ],
        "$db": "testDB"
    })"sv));

    query_shape::InsertCmdShape shape1(icr1);
    query_shape::InsertCmdShape shape2(icr2);

    // Both shapes should hash to the same value since documents is always a fixed placeholder.
    ASSERT_EQ(absl::HashOf(shape1), absl::HashOf(shape2));
}

}  // namespace
}  // namespace mongo::query_stats
