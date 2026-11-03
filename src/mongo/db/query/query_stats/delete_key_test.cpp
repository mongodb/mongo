// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_stats/write_key.h"
#include "mongo/db/query/query_stats/write_key_test_util.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::query_stats {
namespace {
using namespace std::literals::string_view_literals;

using write_ops::DeleteCommandRequest;

class DeleteKeyTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = makeOperationContext();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(DeleteKeyTest, DefaultDeleteCmdComponents) {
    auto dcr = DeleteCommandRequest::parseOwned(fromjson(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB"
    })"));

    DeleteCmdComponents components(dcr);

    BSONObjBuilder bob;
    components.appendTo(
        bob, query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(fromjson(R"({ ordered: true, bypassDocumentValidation: false })"), bob.obj());
}

TEST_F(DeleteKeyTest, DeleteCmdComponentsWithExplicitValues) {
    auto dcr = DeleteCommandRequest::parseOwned(fromjson(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        ordered: false,
        bypassDocumentValidation: true,
        "$db": "testDB"
    })"));

    DeleteCmdComponents components(dcr);

    BSONObjBuilder bob;
    components.appendTo(
        bob, query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(fromjson(R"({ ordered: false, bypassDocumentValidation: true })"), bob.obj());
}

TEST_F(DeleteKeyTest, SizeOfDeleteCmdComponents) {
    auto dcr = DeleteCommandRequest::parseOwned(fromjson(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB"
    })"sv));

    DeleteCmdComponents components(dcr);

    const auto minimumSize = sizeof(SpecificKeyComponents) + 2 /* size for bools */;
    ASSERT_GTE(components.size(), minimumSize);
    ASSERT_LTE(components.size(), minimumSize + 8 /* padding */);
}

TEST_F(DeleteKeyTest, EquivalentDeleteCmdComponentSizes) {
    // Create a request that has no values set.
    auto dcrNoSetValues = DeleteCommandRequest::parseOwned(fromjson(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB"
    })"sv));
    auto deleteComponentsNoValues = std::make_unique<DeleteCmdComponents>(dcrNoSetValues);

    // Create a request that has all values set. None of these should affect the size.
    auto dcrAllValues = DeleteCommandRequest::parseOwned(fromjson(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        bypassDocumentValidation: true,
        ordered: false,
        "$db": "testDB"
    })"sv));
    auto deleteComponentsAllValues = std::make_unique<DeleteCmdComponents>(dcrAllValues);

    // Verify their sizes are equal. This is because the optional parameters such as
    // 'bypassDocumentValidation' and 'ordered' are always provided with their default values when
    // they are not set from command requests.
    ASSERT_EQ(deleteComponentsAllValues->size(), deleteComponentsNoValues->size());
}

TEST_F(DeleteKeyTest, SizeOfDeleteKeyWithAndWithoutComment) {
    auto cmd = R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB"
    })";

    auto keyWithoutComment = makeDeleteKey(opCtx(), cmd);
    opCtx()->setComment(BSON("comment" << "foo"));
    auto keyWithComment = makeDeleteKey(opCtx(), cmd);
    ASSERT_LT(keyWithoutComment->size(), keyWithComment->size());
}

TEST_F(DeleteKeyTest, SizeOfDeleteKeyWithAndWithoutReadConcern) {
    auto keyWithoutReadConcern = makeDeleteKey(opCtx(), R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB"
    })");

    auto keyWithReadConcern = makeDeleteKey(opCtx(), R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB",
        readConcern: {
            afterClusterTime: Timestamp(1654272333, 13),
            level: "majority"
        }
    })");
    ASSERT_LT(keyWithoutReadConcern->size(), keyWithReadConcern->size());
}

TEST_F(DeleteKeyTest, SizeOfDeleteKeyWithAndWithoutHint) {
    auto keyWithoutHint = makeDeleteKey(opCtx(), R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB"
    })");

    auto keyWithHint = makeDeleteKey(opCtx(), R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0, hint: { _id: 1 } } ],
        "$db": "testDB"
    })");
    ASSERT_LT(keyWithoutHint->size(), keyWithHint->size());
}

}  // namespace
}  // namespace mongo::query_stats
