/**
* Tests that using a UUID as an argument to commands will retrieve results from the correct
* collection.
*
* @tags: [requires_fastcount]
*/

(function() {
    'use strict';
    let mainCollName = 'main_coll';
    let subCollName = 'sub_coll';
    db.runCommand({drop: mainCollName});
    db.runCommand({drop: subCollName});
    assert.commandWorked(db.runCommand({create: mainCollName}));
    assert.commandWorked(db.runCommand({create: subCollName}));

    // Check if UUIDs are enabled / supported.
    let collectionInfos = db.getCollectionInfos({name: mainCollName});
    let uuid = collectionInfos[0].info.uuid;
    if (uuid == null) {
        return;
    }

    // No support for UUIDs on mongos.
    const isMaster = db.runCommand("ismaster");
    assert.commandWorked(isMaster);
    const isMongos = (isMaster.msg === "isdbgrid");
    if (isMongos) {
        return;
    }

    assert.commandWorked(db.runCommand({insert: mainCollName, documents: [{fooField: 'FOO'}]}));
    assert.commandWorked(
        db.runCommand({insert: subCollName, documents: [{fooField: 'BAR'}, {fooField: 'FOOBAR'}]}));

    // Ensure passing a UUID to find retrieves results from the correct collection.
    let cmd = {find: uuid};
    let res = db.runCommand(cmd);
    assert.commandWorked(res, 'could not run ' + tojson(cmd));
    let cursor = new DBCommandCursor(db, res);
    let errMsg = 'expected more data from command ' + tojson(cmd) + ', with result ' + tojson(res);
    assert(cursor.hasNext(), errMsg);
    let doc = cursor.next();
    assert.eq(doc.fooField, 'FOO');
    assert(!cursor.hasNext(), 'expected to have exhausted cursor for results ' + tojson(res));

    // Although we check for both string type and BinData type for the collection identifier
    // argument to a find command to accomodate for searching both by name and by UUID, if an
    // invalid type is passed, the parsing error message should say the expected type is string and
    // not BinData to avoid confusing the user.
    cmd = {find: 1.0};
    res = db.runCommand(cmd);
    assert.commandFailed(res, 'expected ' + tojson(cmd) + ' to fail.');
    assert(res.errmsg.includes('field must be of BSON type string'),
           'expected the error message of ' + tojson(res) + ' to include string type');

    // Ensure passing a missing UUID to commands taking UUIDs uasserts that the UUID is not found.
    const missingUUID = UUID();
    for (cmd of[{count: missingUUID},
                {find: missingUUID},
                {listIndexes: missingUUID},
                {parallelCollectionScan: missingUUID, numCursors: 1}]) {
        assert.commandFailedWithCode(
            db.runCommand(cmd), ErrorCodes.NamespaceNotFound, "command: " + tojson(cmd));
    }

    // Ensure passing a UUID to listIndexes retrieves results from the correct collection.
    cmd = {listIndexes: uuid};
    res = db.runCommand(cmd);
    assert.commandWorked(res, 'could not run ' + tojson(cmd));
    cursor = new DBCommandCursor(db, res);
    cursor.forEach(function(doc) {
        assert.eq(doc.ns, 'test.' + mainCollName);
    });

    // Ensure passing a UUID to count retrieves results from the correct collection.
    cmd = {count: uuid};
    res = db.runCommand(cmd);
    assert.commandWorked(res, 'could not run ' + tojson(cmd));
    assert.eq(res.n, 1, "expected to count a single document with command: " + tojson(cmd));

    // Ensure passing a UUID to parallelCollectionScan retrieves results from the correct
    // collection.
    cmd = {parallelCollectionScan: uuid, numCursors: 1};
    res = assert.commandWorked(db.runCommand(cmd), 'could not run ' + tojson(cmd));
    assert.eq(res.cursors[0].cursor.ns, 'test.' + mainCollName);
}());
