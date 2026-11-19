// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/bulk_write_cmd_shape.h"

#include "mongo/bson/json.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_shape/write_cmd_shape_test_util.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

#include <iostream>
#include <string_view>

#include <absl/hash/hash.h>

namespace mongo::query_shape {
namespace {

using namespace std::literals::string_view_literals;

/**
 * Verifies the query shape of a single bulkWrite op. Unlike the standalone write commands, the
 * 'command' field is always "bulkWrite", an 'op' field carries the op type, and the op's fields
 * keep their bulkWrite names ('filter', 'updateMods') instead of the standalone ones ('q', 'u').
 */
class BulkWriteCmdShapeTest : public unittest::Test {
public:
    void setUp() final {
        _serviceContext = std::make_unique<QueryTestServiceContext>();
        _opCtx = _serviceContext->makeOperationContext();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

private:
    std::unique_ptr<QueryTestServiceContext> _serviceContext;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(BulkWriteCmdShapeTest, UpdateOpShape) {
    auto shape = makeBulkWriteUpdateShape(opCtx(),
                                          R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "bulkWrite",
            "op": "update",
            "filter": {
                "a": { "$eq": 1 }
            },
            "updateMods": {
                "$set": { "b": 1 }
            },
            "multi": false,
            "upsert": false
        })",
        shape->toBson(opCtx(),
                      SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                      SerializationContext::stateDefault()));
}

TEST_F(BulkWriteCmdShapeTest, PipelineUpdateOpShape) {
    auto shape = makeBulkWriteUpdateShape(opCtx(),
                                          R"({
            bulkWrite: 1,
            ops: [ {
                update: 0,
                filter: { $expr: { $gt: [ "$a", "$$lim" ] } },
                updateMods: [ { $set: { b: { $concat: [ "$c", "$$suffix" ] } } } ],
                constants: { suffix: "s" }
            } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            let: { lim: 10 },
            "$db": "admin"
        })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "bulkWrite",
            "op": "update",
            "filter": {
                "$expr": { "$gt": [ "$a", "$$lim" ] }
            },
            "updateMods": [
                { "$set": { "b": { "$concat": [ "$c", "$$suffix" ] } } }
            ],
            "constants": {
                "suffix": "?"
            },
            "multi": false,
            "upsert": false,
            "let": {
                "lim": { "$const": 1 }
            }
        })",
        shape->toBson(opCtx(),
                      SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                      SerializationContext::stateDefault()));
}

TEST_F(BulkWriteCmdShapeTest, ArrayFiltersUpdateOpShape) {
    auto shape = makeBulkWriteUpdateShape(opCtx(),
                                          R"({
            bulkWrite: 1,
            ops: [ {
                update: 0,
                filter: { a: 1 },
                updateMods: { $set: { "arr.$[e]": 2 } },
                arrayFilters: [ { e: { $gt: 5 } } ]
            } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "bulkWrite",
            "op": "update",
            "filter": {
                "a": { "$eq": 1 }
            },
            "updateMods": {
                "$set": { "arr.$[e]": 1 }
            },
            "arrayFilters": [
                { "e": { "$gt": 1 } }
            ],
            "multi": false,
            "upsert": false
        })",
        shape->toBson(opCtx(),
                      SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                      SerializationContext::stateDefault()));
}

TEST_F(BulkWriteCmdShapeTest, UpdateOpSortExcludedFromShape) {
    // 'sort' is not part of the standalone update shape, so it must not be part of the bulkWrite
    // op shape either.
    auto withSort = makeBulkWriteUpdateShape(opCtx(),
                                             R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } }, sort: { a: 1 } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto withoutSort = makeBulkWriteUpdateShape(opCtx(),
                                                R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);

    auto serializationContext = SerializationContext::stateDefault();
    ASSERT_BSONOBJ_EQ(
        withoutSort->toBson(opCtx(),
                            SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                            serializationContext),
        withSort->toBson(opCtx(),
                         SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                         serializationContext));
    ASSERT_EQ(withoutSort->sha256Hash(opCtx(), serializationContext),
              withSort->sha256Hash(opCtx(), serializationContext));
}

