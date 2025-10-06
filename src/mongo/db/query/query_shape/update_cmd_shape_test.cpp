/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape/update_cmd_shape.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo::query_shape {
namespace {

using write_ops::UpdateCommandRequest;

static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");

class UpdateCmdShapeTest : public unittest::Test {
public:
    void setUp() final {
        _queryTestServiceContext = std::make_unique<QueryTestServiceContext>();
        _operationContext = _queryTestServiceContext->makeOperationContext();
        _expCtx = make_intrusive<ExpressionContextForTest>();
    }

    std::vector<UpdateCmdShape> makeShapesFromUpdate(StringData updateCmd) {
        auto updateRequest = UpdateCommandRequest::parseOwned(fromjson(updateCmd));

        std::vector<UpdateCmdShape> shapes;
        for (const auto& op : updateRequest.getUpdates()) {
            UpdateRequest request(op);
            request.setNamespaceString(kDefaultTestNss);
            if (updateRequest.getLet()) {
                request.setLetParameters(*updateRequest.getLet());
            }

            ParsedUpdate parsedUpdate(
                _operationContext.get(), &request, CollectionPtr::null, false);
            ASSERT_OK(parsedUpdate.parseRequest());
            shapes.emplace_back(updateRequest, parsedUpdate, _expCtx);
        }
        return shapes;
    }

    UpdateCmdShape makeOneShapeFromUpdate(StringData updateCmd) {
        auto shapes = makeShapesFromUpdate(updateCmd);
        ASSERT_EQ(shapes.size(), 1);
        return shapes.front();
    }

    std::unique_ptr<QueryTestServiceContext> _queryTestServiceContext;

    ServiceContext::UniqueOperationContext _operationContext;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(UpdateCmdShapeTest, BasicReplacementUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false } ],
        "$db": "testDB"
    })"_sd);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": "?number"
                }
            },
            "u": "?object",
            "multi": false,
            "upsert": false 
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": 1
                }
            },
            "u": {
                "?": "?"
            },
            "multi": false,
            "upsert": false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, BatchReplacementUpdateShape) {
    auto shapes = makeShapesFromUpdate(R"({
        update: "testColl",
        updates: [
          { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false },
          { q: { x: {$gt: 3}, y: "foo" }, u: { x: {y: 100}, z: false }, multi: false, upsert: true }
        ],
        "$db": "testDB"
    })"_sd);
    ASSERT_EQ(shapes.size(), 2);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": "?number"
                }
            },
            "u": "?object",
            "multi": false,
            "upsert": false
        })",
        shapes[0].toBson(_operationContext.get(),
                         SerializationOptions::kDebugQueryShapeSerializeOptions,
                         SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "$and": [
                    {
                        "y": {
                            "$eq": "?string"
                        }
                    },
                    {
                        "x": {
                            "$gt": "?number"
                        }
                    }
                ]
            },
            "u": "?object",
            "multi": false,
            "upsert": true
        })",
        shapes[1].toBson(_operationContext.get(),
                         SerializationOptions::kDebugQueryShapeSerializeOptions,
                         SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, IncludesOptionalValues) {
    // Test setting optional values such as 'multi' and 'upsert' to verify that they are included in
    // the query shape.
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: true } ],
        ordered: false,
        bypassDocumentValidation: true,
        let: {x: 4, y: "abc"},
        "$db": "testDB"
    })"_sd);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": "?number"
                }
            },
            "u": "?object",
            "multi": false,
            "upsert": true,
            "let": {
                "x": "?number",
                "y": "?string"
            }
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

// Verifies that "update" command shape hash value is stable (does not change between the
// versions of the server).
TEST_F(UpdateCmdShapeTest, StableQueryShapeHashValue) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false } ],
        "$db": "testDB"
    })"_sd);

    auto serializationContext = SerializationContext::stateCommandRequest();
    const auto hash = shape.sha256Hash(_operationContext.get(), serializationContext);
    ASSERT_EQ("56593B6B2CE6C3968E03CC55DFED93AE728CA730A21E0659390360636BD96B15",
              hash.toHexString());
}

TEST_F(UpdateCmdShapeTest, SizeOfUpdateCmdShapeComponents) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false } ],
        "$db": "testDB"
    })"_sd);
    auto updateComponents =
        static_cast<const UpdateCmdShapeComponents&>(shape.specificComponents());

    // The sizes of any members of UpdateCmdShapeComponents are typically accounted for by
    // sizeof(UpdateCmdShapeComponents). The important part of the test here is to ensure that
    // any additional memory allocations are also included in the size() operation.
    const auto letSize = updateComponents.let.size();

    ASSERT_EQ(updateComponents.size(),
              sizeof(UpdateCmdShapeComponents) + updateComponents.representativeQ.objsize() +
                  updateComponents._representativeUObj.objsize() + letSize -
                  sizeof(LetShapeComponent));
}

TEST_F(UpdateCmdShapeTest, EquivalentUpdateCmdShapeSizes) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false } ],
        "$db": "testDB"
    })"_sd);
    auto shapeOptionalValues = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: true } ],
        "$db": "testDB",
        ordered: false,
        bypassDocumentValidation: true
    })"_sd);
    ASSERT_EQ(shape.size(), shapeOptionalValues.size());
}
}  // namespace
}  // namespace mongo::query_shape
