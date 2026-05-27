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

#include "mongo/db/query/query_stats/delete_key.h"

#include "mongo/bson/json.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/delete_cmd_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <absl/hash/hash.h>

namespace mongo::query_stats {
namespace {

using write_ops::DeleteCommandRequest;

static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");

static constexpr auto kCollectionType = query_shape::CollectionType::kCollection;

class DeleteKeyTest : public ServiceContextTest {
public:
    void setUp() override {
        _opCtx = makeOperationContext();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    std::vector<std::unique_ptr<const Key>> makeDeleteKeys(StringData cmd) {
        auto dcr = DeleteCommandRequest::parseOwned(fromjson(cmd));

        std::vector<std::unique_ptr<const Key>> keys;
        for (const auto& op : dcr.getDeletes()) {
            DeleteRequest request;
            request.setNsString(kDefaultTestNss);
            request.setQuery(op.getQ());
            request.setMulti(op.getMulti());
            if (op.getCollation()) {
                request.setCollation(op.getCollation()->getOwned());
            }
            request.setHint(op.getHint().getOwned());
            if (dcr.getLet()) {
                request.setLet(*dcr.getLet());
            }
            auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx(), request).build();

            auto parsedDelete = uassertStatusOK(parsed_delete_command::parse(
                expCtx, &request, makeExtensionsCallback<ExtensionsCallbackNoop>()));
            auto shape = std::make_unique<query_shape::DeleteCmdShape>(dcr, parsedDelete, expCtx);

            keys.push_back(std::make_unique<DeleteKey>(expCtx,
                                                       dcr,
                                                       parsedDelete.getRequest()->getHint(),
                                                       std::move(shape),
                                                       kCollectionType));
        }
        return keys;
    }

    std::unique_ptr<const Key> makeOneDeleteKey(StringData cmd) {
        auto keys = makeDeleteKeys(cmd);
        ASSERT_EQ(keys.size(), 1U);
        return std::move(keys.front());
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
    components.appendTo(bob, SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
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
    components.appendTo(bob, SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(fromjson(R"({ ordered: false, bypassDocumentValidation: true })"), bob.obj());
}

TEST_F(DeleteKeyTest, SizeOfDeleteCmdComponents) {
    auto dcr = DeleteCommandRequest::parseOwned(fromjson(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB"
    })"_sd));

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
    })"_sd));
    auto deleteComponentsNoValues = std::make_unique<DeleteCmdComponents>(dcrNoSetValues);

    // Create a request that has all values set. None of these should affect the size.
    auto dcrAllValues = DeleteCommandRequest::parseOwned(fromjson(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        bypassDocumentValidation: true,
        ordered: false,
        "$db": "testDB"
    })"_sd));
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

    auto keyWithoutComment = makeOneDeleteKey(cmd);
    opCtx()->setComment(BSON("comment" << "foo"));
    auto keyWithComment = makeOneDeleteKey(cmd);
    ASSERT_LT(keyWithoutComment->size(), keyWithComment->size());
}

TEST_F(DeleteKeyTest, SizeOfDeleteKeyWithAndWithoutReadConcern) {
    auto keyWithoutReadConcern = makeOneDeleteKey(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB"
    })");

    auto keyWithReadConcern = makeOneDeleteKey(R"({
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
    auto keyWithoutHint = makeOneDeleteKey(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB"
    })");

    auto keyWithHint = makeOneDeleteKey(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0, hint: { _id: 1 } } ],
        "$db": "testDB"
    })");
    ASSERT_LT(keyWithoutHint->size(), keyWithHint->size());
}

}  // namespace
}  // namespace mongo::query_stats