TEST_F(BulkWriteCmdShapeTest, HintExcludedFromShape) {
    // 'hint' is part of the query stats key, not the query shape.
    auto serializationContext = SerializationContext::stateDefault();
    auto updateWithHint = makeBulkWriteUpdateShape(opCtx(),
                                                   R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } },
                     hint: { a: 1 } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto updateWithoutHint = makeBulkWriteUpdateShape(opCtx(),
                                                      R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    ASSERT_BSONOBJ_EQ(
        updateWithoutHint->toBson(opCtx(),
                                  SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                                  serializationContext),
        updateWithHint->toBson(opCtx(),
                               SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                               serializationContext));
    ASSERT_EQ(updateWithoutHint->sha256Hash(opCtx(), serializationContext),
              updateWithHint->sha256Hash(opCtx(), serializationContext));

    auto deleteWithHint = makeBulkWriteDeleteShape(opCtx(),
                                                   R"({
            bulkWrite: 1,
            ops: [ { delete: 0, filter: { a: 1 }, hint: { a: 1 } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto deleteWithoutHint = makeBulkWriteDeleteShape(opCtx(),
                                                      R"({
            bulkWrite: 1,
            ops: [ { delete: 0, filter: { a: 1 } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    ASSERT_BSONOBJ_EQ(
        deleteWithoutHint->toBson(opCtx(),
                                  SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                                  serializationContext),
        deleteWithHint->toBson(opCtx(),
                               SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                               serializationContext));
    ASSERT_EQ(deleteWithoutHint->sha256Hash(opCtx(), serializationContext),
              deleteWithHint->sha256Hash(opCtx(), serializationContext));
}

TEST_F(BulkWriteCmdShapeTest, UpdateOpCollationInShape) {
    auto shape = makeBulkWriteUpdateShape(opCtx(),
                                          R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } },
                     collation: { locale: "fr" } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    ASSERT_BSONOBJ_EQ(fromjson(R"({locale: "fr"})"),
                      shape
                          ->toBson(opCtx(),
                                   SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                                   SerializationContext::stateDefault())
                          .getObjectField("collation"));
}

TEST_F(BulkWriteCmdShapeTest, DeleteOpShape) {
    auto shape = makeBulkWriteDeleteShape(opCtx(),
                                          R"({
            bulkWrite: 1,
            ops: [ { delete: 0, filter: { a: 1 }, multi: true } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            let: { v: 4 },
            "$db": "admin"
        })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "bulkWrite",
            "op": "delete",
            "filter": {
                "a": { "$eq": 1 }
            },
            "multi": true,
            "let": {
                "v": { "$const": 1 }
            }
        })",
        shape->toBson(opCtx(),
                      SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                      SerializationContext::stateDefault()));
}

TEST_F(BulkWriteCmdShapeTest, InsertOpShape) {
    auto shape = makeBulkWriteInsertShape(opCtx(),
                                          R"({
            bulkWrite: 1,
            ops: [ { insert: 0, document: { a: 1, b: "hello" } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "bulkWrite",
            "op": "insert",
            "documents": [
                { "?": "?" }
            ]
        })",
        shape->toBson(opCtx(),
                      SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                      SerializationContext::stateDefault()));
}

// The representative shapes are re-parsed and re-shapified for the debug serialization options.
TEST_F(BulkWriteCmdShapeTest, UpdateOpDebugShape) {
    auto shape = makeBulkWriteUpdateShape(opCtx(),
                                          R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { x: { $eq: 3 } }, updateMods: { $set: { b: 2 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "bulkWrite",
            "op": "update",
            "filter": {
                "x": { "$eq": "?number" }
            },
            "updateMods": {
                "$set": { "b": "?number" }
            },
            "multi": false,
            "upsert": false
        })",
        shape->toBson(opCtx(),
                      SerializationOptions::kDebugQueryShapeSerializeOptions,
                      SerializationContext::stateDefault()));
}

TEST_F(BulkWriteCmdShapeTest, DeleteOpDebugShape) {
    auto shape = makeBulkWriteDeleteShape(opCtx(),
                                          R"({
            bulkWrite: 1,
            ops: [ { delete: 0, filter: { x: { $eq: 3 } }, collation: { locale: "fr" } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            let: { v: 4 },
            "$db": "admin"
        })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "collation": {
                "locale": "fr"
            },
            "command": "bulkWrite",
            "op": "delete",
            "filter": {
                "x": { "$eq": "?number" }
            },
            "multi": false,
            "let": {
                "v": "?number"
            }
        })",
        shape->toBson(opCtx(),
                      SerializationOptions::kDebugQueryShapeSerializeOptions,
                      SerializationContext::stateDefault()));
}

