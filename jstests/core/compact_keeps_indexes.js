// SERVER-16676 Make sure compact doesn't leave the collection with bad indexes

(function() {
    'use strict';

    var coll = db.compact_keeps_indexes;

    coll.insert({_id:1, x:1});
    coll.ensureIndex({x:1});

    assert.eq(coll.getIndexes().length, 2);

    var res = coll.runCommand('compact');
    if (res.code != 115) { // CommandNotSupported
        assert.commandWorked(res);
    }

    assert.eq(coll.getIndexes().length, 2);
    assert.eq(coll.find({_id:1}).itcount(), 1);
    assert.eq(coll.find({x:1}).itcount(), 1);
}())
