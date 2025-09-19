/**
 * Start a single standalone and ensures that partial hashes are sane.
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

// This test uses two ranges of _ids. Each _id in each range SHA256 hashes to a value that starts
// with the same hex character.
// For example, {_id: 17} hashes to 'EDF70214D121BD8D6093C4E07F821F6B0D978C44E51D95D30D514EB27D3EE1E0'
// which is why it appears in the 'E' bucket below.
// Further, some _id values in the 'B' bucket hash to 'BB' as well.
// 'E'  -> [17, 18, 19]
// 'B'  -> [22, 26, 35, 47, 56, 65, 99]
// 'BB' -> [22, 35]
//
// In this test, we ensure that the counts of hash values returned for each bucket are valid. Additionally,
// using vanilla {collHash: true}, we ensure that the XORed document hashes in each bucket are valid as well.
//
// There are four collections we use:
// db.full -> all documents
// db.e    -> only documents whose _id hash starts with 'E'
// db.b    -> only documents whose _id hash starts with 'B'
// db.bb   -> only documents whose _id hash starts with 'BB'
//
// We create the other collections so that we can cross-check via {collHash: true}.

let eIds = [17, 18, 19];
let bIds = [22, 26, 35, 47, 56, 65, 99];
let bbIds = [22, 35];

// Populate db.full
for (const id of eIds) {
    assert.commandWorked(db.full.insert({_id: id}));
}
for (const id of bIds) {
    assert.commandWorked(db.full.insert({_id: id}));
}
// Populate db.e
for (const id of eIds) {
    assert.commandWorked(db.e.insert({_id: id}));
}
// Populate db.b
for (const id of bIds) {
    assert.commandWorked(db.b.insert({_id: id}));
}
// Populate db.bb
for (const id of bbIds) {
    assert.commandWorked(db.bb.insert({_id: id}));
}

const eHash = assert.commandWorked(db.e.validate({collHash: true}))["all"];
const bHash = assert.commandWorked(db.b.validate({collHash: true}))["all"];
const bbHash = assert.commandWorked(db.bb.validate({collHash: true}))["all"];
jsTest.log.info("'E' hash value: " + eHash);
jsTest.log.info("'B' hash value: " + bHash);
jsTest.log.info("'BB' hash value: " + bbHash);

// First drill down: pass in an empty hashPrefixes.
let res = assert.commandWorked(db.full.validate({collHash: true, hashPrefixes: []}));
jsTest.log.info(`Received result with hashPrefixes: []. Result: ${tojson(res)}`);
let partial = res.partial;
assert(partial["E"]);
assert.eq(partial["E"]["count"], eIds.length);
assert.eq(partial["E"]["hash"], eHash);
assert(partial["B"]);
assert.eq(partial["B"]["count"], bIds.length);
assert.eq(partial["B"]["hash"], bHash);

// Second drill down: drill down on hashPrefixes: ['B']
res = assert.commandWorked(db.full.validate({collHash: true, hashPrefixes: ["B"]}));
jsTest.log.info(`Received result with hashPrefixes: ['B']. Result: ${tojson(res)}`);
partial = res.partial;
assert(partial["BB"]);
assert.eq(partial["BB"]["count"], bbIds.length);
assert.eq(partial["BB"]["hash"], bbHash);

MongoRunner.stopMongod(conn);
