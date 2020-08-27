// Test $in regular expressions with overlapping index bounds.  SERVER-4677
//
// This test is currently disabled in SBE passthroughs due to bugs that cause problems on big-endian
// platforms. Once these bugs are fixed, this test should be re-enabled for SBE.
// @tags: [
//   sbe_incompatible,
// ]

t = db.jstests_inb;
t.drop();

function checkResults(query) {
    assert.eq(4, t.count(query));
    assert.eq(4, t.find(query).itcount());
}

t.ensureIndex({x: 1});
t.save({x: 'aa'});
t.save({x: 'ab'});
t.save({x: 'ac'});
t.save({x: 'ad'});

checkResults({x: {$in: [/^a/, /^ab/]}});
checkResults({x: {$in: [/^ab/, /^a/]}});
