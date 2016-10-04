// Yield and delete test case for query optimizer cursor.  SERVER-4401

t = db.jstests_distinct3;
t.drop();

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});

var bulk = t.initializeUnorderedBulkOp();
for (i = 0; i < 50; ++i) {
    for (j = 0; j < 2; ++j) {
        bulk.insert({a: i, c: i, d: j});
    }
}
for (i = 0; i < 100; ++i) {
    bulk.insert({b: i, c: i + 50});
}
assert.writeOK(bulk.execute());

// Attempt to remove the last match for the {a:1} index scan while distinct is yielding.
p = startParallelShell('for( i = 0; i < 100; ++i ) {                              ' +
                       '    var bulk = db.jstests_distinct3.initializeUnorderedBulkOp();' +
                       '    bulk.find( { a:49 } ).remove();                       ' +
                       '    for( j = 0; j < 20; ++j ) {                           ' +
                       '        bulk.insert( { a:49, c:49, d:j } );               ' +
                       '    }                                                     ' +
                       '    assert.writeOK(bulk.execute());                       ' +
                       '}                                                         ');

for (i = 0; i < 100; ++i) {
    count = t.distinct('c', {$or: [{a: {$gte: 0}, d: 0}, {b: {$gte: 0}}]}).length;
    assert.gt(count, 100);
}

p();
