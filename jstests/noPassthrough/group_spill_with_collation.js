/**
 * Tests $group execution with increased spilling and a non-simple collation.
 */

import {checkSBEEnabled} from "jstests/libs/sbe_util.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB('test');
if (!checkSBEEnabled(db)) {
    jsTestLog("Skipping test because SBE is not enabled");
    MongoRunner.stopMongod(conn);
    quit();
}

const coll = db.group_pushdown_with_collation;
coll.drop();
for (let i = 0; i < 1000; i++) {
    if (i % 3 === 0) {
        assert.commandWorked(coll.insert({x: 'a'}));
    } else {
        assert.commandWorked(coll.insert({x: 'A'}));
    }
}

assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQuerySlotBasedExecutionHashAggForceIncreasedSpilling: true}));
const caseInsensitive = {
    collation: {locale: "en_US", strength: 2}
};
const results =
    coll.aggregate([{$group: {_id: null, result: {$addToSet: "$x"}}}], caseInsensitive).toArray();
assert.eq(1, results.length, results);
assert.eq({_id: null, result: ["a"]}, results[0]);

MongoRunner.stopMongod(conn);
