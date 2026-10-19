/**
 * Verify $minN native SBE group accumulators evaluate 'n' against the group
 * key, not the incoming document.
 *
 * @tags: [
 *   requires_fcv_91,
 * ]
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());
const coll = db.coll;

assert.commandWorked(
    coll.insert([
        {_id: 1, g: "x", v: 0},
        {_id: 2, g: "x", v: 5},
        {_id: 3, g: "x", v: 1},
        {_id: 4, g: "x", v: 2},
        {_id: 5, g: "x", v: 3},
        {_id: 6, g: "x", v: 4},
    ]),
);

const pipeline = [
    {
        $group: {
            _id: "$g",
            m: {$minN: {input: "$v", n: {$cond: {if: {$eq: ["$v", 0]}, then: 2, else: 10}}}},
        },
    },
];
const expected = [{_id: "x", m: [0, 1, 2, 3, 4, 5]}];

function runWithEngine(frameworkControl, sbeAccumulators) {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            featureFlagSbeAccumulators: sbeAccumulators,
            internalQueryFrameworkControl: frameworkControl,
        }),
    );
    return coll.aggregate(pipeline).toArray();
}

assert.eq(runWithEngine("trySbeRestricted", true), expected);
assert.eq(runWithEngine("forceClassicEngine", true), expected);
assert.eq(runWithEngine("trySbeRestricted", false), expected);

MongoRunner.stopMongod(conn);
