/**
 * Validates that we are able to use an index of object $elemMatch + $not.
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

Random.setRandomSeed(125555);

function randString() {
    return Array.from({length: Random.randInt(11)}, () =>
        String.fromCharCode("A".charCodeAt(0) + Random.randInt(26)),
    ).join("");
}

const atVals = ["ABBA", "BETA", "ALPHA", "THETA", "DINO"];
function randAtVal() {
    return atVals[Random.randInt(atVals.length)];
}

/**
 * Generates a randomized value for the 'th' field.
 */
function thArrVal() {
    const rand = Random.randInt(4);
    switch (rand) {
        case 0:
            return {at: randAtVal()};
        case 1:
        case 2:
            return {sa: ISODate(), at: randAtVal()};
        default:
            return {sa: null, at: randAtVal()};
    }
}

const c = db[jsTestName()];
c.drop();
assert.commandWorked(
    c.insertMany(
        Array.from({length: 121}, () => ({
            s: randString(),
            wa: Random.randInt(2) == 1,
            at: "MATCHES",
            th: Array.from({length: Random.randInt(10)}, thArrVal),
        })),
    ),
);
assert.commandWorked(c.createIndex({s: 1, wa: 1, at: 1, "th.at": 1, "th.sa": 1}));

function runTest(query, expectedIxBounds) {
    const explain = c.find(query).explain();
    const winningPlan = getWinningPlanFromExplain(explain);
    const ixscans = getPlanStages(winningPlan, "IXSCAN");

    assert.eq(
        ixscans.length,
        expectedIxBounds.length,
        `Expected ${expectedIxBounds.length} index scans, found ${ixscans.length}, explain: ${tojson(explain)}`,
    );
    assertArrayEq({expected: expectedIxBounds, actual: ixscans.map((is) => is.indexBounds)});

    // Ensure we get the right result with/without an index.
    assertArrayEq({expected: c.find(query).hint({$natural: 1}).toArray(), actual: c.find(query).toArray()});
}

// Able to use [null, null] bounds on th.sa.
runTest(
    {
        "$and": [
            {s: {$regex: "^.+$"}},
            {wa: {"$ne": true}},
            {at: "MATCHES"},
            {th: {"$elemMatch": {sa: {"$exists": false}}}},
        ],
    },
    [
        {
            "s": ['["", {})', "[/^.+$/, /^.+$/]"],
            "wa": ["[MinKey, true)", "(true, MaxKey]"],
            "at": ['["MATCHES", "MATCHES"]'],
            "th.at": ["[MinKey, MaxKey]"],
            "th.sa": ["[null, null]"],
        },
    ],
);

// Able to use [null, null] bounds on th.sa on one side of the OR.
runTest(
    {
        "$and": [
            {s: {$regex: "^.+$"}},
            {wa: {"$ne": true}},
            {
                "$or": [
                    {at: "ABBA"},
                    {
                        th: {"$elemMatch": {sa: {"$exists": false}, at: "ABBA"}},
                    },
                ],
            },
        ],
    },
    [
        {
            "s": ['["", {})', "[/^.+$/, /^.+$/]"],
            "wa": ["[MinKey, true)", "(true, MaxKey]"],
            "at": ['["ABBA", "ABBA"]'],
            "th.at": ["[MinKey, MaxKey]"],
            "th.sa": ["[MinKey, MaxKey]"],
        },
        {
            "s": ['["", {})', "[/^.+$/, /^.+$/]"],
            "wa": ["[MinKey, true)", "(true, MaxKey]"],
            "at": ["[MinKey, MaxKey]"],
            "th.at": ['["ABBA", "ABBA"]'],
            "th.sa": ["[null, null]"],
        },
    ],
);
