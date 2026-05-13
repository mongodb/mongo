/**
 * Verify runtimeConstants userRoles validation.
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

describe("$merge with runtimeConstants.userRoles", function() {
    let conn;
    let db;
    let internalDb;

    before(function() {
        conn = MongoRunner.runMongod();
        db = conn.getDB(jsTestName());

        // Establish an internal client connection. fromRouter is restricted to internal clients,
        // so tests that pass it must use this connection.
        const internalConn = new Mongo(conn.host);
        assert.commandWorked(
            internalConn.getDB("admin").runCommand({
                hello: 1,
                internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
            }),
        );
        internalDb = internalConn.getDB(jsTestName());
    });

    after(function() {
        MongoRunner.stopMongod(conn);
    });

    beforeEach(function() {
        db.dropDatabase();
        assert.commandWorked(db.src.insert({_id: 1, a: 1}));
    });

    const mergeCmd = (userRoles) => ({
        aggregate: "src",
        pipeline: [
            {$project: {_id: 1, a: 1}},
            {$merge: {into: "dst", on: "_id", whenMatched: "merge", whenNotMatched: "insert"}},
        ],
        cursor: {},
        fromRouter: true,
        runtimeConstants: {
            localNow: new Date(),
            clusterTime: Timestamp(1, 1),
            userRoles,
        },
        readConcern: {},
        writeConcern: {},
    });

    it("rejects fromRouter from an external client", function() {
        // fromRouter is an internal-only field; external clients should get an unknown-field error.
        assert.commandFailedWithCode(db.runCommand(mergeCmd([])), ErrorCodes.BadValue);
    });

    it("rejects non-object elements in userRoles", function() {
        // userRoles: [1] contains a non-object element; verify the server rejects it cleanly.
        const res = internalDb.runCommand(mergeCmd([1]));

        // IDL catches this during deserialization and reports the field path in the message.
        assert.commandFailedWithCode(res, ErrorCodes.TypeMismatch, "Expected TypeMismatch error");
        assert(res.errmsg.includes("userRoles"), "Error message should mention userRoles field");

        // Verify the server is still alive.
        assert.commandWorked(db.adminCommand({ping: 1}));
    });

    it("accepts a valid userRoles array of objects", function() {
        assert.commandWorked(internalDb.runCommand(
            mergeCmd([{_id: "test.readWrite", role: "readWrite", db: "test"}])));

        // Verify the document was merged into dst.
        assert.eq(1, db.dst.find({_id: 1}).count(), "Expected merged document in dst");
    });

    it("accepts an empty userRoles array", function() {
        assert.commandWorked(internalDb.runCommand(mergeCmd([])));
    });
});
