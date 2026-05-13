/**
 * Verify runtimeConstants userRoles validation.
 */
(function () {
    "use strict";

    const conn = MongoRunner.runMongod();
    const db = conn.getDB(jsTestName());

    const internalConn = new Mongo(conn.host);
    assert.commandWorked(
        internalConn.getDB("admin").runCommand({
            hello: 1,
            internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
        }),
    );
    const internalDb = internalConn.getDB(jsTestName());

    const mergeCmd = (userRoles) => ({
        aggregate: "src",
        pipeline: [
            {$project: {_id: 1, a: 1}},
            {$merge: {into: "dst", on: "_id", whenMatched: "merge", whenNotMatched: "insert"}},
        ],
        cursor: {},
        fromMongos: true,
        runtimeConstants: {
            localNow: new Date(),
            clusterTime: Timestamp(1, 1),
            userRoles,
        },
        readConcern: {},
        writeConcern: {},
    });

    // fromMongos is an internal-only field; external clients should get an unknown-field error.
    assert.commandFailedWithCode(db.runCommand(mergeCmd([])), ErrorCodes.BadValue);

    // Reset and insert source document for subsequent tests.
    db.dropDatabase();
    assert.commandWorked(db.src.insert({_id: 1, a: 1}));

    // userRoles: [1] contains a non-object element; verify the server rejects it cleanly.
    const res = internalDb.runCommand(mergeCmd([1]));
    assert.commandFailedWithCode(res, ErrorCodes.TypeMismatch, "Expected TypeMismatch error");

    // Verify the server is still alive.
    assert.commandWorked(db.adminCommand({ping: 1}));

    // Reset and insert source document.
    db.dropDatabase();
    assert.commandWorked(db.src.insert({_id: 1, a: 1}));

    // Accepts a valid userRoles array of objects.
    assert.commandWorked(internalDb.runCommand(mergeCmd([{_id: "test.readWrite", role: "readWrite", db: "test"}])));

    // Verify the document was merged into dst.
    assert.eq(1, db.dst.find({_id: 1}).count(), "Expected merged document in dst");

    // Reset and insert source document.
    db.dropDatabase();
    assert.commandWorked(db.src.insert({_id: 1, a: 1}));

    // Accepts an empty userRoles array.
    assert.commandWorked(internalDb.runCommand(mergeCmd([])));

    MongoRunner.stopMongod(conn);
})();
