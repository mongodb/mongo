// SERVER-27991 Allow inputting UUID to find and listIndexes commands
(function() {
    'use strict';
    let mainCollName = 'main_coll';
    let subCollName = 'sub_coll';
    db.runCommand({drop: mainCollName});
    db.runCommand({drop: subCollName});
    assert.commandWorked(db.runCommand({create: mainCollName}));
    assert.commandWorked(db.runCommand({create: subCollName}));
    let collectionInfos = db.getCollectionInfos({name: mainCollName});
    let uuid = collectionInfos[0].info.uuid;
    if (uuid == null) {
        return;
    }
    assert.commandWorked(db.runCommand({insert: mainCollName, documents: [{fooField: 'FOO'}]}));
    assert.commandWorked(db.runCommand({insert: subCollName, documents: [{fooField: 'BAR'}]}));

    // Ensure passing a UUID to find retrieves results from the correct collection.
    let cmd = {find: uuid};
    let res = db.runCommand(cmd);
    assert.commandWorked(res, 'could not run ' + tojson(cmd));
    let cursor = new DBCommandCursor(db.getMongo(), res);
    let errMsg = 'expected more data from command ' + tojson(cmd) + ', with result ' + tojson(res);
    assert(cursor.hasNext(), errMsg);
    let doc = cursor.next();
    assert.eq(doc.fooField, 'FOO');
    assert(!cursor.hasNext(), 'expected to have exhausted cursor for results ' + tojson(res));

    // Ensure passing a UUID to listIndexes retrieves results from the correct collection.
    cmd = {listIndexes: uuid};
    res = db.runCommand(cmd);
    assert.commandWorked(res, 'could not run ' + tojson(cmd));
    cursor = new DBCommandCursor(db.getMongo(), res);
    cursor.forEach(function(doc) {
        assert.eq(doc.ns, 'test.' + mainCollName);
    });
}());
