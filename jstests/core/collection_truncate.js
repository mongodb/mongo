// SERVER-15033 truncate on a regular collection

var t = db.getCollection('collection_truncate');
t.drop();

function truncate() {
    // Until SERVER-15274 is implemented, this is the only way to truncate a collection.
    assert.commandWorked(t.runCommand('emptycapped'));  // works on non-capped as well.
}

function assertEmpty() {
    var stats = t.stats();

    assert.eq(stats.count, 0);
    assert.eq(stats.size, 0);

    if ('numExtents' in stats) {
        assert.lte(stats.numExtents, 1);
    }

    assert.eq(t.count(), 0);
    assert.eq(t.find().itcount(), 0);

    var res = t.validate({full: true});
    assert.commandWorked(res);
    assert(res.valid, "failed validate(): " + tojson(res));
}

// Single record case.
t.insert({a: 1});
truncate();
assertEmpty();

// Multi-extent case.
var initialStorageSize = t.stats().storageSize;
while (t.stats().storageSize == initialStorageSize) {
    t.insert({a: 1});
}
truncate();
assertEmpty();

// Already empty case.
truncate();
assertEmpty();
