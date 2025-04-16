/*
 * Tests various distinct-like query edge cases on passthrough suites- this does not check explain,
 * only validates that queries produce correct outputs under a variety of conditions. This test only
 * works on sharded passthroughs when shard-filtering distinct scan is enabled (otherwise orphans
 * may appear).
 *
 * @tags: [
 *  featureFlagShardFilteringDistinctScan,
 *  requires_fcv_82,
 *  simulate_atlas_proxy_incompatible,
 *  simulate_mongoq_incompatible,
 *  not_allowed_with_signed_security_token
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.drop();

// TODO SERVER-100039: replace this with passthrough suite. These hashes match the ones for
// featureFlagShardFilteringDistinctScan being disabled locally, but this test can't run in sharded
// passthroughs if the flag is disabled.
const expectedHashes = [
    "35AA7113BB2B887F4EFB4E36FF0FA462164C1BC9560112B9513092FAA22C779E",
    "8A10B8F8FE9CC02D5C21987D80F7A9613AA51912C8AC441B6AB2DB26D9B92009",
    "D67530DE316B15276B477EDE50E2ABE8979840174668B60EB4BE7B79CE1E0E5B",
    "6657D304F745788ADD65D8237F6D001AABC3B785F9CB04F6C233B8605D3B4D22",
    "B7CB43C07CEAB254F598C17B3FB14F85E39C314B4387EB96CE858A87CE99236F",
    "CD07D6934515ADB30A56C145AFF21E6740E92230DF43F4525DEC116C6D45E07D",
    "E7D1E5F9B0D8E15CF8EB0596C1AB51EF4DB8C87C171937007EAED174B492088C",
    "4438BB0B828B0C9593DD4E66DF00182FE2E5742D597C6EAE14B1BF361A1B773D",
    "C0AB73D5E358420A9D56B48EB823717665C95EEB6D3F3ADFC0F01275DDAA6BF4",
    "720A9764AAB0B214E60023679B864E8CE1C667C15483E41151E745EA0A52F038",
    "D135DF439C91979FE41D8C3D1FFC1E6AF9118E0A7DCF6D856822635B193F049D",
    "CCD0624F6FAB7002E18CCC7091697F14605D29618035DAED5E8352B101537766",
    "D67530DE316B15276B477EDE50E2ABE8979840174668B60EB4BE7B79CE1E0E5B",
    "6657D304F745788ADD65D8237F6D001AABC3B785F9CB04F6C233B8605D3B4D22",
    "349CFA789A63E477644D4F0CF6E2F72627B33D5FA76E6ACA95C1265932B8E1D2",
    "2A14F5FC771C9DA307BDD8DEB0739A6401F9E0CA14339C017BCD639D8F4DD830",
    "E7D1E5F9B0D8E15CF8EB0596C1AB51EF4DB8C87C171937007EAED174B492088C",
    "4438BB0B828B0C9593DD4E66DF00182FE2E5742D597C6EAE14B1BF361A1B773D",
    "B2FEF485A441917DF2FAF22FDE385CC06E3F52FA12C07D2AB4A8B1F5876F993B",
    "CDDD9755CE907E4CD61EEB601432A85E32C5EF45F0D72D11DDE362003B4D005E",
    "C07636437D3F11C810E8AEC6A32EBDE133EA74DF7E78BF983B2AAFC744312851",
    "03FD15094DA3A6AE2CC78EA62E0D63C0767F4170C8535C25591C77F354978799",
    "334B70D3D65E8F4B37200406689C0E29B3F7C99B9788CAA2391922047FE93B03",
    "94C743099AC12D645286FC2F47B2F147ECBA8590F3E0A1DD785B47E747837BA0",
    "9928D54A393E5BD4061C6E9E797FEC7BDB67B594B66B6BBB35F812F3F8AB4B5E",
    "88F82EBDAA0CC72292E028428E6BFBB7A443F96EE6DE58984CA77799D11648A8",
    "949A1D1FD937ECDA4B26736D95101CEB84E80C44006E166C6F82115BA5322324",
    "9BF3CCC850FBFB966E7ED9FC28E0C7D328BAE9624C8D3D2028040640549F734A",
    "6B176CB8BD92EF3FD08C81CCEF150A22BD5186BD432B0F5F9FE3ABC10B7EBA0C",
    "27963738A360C40177C24B43DD6240C8AFAF96FAF76F88BFB528BE052D540716",
    "27963738A360C40177C24B43DD6240C8AFAF96FAF76F88BFB528BE052D540716",
    "1F9156066E7D4AE2DE2B4F1E696DB6121ADBD2818E58CC2CB3372223B8D184BA"
];

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

let i = 0;
function testCmd(cmd, expected, hints = []) {
    ensureResults(cmd, expected);
    assert.eq(commandWorked({explain: cmd}).queryShapeHash, expectedHashes[i]);
    i++;
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
