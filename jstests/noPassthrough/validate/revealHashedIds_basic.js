/**
 * Verifies the 'revealHashedIds' field for the validate command returns results as expected.
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());
const coll = db.getCollection(jsTestName());

assert.commandWorked(coll.insert({_id: 1, f: "random string"}));

jsTest.log.info(
    "Compute the hash prefix of the document's _id field through validate 'collHash' 'hashPrefixes' option",
);
let res = assert.commandWorked(coll.validate({collHash: true, hashPrefixes: []}));
const idHashPrefixes = Object.keys(res.partial);
assert.eq(idHashPrefixes.length, 1, res);
const idHashPrefix = idHashPrefixes[0];
jsTest.log.info(`The hash prefix of the document's _id field is: ${idHashPrefix}`);

jsTest.log.info("revealHashedIds with a hash prefix");
res = assert.commandWorked(coll.validate({collHash: true, revealHashedIds: [idHashPrefix]}));
assert(res.valid);
assert(res.all);
assert.eq(res.revealedIds[idHashPrefix].length, 1, res);
assert.eq(res.revealedIds[idHashPrefix][0], {_id: 1}, res);

jsTest.log.info("revealHashedIds with a non-matching hash prefix");
// XOR the hex value with 1 to get a different character to use as a non-matching prefix.
const differentHash = (parseInt(idHashPrefix[0], 16) ^ 1).toString(16).toUpperCase();
res = assert.commandWorked(coll.validate({collHash: true, revealHashedIds: [differentHash]}));
assert(res.valid);
assert(res.all);
assert.eq(res.revealedIds[differentHash].length, 0, res);

MongoRunner.stopMongod(conn);
