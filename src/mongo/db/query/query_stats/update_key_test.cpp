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

using write_ops::UpdateCommandRequest;

class UpdateKeyTest : public ServiceContextTest {
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

TEST_F(UpdateKeyTest, ReplacementUpdateCmdComponents) {
    // Create a request that none of the optional values are set.
    auto ucr = UpdateCommandRequest::parseOwned(fromjson(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" } } ],
        "$db": "testDB"
    })"sv));

    auto updateComponents = std::make_unique<UpdateCmdComponents>(ucr);

    // Confirm all the optional values are still present. They are provided with their default
    // values specified in the IDL.
    BSONObjBuilder bob;
    updateComponents->appendTo(
        bob, query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(fromjson(R"({ ordered: true, bypassDocumentValidation: false })"), bob.obj());
}

TEST_F(UpdateKeyTest, IncludesOptionalValues) {
    // Create a request that all the optional values are included.
    auto ucr = UpdateCommandRequest::parseOwned(fromjson(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: true, upsert: true } ],
        bypassDocumentValidation: true,
        ordered: false,
        "$db": "testDB"
    })"sv));

    auto updateComponents = std::make_unique<UpdateCmdComponents>(ucr);

    // Verify that the optional values are reflected in the stats key components.
    BSONObjBuilder bob;
    updateComponents->appendTo(
        bob, query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(fromjson(R"({ ordered: false, bypassDocumentValidation: true })"), bob.obj());
}

TEST_F(UpdateKeyTest, SizeOfUpdateCmdComponents) {
    auto update = fromjson(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false } ],
        "$db": "testDB"
    })"sv);
    auto ucr = UpdateCommandRequest::parse(std::move(update));

    auto updateComponents = std::make_unique<UpdateCmdComponents>(ucr);

    const auto minimumSize = sizeof(SpecificKeyComponents) + 2 /*size for bools*/;
    ASSERT_GTE(updateComponents->size(), minimumSize);
    ASSERT_LTE(updateComponents->size(), minimumSize + 8 /*padding*/);
}

TEST_F(UpdateKeyTest, EquivalentUpdateCmdComponentSizes) {
    // Create a request that has no values set.
    auto ucrNoSetValues = UpdateCommandRequest::parseOwned(fromjson(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" } } ],
        "$db": "testDB"
    })"sv));

    auto updateComponentsNoValues = std::make_unique<UpdateCmdComponents>(ucrNoSetValues);

    // Create a request that has all values set. None of these should affect the size.
    auto ucrAllValues = UpdateCommandRequest::parseOwned(fromjson(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: true, upsert: true } ],
        bypassDocumentValidation: true,
        ordered: false,
        "$db": "testDB"
    })"sv));

    auto updateComponentsAllValues = std::make_unique<UpdateCmdComponents>(ucrAllValues);

    // Verify their sizes are equal. This is because the optional parameters such as
    // 'bypassDocumentValidation' and 'ordered' are always provided with their default values when
    // they are not set from command requests.
    ASSERT_EQ(updateComponentsAllValues->size(), updateComponentsNoValues->size());
}

// Testing item in opCtx that should impact key size.
TEST_F(UpdateKeyTest, SizeOfUpdateKeyWithAndWithoutComment) {
    auto cmd = R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" } } ],
        "$db": "testDB"
    })"sv;
    auto keyWithoutComment = makeUpdateKey(opCtx(), cmd);

    opCtx()->setComment(BSON("comment" << " foo"));
    auto keyWithComment = makeUpdateKey(opCtx(), cmd);
    ASSERT_LT(keyWithoutComment->size(), keyWithComment->size());
}

// Testing item in command request that should impact key size.
TEST_F(UpdateKeyTest, SizeOfUpdateKeyWithAndWithoutReadConcern) {
    auto keyWithoutReadConcern = makeUpdateKey(opCtx(), R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" } } ],
        "$db": "testDB"
    })"sv);

    auto keyWithReadConcern = makeUpdateKey(opCtx(), R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" } } ],
        "$db": "testDB",
        readConcern: {
            afterClusterTime: Timestamp(1654272333, 13),
            level: "majority"
        }
    })"sv);
    ASSERT_LT(keyWithoutReadConcern->size(), keyWithReadConcern->size());
}
}  // namespace
}  // namespace mongo::query_stats
