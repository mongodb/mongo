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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {

// Forward declaration — implemented in apply_ops_cmd.cpp
BSONObj removeRidFieldFromOps(const BSONObj& applyOpsObj);

namespace {

// ---------------------------------------------------------------------------
// Single-level
// ---------------------------------------------------------------------------

TEST(RemoveRidFieldFromOpsTest, CommandOpRidIsStripped) {
    // A "c" op that is NOT a nested applyOps must also have its "rid" field removed.
    auto cmd = fromjson("{applyOps: [{op: 'c', ns: 'db.$cmd', o: {create: 'coll'}, rid: 11}]}");
    auto expected = fromjson("{applyOps: [{op: 'c', ns: 'db.$cmd', o: {create: 'coll'}}]}");

    ASSERT_BSONOBJ_EQ(removeRidFieldFromOps(cmd), expected);
}

TEST(RemoveRidFieldFromOpsTest, MultipleInsertUpdateDeleteWithRidAllStripped) {
    // All three write op types in a flat batch: every "rid" must be removed.
    auto cmd = fromjson(
        "{applyOps: ["
        "  {op: 'i', ns: 'db.coll', o: {_id: 1}, rid: 1},"
        "  {op: 'u', ns: 'db.coll', o: {$set: {a: 1}}, o2: {_id: 1}, rid: 2},"
        "  {op: 'd', ns: 'db.coll', o: {_id: 2}, rid: 3}"
        "]}");
    auto expected = fromjson(
        "{applyOps: ["
        "  {op: 'i', ns: 'db.coll', o: {_id: 1}},"
        "  {op: 'u', ns: 'db.coll', o: {$set: {a: 1}}, o2: {_id: 1}},"
        "  {op: 'd', ns: 'db.coll', o: {_id: 2}}"
        "]}");

    ASSERT_BSONOBJ_EQ(removeRidFieldFromOps(cmd), expected);
}

TEST(RemoveRidFieldFromOpsTest, MixOfCRUDWithRidAndCommandOpWithRid) {
    // In a flat batch: both the write op and the command op lose "rid".
    auto cmd = fromjson(
        "{applyOps: ["
        "  {op: 'i', ns: 'db.coll', o: {_id: 1}, rid: 10},"
        "  {op: 'c', ns: 'db.$cmd', o: {create: 'coll'}, rid: 20}"
        "]}");
    auto expected = fromjson(
        "{applyOps: ["
        "  {op: 'i', ns: 'db.coll', o: {_id: 1}},"
        "  {op: 'c', ns: 'db.$cmd', o: {create: 'coll'}}"
        "]}");

    ASSERT_BSONOBJ_EQ(removeRidFieldFromOps(cmd), expected);
}

// ---------------------------------------------------------------------------
// One level of nesting
// ---------------------------------------------------------------------------

TEST(RemoveRidFieldFromOpsTest, NestedApplyOpsMixedInnerOps) {
    // Inside a nested applyOps: both the write op and the command op lose "rid".
    // The outer nested-applyOps wrapper op also loses its own "rid".
    auto cmd = fromjson(
        "{applyOps: [{op: 'c', ns: 'db.$cmd', rid: 5, o: {applyOps: ["
        "  {op: 'i', ns: 'db.coll', o: {_id: 1}, rid: 10},"
        "  {op: 'c', ns: 'db.$cmd', o: {create: 'coll'}, rid: 20}"
        "]}}]}");
    auto expected = fromjson(
        "{applyOps: [{op: 'c', ns: 'db.$cmd', o: {applyOps: ["
        "  {op: 'i', ns: 'db.coll', o: {_id: 1}},"
        "  {op: 'c', ns: 'db.$cmd', o: {create: 'coll'}}"
        "]}}]}");

    ASSERT_BSONOBJ_EQ(removeRidFieldFromOps(cmd), expected);
}

// ---------------------------------------------------------------------------
// Two levels of nesting
// ---------------------------------------------------------------------------

TEST(RemoveRidFieldFromOpsTest, TwoLevelNestedApplyOpsRidStrippedAtEveryLevel) {
    // Level 1: insert with "rid".
    // Level 2 (nested applyOps): delete with "rid".
    // Level 3 (doubly nested applyOps): update with "rid".
    // All three "rid" fields must be removed.
    auto cmd = fromjson(
        "{applyOps: ["
        "  {op: 'i', ns: 'db.coll', o: {_id: 1}, rid: 100},"
        "  {op: 'c', ns: 'db.$cmd', o: {applyOps: ["
        "    {op: 'd', ns: 'db.coll', o: {_id: 2}, rid: 200},"
        "    {op: 'c', ns: 'db.$cmd', o: {applyOps: ["
        "      {op: 'u', ns: 'db.coll', o: {$set: {v: 3}}, o2: {_id: 3}, rid: 300}"
        "    ]}}"
        "  ]}}"
        "]}");
    auto expected = fromjson(
        "{applyOps: ["
        "  {op: 'i', ns: 'db.coll', o: {_id: 1}},"
        "  {op: 'c', ns: 'db.$cmd', o: {applyOps: ["
        "    {op: 'd', ns: 'db.coll', o: {_id: 2}},"
        "    {op: 'c', ns: 'db.$cmd', o: {applyOps: ["
        "      {op: 'u', ns: 'db.coll', o: {$set: {v: 3}}, o2: {_id: 3}}"
        "    ]}}"
        "  ]}}"
        "]}");

    ASSERT_BSONOBJ_EQ(removeRidFieldFromOps(cmd), expected);
}

// ---------------------------------------------------------------------------
// Extra top-level fields (e.g. preCondition) must be preserved
// ---------------------------------------------------------------------------

TEST(RemoveRidFieldFromOpsTest, TopLevelExtraFieldsArePreservedWhileRidIsStripped) {
    // A top-level applyOps command that carries a extra fields alongside
    // the ops. The 'rid' on the insert must be stripped, but the extra fields must
    // be passed through unchanged.
    auto cmd = fromjson(
        "{applyOps: [{op: 'i', ns: 'db.coll', o: {_id: 1}, rid: 42}],"
        " preCondition: [{ns: 'db.coll', q: {_id: 5}, res: {x: 19}}]}");
    auto expected = fromjson(
        "{applyOps: [{op: 'i', ns: 'db.coll', o: {_id: 1}}],"
        " preCondition: [{ns: 'db.coll', q: {_id: 5}, res: {x: 19}}]}");

    ASSERT_BSONOBJ_EQ(removeRidFieldFromOps(cmd), expected);
}

TEST(RemoveRidFieldFromOpsTest, NestedApplyOpsExtraFieldsArePreservedWhileRidIsStripped) {
    // A nested applyOps whose 'o' object contains both an ops array and extra
    // fields. The inner 'rid' must be stripped and the extra fields
    // must be preserved at every level.
    auto cmd = fromjson(
        "{applyOps: [{op: 'c', ns: 'db.$cmd', o: {"
        "  applyOps: [{op: 'd', ns: 'db.coll', o: {_id: 2}, rid: 99}],"
        "  preCondition: []}}],"
        " preCondition: [{ns: 'db.coll', q: {_id: 5}, res: {x: 19}}]}");
    auto expected = fromjson(
        "{applyOps: [{op: 'c', ns: 'db.$cmd', o: {"
        "  applyOps: [{op: 'd', ns: 'db.coll', o: {_id: 2}}],"
        "  preCondition: []}}],"
        " preCondition: [{ns: 'db.coll', q: {_id: 5}, res: {x: 19}}]}");

    ASSERT_BSONOBJ_EQ(removeRidFieldFromOps(cmd), expected);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
