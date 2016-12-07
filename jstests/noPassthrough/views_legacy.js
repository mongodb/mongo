/**
 * Tests that views properly reject queries in legacy read mode, and reject writes performed in
 * legacy write mode. Also confirms that legacy killCursors execution is successful.
 */
(function() {
    "use strict";

    let conn = MongoRunner.runMongod({});

    let viewsDB = conn.getDB("views_legacy");
    assert.commandWorked(viewsDB.dropDatabase());
    assert.commandWorked(viewsDB.createView("view", "collection", []));
    let coll = viewsDB.getCollection("collection");

    for (let i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }

    conn.forceReadMode("legacy");
    conn.forceWriteMode("legacy");

    //
    // Legacy getMore is explicitly prohibited on views; you must use the getMore command.
    //
    let cmdRes =
        viewsDB.runCommand({find: "view", filter: {a: {$gt: 0}}, sort: {a: 1}, batchSize: 0});
    assert.commandWorked(cmdRes);
    let cursor = new DBCommandCursor(viewsDB.getMongo(), cmdRes, 2);

    let err = assert.throws(function() {
        cursor.itcount();
    }, [], "Legacy getMore expected to fail on a view cursor");
    assert.eq(ErrorCodes.CommandNotSupportedOnView, err.code, tojson(err));

    //
    // Legacy killcursors is expected to work on views.
    //
    cmdRes = viewsDB.runCommand({find: "view", filter: {a: {$gt: 0}}, sort: {a: 1}, batchSize: 0});
    assert.commandWorked(cmdRes);
    cursor = new DBCommandCursor(viewsDB.getMongo(), cmdRes, 2);

    // When DBCommandCursor is constructed under legacy readMode, cursor.close() will execute a
    // legacy killcursors operation.
    cursor.close();
    assert.gleSuccess(viewsDB, "legacy killcursors expected to work on view cursor");

    //
    // A view should reject all write CRUD operations performed in legacy write mode.
    //
    viewsDB.view.insert({x: 1});
    assert.gleErrorCode(viewsDB, ErrorCodes.CommandNotSupportedOnView);

    viewsDB.view.remove({x: 1});
    assert.gleErrorCode(viewsDB, ErrorCodes.CommandNotSupportedOnView);

    viewsDB.view.update({x: 1}, {x: 2});
    assert.gleErrorCode(viewsDB, ErrorCodes.CommandNotSupportedOnView);

    //
    // Legacy find is explicitly prohibited on views; you must use the find command.
    //
    let res = assert.throws(function() {
        viewsDB.view.find({x: 1}).toArray();
    });
    assert.eq(res.code, ErrorCodes.CommandNotSupportedOnView, tojson(res));

    MongoRunner.stopMongod(conn);
}());
