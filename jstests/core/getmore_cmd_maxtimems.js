// Test attaching maxTimeMS to a getMore command.
(function() {
    'use strict';

    var cmdRes;
    var collName = 'getmore_cmd_maxtimems';
    var coll = db[collName];
    coll.drop();

    for (var i = 0; i < 10; i++) {
        assert.writeOK(coll.insert({a: i}));
    }

    // Can't attach maxTimeMS to a getMore command for a non-tailable cursor over a non-capped
    // collection.
    cmdRes = db.runCommand({find: collName, batchSize: 2});
    assert.commandWorked(cmdRes);
    cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName, maxTimeMS: 60000});
    assert.commandFailed(cmdRes);

    coll.drop();
    assert.commandWorked(db.createCollection(collName, {capped: true, size: 1024}));
    for (var i = 0; i < 10; i++) {
        assert.writeOK(coll.insert({a: i}));
    }

    // Can't attach maxTimeMS to a getMore command for a non-tailable cursor over a capped
    // collection.
    cmdRes = db.runCommand({find: collName, batchSize: 2});
    assert.commandWorked(cmdRes);
    cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName, maxTimeMS: 60000});
    assert.commandFailed(cmdRes);

    // Can't attach maxTimeMS to a getMore command for a non-awaitData tailable cursor.
    cmdRes = db.runCommand({find: collName, batchSize: 2, tailable: true});
    assert.commandWorked(cmdRes);
    cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName, maxTimeMS: 60000});
    assert.commandFailed(cmdRes);

    // Can attach maxTimeMS to a getMore command for an awaitData cursor.
    cmdRes = db.runCommand({find: collName, batchSize: 2, tailable: true, awaitData: true});
    assert.commandWorked(cmdRes);
    cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName, maxTimeMS: 60000});
    assert.commandWorked(cmdRes);
})();
