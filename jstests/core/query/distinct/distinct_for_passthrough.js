/*
 * Tests various distinct-like query edge cases on passthrough suites- this does not check explain,
 * only validates that queries produce correct outputs under a variety of conditions. This test only
 * works on sharded passthroughs when shard-filtering distinct scan is enabled (otherwise orphans
 * may appear).
 *
 * @tags: [featureFlagShardFilteringDistinctScan, requires_fcv_81]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.drop();

function commandWorked(cmd) {
    const res = assert.commandWorked(db.runCommand(cmd));
    if (cmd.distinct) {
        return res.values;
    } else if (cmd.aggregate) {
        return res.cursor.firstBatch;
    }
    return res;
}

function ensureResults(cmd, expected) {
    assertArrayEq({actual: commandWorked(cmd), expected});
}

function testCmd(cmd, expected, hints = []) {
    ensureResults(cmd, expected);
    // Now repeat to test caching of plan!
    ensureResults(cmd, expected);
    ensureResults(cmd, expected);
    // Now test hinted plans.
    for (const hint of hints) {
        cmd.hint = hint;
        ensureResults(cmd, expected);
    }
}

function testAggAndDistinct(key, query, expected, hints = []) {
    testCmd({distinct: coll.getName(), key, query}, expected, hints);
    const pipeline = [];
    if (query) {
        pipeline.push({$match: query});
    }
    pipeline.push({$group: {_id: `$${key}`}});
    testCmd({aggregate: coll.getName(), pipeline, cursor: {}},
            expected.map(e => {
                return {_id: e};
            }),
            hints);
}

{
    //
    // Test distinct() with $regex.
    //
    assert.commandWorked(coll.insertMany(
        [{a: "abc", b: "foo"}, {a: "abc", b: "bar"}, {a: "abd", b: "far"}, {a: "aeb", b: "car"}]));
    assert.commandWorked(coll.createIndex({a: 1}));
    testAggAndDistinct("a", {a: {"$regex": "^ab.*"}}, ["abc", "abd"], [{a: 1}, {$natural: 1}], []);
}

assert(coll.drop());

{
    //
    // Test distinct() with hashed index.
    //
    const docs = [];
    const aValues = [];
    const bValues = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];
    bValues.push(...bValues.slice().map(b => {
        return {subObj: "str_" + b};
    }));
    const cValues = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
    for (let i = 0; i < 100; i++) {
        docs.push({a: i, b: {subObj: "str_" + (i % 13)}, c: NumberInt(i % 10)});
        docs.push({a: i, b: (i % 13), c: NumberInt(i % 10)});
        aValues.push(i);
    }
    assert.commandWorked(coll.insertMany(docs));

    //
    // Tests for 'distinct' operation when hashed field is not a prefix.
    //
    assert.commandWorked(coll.createIndex({a: -1, b: "hashed", c: 1}));

    testAggAndDistinct("a", {a: {$gt: 50, $lt: 55}}, [51, 52, 53, 54]);
    testAggAndDistinct("b", undefined, bValues);
    testAggAndDistinct("c", undefined, cValues);
    testAggAndDistinct("c", {a: 12, b: {subObj: "str_12"}}, [2]);
    testAggAndDistinct("c", {a: 12, b: 4}, []);

    //
    // Tests for 'distinct' operation when hashed field is a prefix.
    //
    assert.commandWorked(coll.createIndex({b: "hashed", c: 1}));
    testAggAndDistinct("a", {a: {$gt: 50, $lt: 55}}, [51, 52, 53, 54]);
    testAggAndDistinct("b", {b: {$lt: 6, $gt: 4}}, [5]);
    testAggAndDistinct("c", undefined, cValues);
}

assert(coll.drop());

{
    //
    // Test distinct-like queries with multikeyness.
    // Note: not running same query with $group now that the field is multikey- agg & distinct
    // differ.
    //

    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.insert({a: [1, 2, 3]}));
    assert.commandWorked(coll.insert({a: [2, 3, 4]}));
    assert.commandWorked(coll.insert({a: [5, 6, 7]}));

    testCmd({distinct: coll.getName(), key: "a"}, [1, 2, 3, 4, 5, 6, 7], [{a: 1}, {$natural: -1}]);
    testCmd({distinct: coll.getName(), key: "a", query: {a: 3}}, [1, 2, 3, 4], [{a: 1}]);

    // Test distinct over a dotted multikey field, with a predicate.
    assert(coll.drop());
    assert.commandWorked(coll.createIndex({"a.b": 1}));
    assert.commandWorked(coll.insert({a: {b: [1, 2, 3]}}));
    assert.commandWorked(coll.insert({a: {b: [2, 3, 4]}}));

    testCmd({distinct: coll.getName(), key: "a.b", query: {"a.b": 3}},
            [1, 2, 3, 4],
            [{"a.b": 1}, {$natural: 1}]);

    // Test a distinct which can use a multikey index, where the field being distinct'ed is not
    // multikey.
    assert(coll.drop());
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    assert.commandWorked(coll.insert({a: 1, b: [2, 3]}));
    assert.commandWorked(coll.insert({a: 8, b: [3, 4]}));
    assert.commandWorked(coll.insert({a: 7, b: [4, 5]}));

    testCmd({distinct: coll.getName(), key: "a", query: {a: {$gte: 2}}},
            [7, 8],
            [{a: 1, b: 1}, {$natural: -1}]);

    // Test distinct over a trailing multikey field.
    testCmd({distinct: coll.getName(), key: "b", query: {a: {$gte: 2}}},
            [3, 4, 5],
            [{a: 1, b: 1}, {$natural: 1}]);

    // Test distinct over a trailing non-multikey field, where the leading field is multikey.
    assert(coll.drop());
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    assert.commandWorked(coll.insert({a: [2, 3], b: 1}));
    assert.commandWorked(coll.insert({a: [3, 4], b: 8}));
    assert.commandWorked(coll.insert({a: [3, 5], b: 7}));

    testCmd({distinct: coll.getName(), key: "b", query: {a: 3}},
            [1, 7, 8],
            [{a: 1, b: 1}, {$natural: 1}]);

    // Test distinct over a trailing non-multikey dotted path where the leading field is multikey.
    assert(coll.drop());
    assert.commandWorked(coll.createIndex({a: 1, "b.c": 1}));
    assert.commandWorked(coll.insert({a: [2, 3], b: {c: 1}}));
    assert.commandWorked(coll.insert({a: [3, 4], b: {c: 8}}));
    assert.commandWorked(coll.insert({a: [3, 5], b: {c: 7}}));

    testCmd({distinct: coll.getName(), key: "b.c", query: {a: 3}},
            [1, 7, 8],
            [{a: 1, "b.c": 1}, {$natural: 1}]);
}

assert(coll.drop());

{
    //
    // Tests for multikey indexes with a dotted path.
    //
    assert.commandWorked(coll.createIndex({"a.b.c": 1}));
    assert.commandWorked(coll.insertMany([
        {a: {b: {c: 1}}},
        {a: {b: {c: 2}}},
        {a: {b: {c: 3}}},
        {a: {b: {notRelevant: 3}}},
        {a: {notRelevant: 3}}
    ]));

    // TODO SERVER-14832: Returning 'null' here is inconsistent with the behavior when no index is
    // present. However, on sharded passthroughs, we sometimes omit it, since whichever shard has
    // the "null" document may pick a different plan due to multiplanning.
    // testAggAndDistinct("a.b.c", undefined, [1, 2, 3, null], [{"a.b.c": 1}]);

    testAggAndDistinct("a.b.c", {"a.b.c": {$gt: 0}}, [1, 2, 3], [{"a.b.c": 1}, {$natural: -1}]);

    assert.commandWorked(coll.insertMany([
        // Make the index multi-key.
        {a: {b: [{c: 4}, {c: 5}]}},
        {a: {b: [{c: 4}, {c: 6}]}},
        // Empty array is indexed as 'undefined'.
        {a: {b: {c: []}}}
    ]));

    // Not running same query with $group now that the field is multikey- agg & distinct differ.

    // TODO SERVER-14832: Returning 'null' and 'undefined' here is inconsistent with the behavior
    // when no index is present. However, on sharded passthroughs, we sometimes omit it, since
    // whichever shard has the "null" document may pick a different plan due to multiplanning.
    // testCmd({distinct: coll.getName(), key: "a.b.c"}, [1, 2, 3, 4, 5, 6, null, undefined],
    // [{"a.b.c": 1}]);

    testCmd({distinct: coll.getName(), key: "a.b.c", query: {"a.b.c": 4}},
            [4, 5, 6],
            [{"a.b.c": 1}, {$natural: 1}]);

    // Index where last component of path is multikey.
    assert.commandWorked(coll.createIndex({"a.b": 1}));

    // TODO SERVER-14832: Returning 'null' and 'undefined' here is inconsistent with the behavior
    // when no index is present. However, on sharded passthroughs, we sometimes omit it, since
    // whichever shard has the "null" document may pick a different plan due to multiplanning.
    // testCmd({distinct: coll.getName(), key: "a.b"}, [
    //     null,  // From the document with no 'b' field.
    //     {c: 1},
    //     {c: 2},
    //     {c: 3},
    //     {c: 4},
    //     {c: 5},
    //     {c: 6},
    //     {c: []},
    //     {notRelevant: 3}
    // ], [{"a.b": 1}]);

    testCmd({distinct: coll.getName(), key: "a.b", query: {"a.b": {$type: "array"}}},
            [
                {c: 4},
                {c: 5},
                {c: 6},
            ],
            [{"a.b": 1}, {$natural: 1}]);
    testCmd({distinct: coll.getName(), key: "a.b.0"}, [{c: 4}], []);

    assert.commandWorked(coll.createIndex({"a.b.0": 1}));
    assert.commandWorked(coll.insert({a: {b: {0: "hello world"}}}));

    // Will not attempt the equivalent query with aggregation, since $group by "a.b.0" will
    // only treat '0' as a field name (not array index).
    testCmd({distinct: coll.getName(), key: "a.b.0"}, [{c: 4}, "hello world"]);
    testCmd({distinct: coll.getName(), key: "a.b.0", query: {"a.b.0": {$type: "object"}}},
            [{c: 4}]);
}
