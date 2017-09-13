// SERVER-16497: Check that failIndexKeyTooLong setting works
(function() {
    "use strict";

    var t = db.index_bigkeys_nofail;
    t.drop();
    var res = db.getSiblingDB('admin').runCommand({setParameter: 1, failIndexKeyTooLong: true});
    var was = res.was;
    assert.commandWorked(res);

    var x = new Array(1025).join('x');
    assert.commandWorked(t.ensureIndex({name: 1}));
    assert.writeError(t.insert({name: x}));
    assert.commandWorked(t.dropIndex({name: 1}));
    assert.writeOK(t.insert({name: x}));
    assert.commandFailed(t.ensureIndex({name: 1}));

    t.drop();
    db.getSiblingDB('admin').runCommand({setParameter: 1, failIndexKeyTooLong: false});

    // inserts
    assert.writeOK(t.insert({_id: 1, name: x}));
    assert.commandWorked(t.ensureIndex({name: 1}));
    assert.writeOK(t.insert({_id: 2, name: x}));
    assert.writeOK(t.insert({_id: 3, name: x}));
    assert.eq(t.count(), 3);

    // updates (smaller and larger)
    assert.writeOK(t.update({_id: 1}, {$set: {name: 'short'}}));
    assert.writeOK(t.update({_id: 1}, {$set: {name: x}}));
    assert.writeOK(t.update({_id: 1}, {$set: {name: x + 'even longer'}}));

    // remove
    assert.writeOK(t.remove({_id: 1}));
    assert.eq(t.count(), 2);

    db.getSiblingDB('admin').runCommand({setParameter: 1, failIndexKeyTooLong: true});

    // can still delete even if key is oversized
    assert.writeOK(t.remove({_id: 2}));
    assert.eq(t.count(), 1);

    // can still update to shorter, but not longer name.
    assert.writeError(t.update({_id: 3}, {$set: {name: x + 'even longer'}}));
    assert.writeOK(t.update({_id: 3}, {$set: {name: 'short'}}));
    assert.writeError(t.update({_id: 3}, {$set: {name: x}}));

    db.getSiblingDB('admin').runCommand({setParameter: 1, failIndexKeyTooLong: was});

    // Explicitly drop the collection to avoid failures in post-test hooks that run dbHash and
    // validate commands.
    t.drop();
}());