TEST_F(BulkWriteCmdShapeTest, SameShapeDifferentLiteralsHashEqual) {
    auto serializationContext = SerializationContext::stateDefault();
    auto shape1 = makeBulkWriteUpdateShape(opCtx(),
                                           R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto shape2 = makeBulkWriteUpdateShape(opCtx(),
                                           R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 3 }, updateMods: { $set: { b: 4 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);

    ASSERT_EQ(shape1->sha256Hash(opCtx(), serializationContext),
              shape2->sha256Hash(opCtx(), serializationContext));
}

TEST_F(BulkWriteCmdShapeTest, DifferentFilterHashesDiffer) {
    auto serializationContext = SerializationContext::stateDefault();
    auto shape1 = makeBulkWriteUpdateShape(opCtx(),
                                           R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto shape2 = makeBulkWriteUpdateShape(opCtx(),
                                           R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { c: 1 }, updateMods: { $set: { b: 2 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);

    ASSERT_NOT_EQUALS(shape1->sha256Hash(opCtx(), serializationContext),
                      shape2->sha256Hash(opCtx(), serializationContext));
}

TEST_F(BulkWriteCmdShapeTest, DifferentNamespaceHashesDiffer) {
    auto serializationContext = SerializationContext::stateDefault();
    constexpr auto kTwoNamespaces = R"({
            bulkWrite: 1,
            ops: [
                { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } } },
                { update: 1, filter: { a: 1 }, updateMods: { $set: { b: 2 } } },
                { delete: 0, filter: { a: 1 } },
                { delete: 1, filter: { a: 1 } },
                { insert: 0, document: { a: 1 } },
                { insert: 1, document: { a: 1 } }
            ],
            nsInfo: [ { ns: "testDB.testColl" }, { ns: "testDB.otherColl" } ],
            "$db": "admin"
        })"sv;
    ASSERT_NOT_EQUALS(makeBulkWriteUpdateShape(opCtx(), kTwoNamespaces, 0)
                          ->sha256Hash(opCtx(), serializationContext),
                      makeBulkWriteUpdateShape(opCtx(), kTwoNamespaces, 1)
                          ->sha256Hash(opCtx(), serializationContext));
    ASSERT_NOT_EQUALS(makeBulkWriteDeleteShape(opCtx(), kTwoNamespaces, 2)
                          ->sha256Hash(opCtx(), serializationContext),
                      makeBulkWriteDeleteShape(opCtx(), kTwoNamespaces, 3)
                          ->sha256Hash(opCtx(), serializationContext));
    ASSERT_NOT_EQUALS(makeBulkWriteInsertShape(opCtx(), kTwoNamespaces, 4)
                          ->sha256Hash(opCtx(), serializationContext),
                      makeBulkWriteInsertShape(opCtx(), kTwoNamespaces, 5)
                          ->sha256Hash(opCtx(), serializationContext));
}

TEST_F(BulkWriteCmdShapeTest, DifferentOpTypeHashesDiffer) {
    auto serializationContext = SerializationContext::stateDefault();
    auto updateShape = makeBulkWriteUpdateShape(opCtx(),
                                                R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto deleteShape = makeBulkWriteDeleteShape(opCtx(),
                                                R"({
            bulkWrite: 1,
            ops: [ { delete: 0, filter: { a: 1 } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);

    ASSERT_NOT_EQUALS(updateShape->sha256Hash(opCtx(), serializationContext),
                      deleteShape->sha256Hash(opCtx(), serializationContext));
}

// The 'multi' and 'upsert' flags are hashed as bits of a single word; verify each bit
// differentiates the hash.
TEST_F(BulkWriteCmdShapeTest, UpdateOpMultiFlagHashesDiffer) {
    auto serializationContext = SerializationContext::stateDefault();
    auto multiShape = makeBulkWriteUpdateShape(opCtx(),
                                               R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } }, multi: true } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto singleShape = makeBulkWriteUpdateShape(opCtx(),
                                                R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);

    ASSERT_NOT_EQUALS(multiShape->sha256Hash(opCtx(), serializationContext),
                      singleShape->sha256Hash(opCtx(), serializationContext));
}

TEST_F(BulkWriteCmdShapeTest, UpdateOpUpsertFlagHashesDiffer) {
    auto serializationContext = SerializationContext::stateDefault();
    auto upsertShape = makeBulkWriteUpdateShape(opCtx(),
                                                R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } }, upsert: true } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto plainShape = makeBulkWriteUpdateShape(opCtx(),
                                               R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);

    ASSERT_NOT_EQUALS(upsertShape->sha256Hash(opCtx(), serializationContext),
                      plainShape->sha256Hash(opCtx(), serializationContext));
}

