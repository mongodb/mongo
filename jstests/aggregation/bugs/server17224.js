// SERVER-17224 An aggregation result with exactly the right size could crash the server rather than
//              returning an error.
(function() {
    'use strict';

    var t = db.server17224;
    t.drop();

    // first 63MB
    for (var i = 0; i < 63; i++) {
        t.insert({a: new Array(1024 * 1024 + 1).join('a')});
    }

    // the remaining ~1MB with room for field names and other overhead
    t.insert({a: new Array(1024 * 1024 - 1105).join('a')});

    // do not use cursor form, since it has a different workaroud for this issue.
    assert.commandFailed(db.runCommand({
        aggregate: t.getName(),
        pipeline: [{$match: {}}, {$group: {_id: null, arr: {$push: {a: '$a'}}}}]
    }));

    // Make sure the server is still up.
    assert.commandWorked(db.runCommand('ping'));
}());
