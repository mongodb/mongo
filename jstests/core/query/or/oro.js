// @tags: [
//   requires_collstats,
//   requires_getmore,
// ]

// Test $or query with several clauses on separate indexes.

let t = db.jstests_oro;
t.drop();

let orClauses = [];
for (let idxKey = 'a'; idxKey <= 'aaaaaaaaaa'; idxKey += 'a') {
    let idx = {};
    idx[idxKey] = 1;
    t.createIndex(idx);
    for (let i = 0; i < 200; ++i) {
        t.insert(idx);
    }
    orClauses.push(idx);
}

printjson(t.find({$or: orClauses}).explain());
let c = t.find({$or: orClauses}).batchSize(100);
let count = 0;

while (c.hasNext()) {
    for (let i = 0; i < 50 && c.hasNext(); ++i, c.next(), ++count)
        ;
    // Interleave with another operation.
    t.stats();
}

assert.eq(10 * 200, count);
