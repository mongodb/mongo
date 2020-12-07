/**
 * Tests operations on indexes with the maximum number of compound index fields, 32.
 *
 *  @tags: [
 *   assumes_unsharded_collection,
 *   multiversion_incompatible,
 *   requires_non_retryable_writes,
 * ]
 */
(function() {

const collName = jsTestName();
const coll = db[collName];
coll.drop();

// Create a spec with alternating ascending and descending fields to keep things interesting.
let spec = {};
for (let i = 0; i < 32; i++) {
    spec["f" + i] = (i % 2 == 0) ? 1 : -1;
}

const indexName = "big_index";
assert.commandWorked(coll.createIndex(spec, {name: indexName}));
assert.commandWorked(coll.insert({_id: 0}));
for (let i = 0; i < 32; i++) {
    assert.commandWorked(coll.update({_id: 0}, {
        $set: {['f' + i]: 1},
    }));
}

for (let i = 0; i < 32; i++) {
    assert.eq(1, coll.find({['f' + i]: 1}).hint(indexName).itcount());
}

// Create an index that has one too many fields.
let bigSpec = Object.extend({'f32': -1}, spec);
assert.commandFailedWithCode(coll.createIndex(bigSpec), 13103);

coll.drop();

// Create a unique version of the same index from before.
assert.commandWorked(coll.createIndex(spec, {unique: true, name: indexName}));

let doc = {};
let doc2 = {};
for (let i = 0; i < 32; i++) {
    doc['f' + i] = 1;
    doc2['f' + i] = 2;
}

assert.commandWorked(coll.insert(doc));
assert.commandWorked(coll.insert(doc2));

for (let i = 0; i < 32; i++) {
    let query = {['f' + i]: 1};
    assert.eq(2, coll.find().hint(indexName).itcount(), "failed on query: " + tojson(query));
}
})();
