// Test $in regular expressions with overlapping index bounds.  SERVER-4677
// @tags: [
//   requires_getmore
// ]

let t = db.jstests_inb;
t.drop();

function checkResults(query) {
    assert.eq(4, t.count(query));
    assert.eq(4, t.find(query).itcount());
}

t.createIndex({x: 1});
t.save({x: "aa"});
t.save({x: "ab"});
t.save({x: "ac"});
t.save({x: "ad"});

checkResults({x: {$in: [/^a/, /^ab/]}});
checkResults({x: {$in: [/^ab/, /^a/]}});
