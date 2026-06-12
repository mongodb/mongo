/**
 * Tests that the join optimizer does not run when a non-simple collation is specified on the
 * aggregation.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */
import {joinOptUsed} from "jstests/libs/query/join_utils.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagPathArrayness: true,
        internalEnableJoinOptimization: true,
        internalEnablePathArrayness: true,
    },
});
const testDB = conn.getDB("test");

const local = testDB.local;
const foreign = testDB.foreign;
local.drop();
foreign.drop();

// Mixed-case strings so case-insensitive collation produces different results than simple collation.
assert.commandWorked(
    local.insertMany([
        {_id: 1, a: "foo", b: 1},
        {_id: 2, a: "BAR", b: 2},
        {_id: 3, a: "baz", b: 3},
    ]),
);
assert.commandWorked(
    foreign.insertMany([
        {_id: 10, a: "FOO", c: 1},
        {_id: 11, a: "bar", c: 2},
        {_id: 12, a: "BAZ", c: 3},
    ]),
);

// Indexes for multikeyness info for path arrayness.
assert.commandWorked(local.createIndex({dummy: 1, a: 1, b: 1}));
assert.commandWorked(foreign.createIndex({dummy: 1, a: 1, c: 1}));

const pipeline = [
    {$lookup: {from: foreign.getName(), localField: "a", foreignField: "a", as: "joined"}},
    {$unwind: "$joined"},
    {$project: {_id: 1, matched: "$joined._id"}},
];

const caseInsensitive = {locale: "en_US", strength: 2};

// With simple collation, there are no matches and the join optimizer applies.
{
    const explain = local.explain().aggregate(pipeline);
    assert(joinOptUsed(explain), "Expected join optimizer to be used: " + tojson(explain));
    assert.sameMembers([], local.aggregate(pipeline).toArray());
}

// With a non-simple (case-insensitive) collation, the join optimizer should fall back but the
// query should still return correct results.
{
    const explain = local.explain().aggregate(pipeline, {collation: caseInsensitive});
    assert(
        !joinOptUsed(explain),
        "Expected join optimizer to not be used with non-simple collation: " + tojson(explain),
    );

    const withOpt = local.aggregate(pipeline, {collation: caseInsensitive}).toArray();

    // Validate against running the same pipeline with the optimizer disabled.
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}),
    );
    const withoutOpt = local.aggregate(pipeline, {collation: caseInsensitive}).toArray();
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}),
    );

    const expected = [
        {_id: 1, matched: 10},
        {_id: 2, matched: 11},
        {_id: 3, matched: 12},
    ];
    assert.sameMembers(expected, withOpt);
    assert.sameMembers(expected, withoutOpt);
}

MongoRunner.stopMongod(conn);
