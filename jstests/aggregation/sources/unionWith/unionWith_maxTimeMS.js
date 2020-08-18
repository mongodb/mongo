/**
 * Tests that maxTimeMS is respected even in an inner $unionWith pipeline.
 * @tags: [
 *   sbe_incompatible,
 * ]
 */

(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
const collA = testDB.A;
collA.drop();
const collB = testDB.B;
collB.drop();

for (let i = 0; i < 10; i++) {
    assert.commandWorked(collA.insert({val: i}));
    assert.commandWorked(collB.insert({val: i * 2}));
}
function sleepAndIncrement(val) {
    sleep(2000);
    return val + 1;
}
assert.commandFailedWithCode(testDB.runCommand({
    aggregate: collA.getName(),
    pipeline: [{
        $unionWith: {
            coll: collB.getName(),
            pipeline: [{
                $project:
                    {newVal: {$function: {args: ["$val"], body: sleepAndIncrement, lang: "js"}}}
            }]
        }
    }],
    cursor: {},
    maxTimeMS: 3 * 1000
}),
                             ErrorCodes.MaxTimeMSExpired);
})();
