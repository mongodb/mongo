// @tags: [
//  requires_non_retryable_writes,
//  requires_fastcount,
//  requires_getmore,
//  # TODO SERVER-113670: Investigate why this test fails with primary_driven_index_builds suites.
//  primary_driven_index_builds_incompatible,
// ]

import {IndexUtils} from "jstests/libs/index_utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

let t = db.index_sparse1;
t.drop();

t.insert({_id: 1, x: 1});
t.insert({_id: 2, x: 2});
t.insert({_id: 3, x: 2});
t.insert({_id: 4});
t.insert({_id: 5});

assert.eq(5, t.count(), "A1");
assert.eq(5, t.find().sort({x: 1}).itcount(), "A2");

t.createIndex({x: 1});
IndexUtils.assertIndexes(t, [{_id: 1}, {x: 1}], "B1");
assert.eq(5, t.find().sort({x: 1}).itcount(), "B2");
t.dropIndex({x: 1});
IndexUtils.assertIndexes(t, [{_id: 1}], "B3");

t.createIndex({x: 1}, {sparse: 1});
IndexUtils.assertIndexes(t, [{_id: 1}, {x: 1}], "C1");
assert.eq(5, t.find().sort({x: 1}).itcount(), "C2");
t.dropIndex({x: 1});
IndexUtils.assertIndexes(t, [{_id: 1}], "C3");

// -- sparse & unique

t.remove({_id: 2});

// Test that we can't create a unique index without sparse
assert.commandFailedWithCode(
    t.createIndex({x: 1}, {unique: 1}),
    [ErrorCodes.DuplicateKey, ErrorCodes.CannotCreateIndex],
    "D1",
);
IndexUtils.assertIndexes(t, [{_id: 1}], "D2");

if (!FixtureHelpers.isSharded(t)) {
    // Can't create unique indexes on sharded collections.

    t.createIndex({x: 1}, {unique: 1, sparse: 1});
    IndexUtils.assertIndexes(t, [{_id: 1}, {x: 1}], "E1");
    t.dropIndex({x: 1});
    IndexUtils.assertIndexes(t, [{_id: 1}], "E2");

    t.insert({_id: 2, x: 2});
    assert.commandFailedWithCode(
        t.createIndex({x: 1}, {unique: 1, sparse: 1}),
        [ErrorCodes.DuplicateKey, ErrorCodes.CannotCreateIndex],
        "E3",
    );
    IndexUtils.assertIndexes(t, [{_id: 1}], "E4");
    assert.eq(1, t.getIndexes().length, "E5");
}
