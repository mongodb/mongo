/**
 * Tests the bsonGetImmutable() shell function.
 */
"use strict";

const coll = db.immutable_bson;
coll.drop();

assert.commandWorked(coll.insert({_id: 0}));

// Gets a BSON object from the server for testing purposes. The resulting BSON contains the fields
// and values of the 'object' argument.
function getBsonFromServer(object) {
    const result = coll.aggregate([{$count: "count"}, {$project: {count: 0}}, {$addFields: object}]).toArray();
    assert.eq(result.length, 1);
    return result[0];
}

const testDoc = {
    a: 1,
    b: "string",
    _id: {c: 2, d: 3},
    e: [4, 5, 6],
    f: {g: 7, h: 8},
};

// Get two immutable copies of the same BSON from the server.
const immutableBson1 = bsonGetImmutable(getBsonFromServer(testDoc));
const immutableBson2 = bsonGetImmutable(getBsonFromServer(testDoc));

// Even just _reading_ the 'f' sub-document of a non-immutable BSON object would mark it as
// "altered" and cause it to be reserialized.
//
// This assertion is just here to make sure that there is some behavior that depends on the read
// operation and that cannot be reasonably optimized away.
assert.eq(immutableBson1.f.g, 7, immutableBson1);

// Reserializing 'immutableBson1' would move its _id field to the front of the document, causing
// this comparison to fail. The immutability should prevent that from happening.
assert(bsonBinaryEqual(immutableBson1, immutableBson2));

// Similarly, reading from the 'e' array should not reserialize an immutable BSON object.
assert.eq(immutableBson1.e[1], 5, immutableBson1);
assert(bsonBinaryEqual(immutableBson1, immutableBson2));

// Immutable means IMMUTABLE! NO MUTATIONS!
assert.throws(() => {
    immutableBson1.f.g = 9;
});
assert.eq(immutableBson1.f.g, 7, immutableBson1);

assert.throws(() => {
    immutableBson1.e[1] = 9;
});
assert.eq(immutableBson1.e[1], 5, immutableBson1);

assert(bsonBinaryEqual(immutableBson1, immutableBson2));
