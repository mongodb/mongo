/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/query_stats/insert_key.h"

#include "mongo/bson/json.h"
#include "mongo/db/query/query_shape/insert_cmd_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo::query_stats {
namespace {

using write_ops::InsertCommandRequest;

static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");

static constexpr auto kCollectionType = query_shape::CollectionType::kCollection;

class InsertKeyTest : public ServiceContextTest {
public:
    void setUp() override {
        _opCtx = makeOperationContext();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    std::unique_ptr<InsertKey> makeInsertKey(StringData cmd) {
        auto icr = InsertCommandRequest::parseOwned(fromjson(cmd));
        auto shape = std::make_unique<query_shape::InsertCmdShape>(icr);
        return std::make_unique<InsertKey>(opCtx(), icr, std::move(shape), kCollectionType);
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
    })"_sd));

    InsertCmdComponents components(icr);

    BSONObjBuilder bob;
    components.appendTo(bob, SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
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
    })"_sd));

    InsertCmdComponents components(icr);

    BSONObjBuilder bob;
    components.appendTo(bob, SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(fromjson(R"({ ordered: false, bypassDocumentValidation: true })"), bob.obj());
}

TEST_F(InsertKeyTest, SizeOfInsertCmdComponents) {
    auto icr = InsertCommandRequest::parseOwned(fromjson(R"({
        insert: "testColl",
        documents: [ { x: 1 } ],
        "$db": "testDB"
    })"_sd));

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
    })"_sd));
    auto icr2 = InsertCommandRequest::parseOwned(fromjson(R"({
        insert: "testColl",
        documents: [ { b: "hello", c: true }, { d: 42 } ],
        "$db": "testDB"
    })"_sd));

    query_shape::InsertCmdShape shape1(icr1);
    query_shape::InsertCmdShape shape2(icr2);

    // Both shapes should hash to the same value since documents is always a fixed placeholder.
    ASSERT_EQ(absl::HashOf(shape1), absl::HashOf(shape2));
}

}  // namespace
}  // namespace mongo::query_stats
