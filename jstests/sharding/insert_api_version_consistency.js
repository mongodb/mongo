// Test that we support all the same BSON types throughout API Version 1.
// @tags: [
//       requires_fcv_49,
// ]

const OID = new ObjectId();
const DATE = new Date();

const DOC = {
    x01: .01,
    x02: "string",
    x03: {a: 1},
    x04: ["array"],
    x05: {
        x00: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
        x01: BinData(1, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
        x02: BinData(2, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
        x03: BinData(3, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
        x04: BinData(4, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
        x05: BinData(5, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
        x06: BinData(6, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
    },
    x06: undefined,
    x07: OID,
    x08: false,
    x09: DATE,
    x0A: null,
    x0B: /^abc/,
    x0C: new DBPointer("test", OID),
    x0D: Code("function() { return true; }"),
    x0E: "Symbol does not exist in javascript",
    x0F: Code("function() { return true; }", {scope: true}),
    x10: NumberInt(1),
    x11: Timestamp(0, 1234),
    x12: NumberLong(1),
    x13: NumberDecimal(1.0),
    xFF: MinKey(),
    x7F: MaxKey(),
};

(function() {
"use strict";

let test = function(db) {
    // We have to create a collection until _configsvrCreateDatabase is in API Version 1.
    assert.commandWorked(db.createCollection("collection"));
    assert.commandWorked(
        db.runCommand({insert: "collection", documents: [DOC], apiVersion: "1", apiStrict: true}));
    const val = db.collection.findOne();

    assert.eq(val.x01, .01);
    assert.eq(val.x02, "string");
    assert.eq(val.x03, {a: 1});
    assert.eq(val.x04, ["array"]);

    assert.eq(val.x05.x00, BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    assert.eq(val.x05.x01, BinData(1, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    assert.eq(val.x05.x02, BinData(2, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    assert.eq(val.x05.x03, BinData(3, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    assert.eq(val.x05.x04, BinData(4, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    assert.eq(val.x05.x05, BinData(5, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    assert.eq(val.x05.x06, BinData(6, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA"));

    assert.eq(val.x06, undefined);
    assert.eq(val.x07, OID);
    assert.eq(val.x08, false);
    assert.eq(val.x09, DATE);
    assert.eq(val.x0A, null);
    assert.eq(val.x0B, /^abc/);
    assert.eq(val.x0C, new DBPointer("test", OID));
    assert.eq(val.x0D, Code("function() { return true; }"));
    assert.eq(val.x0E, "Symbol does not exist in javascript");
    assert.eq(val.x0F, Code("function() { return true; }", {scope: true}));
    assert.eq(val.x10, NumberInt(1));
    assert.eq(val.x11, Timestamp(0, 1234));
    assert.eq(val.x12, NumberLong(1));
    assert.eq(val.x13, NumberDecimal(1.0));
    assert.eq(val.xFF, MinKey());
    assert.eq(val.x7F, MaxKey());
};

// Testing against a sharded cluster.
{
    const st = new ShardingTest({shards: 1});
    const db = st.s.getDB("test");
    test(db);
    st.stop();
}

// Testing against a replica set.
{
    const rst = new ReplSetTest({nodes: 3});
    rst.startSet();
    rst.initiate();
    const db = rst.getPrimary().getDB("test");
    test(db);
    rst.stopSet();
}
})();