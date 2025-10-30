/**
 * Verifies that metadata hash is computed correctly when using {collHash: true} for validation.
 */

const conn1 = MongoRunner.runMongod();
const conn2 = MongoRunner.runMongod();
const dbName = jsTestName();
const db1 = conn1.getDB(dbName);
const db2 = conn2.getDB(dbName);
const collName = jsTestName();
const coll1 = db1.getCollection(collName);
const coll2 = db2.getCollection(collName);

const createEntry = {
    "op": "c",
    "ns": `${dbName}.$cmd`,
    "ui": UUID("aa2aa34e-91fd-4566-8fc4-60d19f60c802"),
    "o": {
        "create": collName,
        "idIndex": {
            "v": 2,
            "key": {
                "_id": 1,
            },
            "name": "_id_",
        },
    },
    "o2": {
        "catalogId": NumberLong(22),
        "ident": "9f0a7f57-e55d-4831-bd0f-1dd7a2a243e2",
        "idIndexIdent": "0140dbda-299a-4629-867c-bf087762e617",
    },
};

// Use applyOps to make sure catalog entries are the same on the nodes.
assert.commandWorked(db1.adminCommand({applyOps: [createEntry]}));
assert.commandWorked(db2.adminCommand({applyOps: [createEntry]}));

jsTest.log.info("Testing empty collection");
const res1 = assert.commandWorked(coll1.validate({collHash: true}));
const res2 = assert.commandWorked(coll2.validate({collHash: true}));
assert(res1.metadata, res1);
assert.eq(res1.metadata, res2.metadata, `Collection1 result: ${tojson(res1)}\nCollection2 result: ${tojson(res2)}`);

jsTest.log.info("Testing nodes with the same collection contents");
assert.commandWorked(coll1.insert({_id: "id1", a: 1}));
assert.commandWorked(coll2.insert({_id: "id1", a: 1}));
const res1_same_doc = assert.commandWorked(coll1.validate({collHash: true}));
const res2_same_doc = assert.commandWorked(coll2.validate({collHash: true}));
assert.eq(
    res1_same_doc.metadata,
    res1.metadata,
    `Collection1 before insert result: ${tojson(res1)}\nCollection1 after insert result: ${tojson(res1_same_doc)}`,
);
assert.eq(
    res2_same_doc.metadata,
    res2.metadata,
    `Collection2 before insert result: ${tojson(res2)}\nCollection2 after insert result: ${tojson(res2_same_doc)}`,
);

jsTest.log.info("Testing nodes with different collection contents");
assert.commandWorked(coll2.updateOne({_id: "id1"}, {$set: {b: 5}}));
let res1_diff_doc = assert.commandWorked(coll1.validate({collHash: true}));
let res2_diff_doc = assert.commandWorked(coll2.validate({collHash: true}));
assert.eq(
    res1_diff_doc.metadata,
    res1.metadata,
    `Collection1 before insert result: ${tojson(res1)}\nCollection1 after insert result: ${tojson(res1_diff_doc)}`,
);
assert.eq(
    res2_diff_doc.metadata,
    res2.metadata,
    `Collection2 before insert result: ${tojson(res2)}\nCollection2 after insert result: ${tojson(res2_diff_doc)}`,
);

jsTest.log.info("Testing with 'revealHashedIds'");
res1_diff_doc = assert.commandWorked(coll1.validate({collHash: true, hashPrefixes: []}));
res2_diff_doc = assert.commandWorked(coll2.validate({collHash: true, hashPrefixes: []}));
const partial1Keys = Object.keys(res1_diff_doc.partial);
const partial2Keys = Object.keys(res2_diff_doc.partial);
assert.eq(partial1Keys.length, 1, res1_diff_doc);
assert.eq(partial2Keys.length, 1, res2_diff_doc);
const res1_reveal = assert.commandWorked(
    coll1.validate({collHash: true, revealHashedIds: [res1_diff_doc.partial[partial1Keys[0]].hash]}),
);
const res2_reveal = assert.commandWorked(
    coll2.validate({collHash: true, revealHashedIds: [res2_diff_doc.partial[partial2Keys[0]].hash]}),
);
assert.eq(
    res1_reveal.metadata,
    res1.metadata,
    `Collection1 before insert result: ${tojson(res1)}\nCollection1 after insert result: ${tojson(res1_reveal)}`,
);
assert.eq(
    res2_reveal.metadata,
    res2.metadata,
    `Collection2 before insert result: ${tojson(res2)}\nCollection2 after insert result: ${tojson(res2_reveal)}`,
);

jsTest.log.info("Testing when index catalog entries are in different orders");
assert.commandWorked(coll1.createIndexes([{first: 1}, {second: 1}]));
assert.commandWorked(coll2.createIndexes([{second: 1}, {first: 1}]));
const res1_order = assert.commandWorked(coll1.validate({collHash: true}));
const res2_order = assert.commandWorked(coll2.validate({collHash: true}));
assert.eq(
    res1_order.metadata,
    res2_order.metadata,
    `Collection1 result: ${tojson(res1_order)}\nCollection2 result: ${tojson(res2_order)}`,
);

jsTest.log.info("Testing when index catalog entries have different multikey fields");
assert.commandWorked(coll1.insert({_id: "multikey", first: [1, 2, 3]}));
assert.commandWorked(coll1.deleteOne({_id: "multikey"}));
const res1_multikey = assert.commandWorked(coll1.validate({collHash: true}));
const res2_multikey = assert.commandWorked(coll2.validate({collHash: true}));
assert.eq(
    res1_multikey.metadata,
    res2_multikey.metadata,
    `Collection1 result: ${tojson(res1_multikey)}\nCollection2 result: ${tojson(res2_multikey)}`,
);

jsTest.log.info("Testing when catalog entries diverge");
assert.commandWorked(coll1.createIndex({b: 1}));
const res1_div = assert.commandWorked(coll1.validate({collHash: true}));
const res2_div = assert.commandWorked(coll2.validate({collHash: true}));
assert.neq(
    res1_div.metadata,
    res2_div.metadata,
    `Collection1 result: ${tojson(res1_div)}\nCollection2 result: ${tojson(res2_div)}`,
);

MongoRunner.stopMongod(conn1);
MongoRunner.stopMongod(conn2);