TEST_F(BulkWriteCmdShapeTest, DeleteOpMultiFlagHashesDiffer) {
    auto serializationContext = SerializationContext::stateDefault();
    auto multiShape = makeBulkWriteDeleteShape(opCtx(),
                                               R"({
            bulkWrite: 1,
            ops: [ { delete: 0, filter: { a: 1 }, multi: true } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto singleShape = makeBulkWriteDeleteShape(opCtx(),
                                                R"({
            bulkWrite: 1,
            ops: [ { delete: 0, filter: { a: 1 } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);

    ASSERT_NOT_EQUALS(multiShape->sha256Hash(opCtx(), serializationContext),
                      singleShape->sha256Hash(opCtx(), serializationContext));
}

// Under option D the bulkWrite op shape intentionally differs from the standalone write shape: the
// command name is "bulkWrite" and the fields keep the bulkWrite names, so the hashes differ.
TEST_F(BulkWriteCmdShapeTest, UpdateOpHashDiffersFromStandalone) {
    auto serializationContext = SerializationContext::stateDefault();
    auto bulkShape = makeBulkWriteUpdateShape(opCtx(),
                                              R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto standaloneShape = makeOneUpdateShape(opCtx(),
                                              R"({
            update: "testColl",
            updates: [ { q: { a: 1 }, u: { $set: { b: 2 } } } ],
            "$db": "testDB"
        })"sv);

    ASSERT_NOT_EQUALS(bulkShape->sha256Hash(opCtx(), serializationContext),
                      standaloneShape.sha256Hash(opCtx(), serializationContext));
}

TEST_F(BulkWriteCmdShapeTest, DeleteOpHashDiffersFromStandalone) {
    auto serializationContext = SerializationContext::stateDefault();
    auto bulkShape = makeBulkWriteDeleteShape(opCtx(),
                                              R"({
            bulkWrite: 1,
            ops: [ { delete: 0, filter: { a: 1 } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto standaloneShape = makeOneDeleteShape(opCtx(),
                                              R"({
            delete: "testColl",
            deletes: [ { q: { a: 1 }, limit: 1 } ],
            "$db": "testDB"
        })"sv);

    ASSERT_NOT_EQUALS(bulkShape->sha256Hash(opCtx(), serializationContext),
                      standaloneShape.sha256Hash(opCtx(), serializationContext));
}

TEST_F(BulkWriteCmdShapeTest, InsertOpHashDiffersFromStandalone) {
    auto serializationContext = SerializationContext::stateDefault();
    auto bulkShape = makeBulkWriteInsertShape(opCtx(),
                                              R"({
            bulkWrite: 1,
            ops: [ { insert: 0, document: { a: 1 } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto standaloneShape = makeInsertShape(R"({
        insert: "testColl",
        documents: [ { a: 1 } ],
        "$db": "testDB"
    })"sv);

    ASSERT_NOT_EQUALS(bulkShape->sha256Hash(opCtx(), serializationContext),
                      standaloneShape.sha256Hash(opCtx(), serializationContext));
}

// The in-memory (Abseil) hash keys the query stats store, so a bulkWrite op must not hash like the
// equivalent standalone write, nor like another bulkWrite op type.
TEST_F(BulkWriteCmdShapeTest, AbslHashDiffersFromStandaloneAndOtherOps) {
    auto bulkUpdate = makeBulkWriteUpdateShape(opCtx(),
                                               R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto standaloneUpdate = makeOneUpdateShape(opCtx(),
                                               R"({
            update: "testColl",
            updates: [ { q: { a: 1 }, u: { $set: { b: 2 } } } ],
            "$db": "testDB"
        })"sv);
    auto bulkDelete = makeBulkWriteDeleteShape(opCtx(),
                                               R"({
            bulkWrite: 1,
            ops: [ { delete: 0, filter: { a: 1 } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto standaloneDelete = makeOneDeleteShape(opCtx(),
                                               R"({
            delete: "testColl",
            deletes: [ { q: { a: 1 }, limit: 1 } ],
            "$db": "testDB"
        })"sv);
    auto bulkInsert = makeBulkWriteInsertShape(opCtx(),
                                               R"({
            bulkWrite: 1,
            ops: [ { insert: 0, document: { a: 1 } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto standaloneInsert = makeInsertShape(R"({
        insert: "testColl",
        documents: [ { a: 1 } ],
        "$db": "testDB"
    })"sv);

    auto hashOf = [](const Shape& shape) {
        return absl::HashOf(shape);
    };
    ASSERT_NOT_EQUALS(hashOf(*bulkUpdate), hashOf(standaloneUpdate));
    ASSERT_NOT_EQUALS(hashOf(*bulkDelete), hashOf(standaloneDelete));
    ASSERT_NOT_EQUALS(hashOf(*bulkInsert), hashOf(standaloneInsert));

    ASSERT_NOT_EQUALS(hashOf(*bulkUpdate), hashOf(*bulkDelete));
    ASSERT_NOT_EQUALS(hashOf(*bulkUpdate), hashOf(*bulkInsert));
    ASSERT_NOT_EQUALS(hashOf(*bulkDelete), hashOf(*bulkInsert));
}

// The op type alone distinguishes otherwise identical components.
TEST(BulkWriteCmdShapeComponentsTest, AbslHashIncludesOpType) {
    auto hashOf = [](const CmdSpecificShapeComponents& components) {
        return absl::HashOf(components);
    };
    using InsertComponentsAsUpdateOp =
        BulkWriteCmdShapeComponents<InsertCmdShapeComponents, kBulkWriteUpdateOpName>;
    ASSERT_NOT_EQUALS(hashOf(BulkWriteInsertCmdShapeComponents{}),
                      hashOf(InsertComponentsAsUpdateOp{}));
}

// The wrapped components report the standalone components' size plus only the wrapper's overhead.
TEST_F(BulkWriteCmdShapeTest, ComponentsSizeIncludesStandaloneComponents) {
    auto bulkUpdate = makeBulkWriteUpdateShape(opCtx(),
                                               R"({
            bulkWrite: 1,
            ops: [ { update: 0, filter: { a: 1 }, updateMods: { $set: { b: 2 } } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto standaloneUpdate = makeOneUpdateShape(opCtx(),
                                               R"({
            update: "testColl",
            updates: [ { q: { a: 1 }, u: { $set: { b: 2 } } } ],
            "$db": "testDB"
        })"sv);
    ASSERT_EQ(bulkUpdate->specificComponents().size(),
              standaloneUpdate.specificComponents().size() +
                  sizeof(BulkWriteUpdateCmdShapeComponents) - sizeof(UpdateCmdShapeComponents));

    auto bulkDelete = makeBulkWriteDeleteShape(opCtx(),
                                               R"({
            bulkWrite: 1,
            ops: [ { delete: 0, filter: { a: 1 } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],
            "$db": "admin"
        })"sv);
    auto standaloneDelete = makeOneDeleteShape(opCtx(),
                                               R"({
            delete: "testColl",
            deletes: [ { q: { a: 1 }, limit: 1 } ],
            "$db": "testDB"
        })"sv);
    ASSERT_EQ(bulkDelete->specificComponents().size(),
              standaloneDelete.specificComponents().size() +
                  sizeof(BulkWriteDeleteCmdShapeComponents) - sizeof(DeleteCmdShapeComponents));
}

}  // namespace
}  // namespace mongo::query_shape
