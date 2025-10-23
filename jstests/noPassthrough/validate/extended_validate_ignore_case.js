/**
 * Verifies that the inputs to hashPrefixes and revealHashedIds ignore case.
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());
const coll = db.getCollection(jsTestName());

for (let i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({_id: i}));
}

// Assert that we get the same output whether the hashPrefixes passed in are upper case
// or lower case.
let resUpper = Object.entries(
    assert.commandWorked(coll.validate({collHash: true, hashPrefixes: "ABCDEF".split("")})).partial,
);
let resLower = Object.entries(
    assert.commandWorked(coll.validate({collHash: true, hashPrefixes: "abcdef".split("")})).partial,
);
assert.gt(resUpper.length, 0, resUpper);
assert.sameMembers(resLower, resUpper, `Got different results. Lower: ${tojson(resLower)}, Upper: ${tojson(resUpper)}`);

// Assert that we get the same output when revealing hashed IDs regardless of case.
resUpper = Object.entries(
    assert.commandWorked(coll.validate({collHash: true, revealHashedIds: "ABCDEF".split("")})).revealedIds,
);
resLower = Object.entries(
    assert.commandWorked(coll.validate({collHash: true, revealHashedIds: "abcdef".split("")})).revealedIds,
);
assert.sameMembers(resLower, resUpper, `Got different results. Lower: ${tojson(resLower)}, Upper: ${tojson(resUpper)}`);

MongoRunner.stopMongod(conn);
