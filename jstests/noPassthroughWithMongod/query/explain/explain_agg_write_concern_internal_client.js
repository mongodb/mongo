/**
 * Tests that explain + writeConcern is rejected for external clients but allowed for internal
 * clients.
 *
 * Shards expect all operations from internal clients to have an explicit writeConcern specified,
 * so the aggregate command parsing must allow it.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();

describe("explain + writeConcern on external client", function () {
    before(function () {
        const coll = db[collName];
        coll.drop();
        assert.commandWorked(coll.insert({_id: 1}));
    });

    it("rejects explain + writeConcern on a read-only pipeline (boolean explain)", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: collName,
                pipeline: [],
                explain: true,
                writeConcern: {w: 1},
            }),
            ErrorCodes.FailedToParse,
        );
    });

    it("rejects explain + writeConcern on a read-only pipeline (explain command)", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                explain: {aggregate: collName, pipeline: [], cursor: {}, writeConcern: {w: 1}},
                verbosity: "queryPlanner",
            }),
            ErrorCodes.FailedToParse,
        );
    });
});

describe("explain + writeConcern on internal client", function () {
    let internalDB;

    before(function () {
        const coll = db[collName];
        coll.drop();
        assert.commandWorked(coll.insert({_id: 1}));

        const internalConn = new Mongo(db.getMongo().host);
        assert.commandWorked(
            internalConn.getDB("admin").runCommand({
                hello: 1,
                internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
            }),
        );
        internalDB = internalConn.getDB(db.getName());
    });

    it("allows explain + writeConcern on a read-only pipeline (boolean explain)", function () {
        assert.commandWorked(
            internalDB.runCommand({
                aggregate: collName,
                pipeline: [],
                explain: true,
                writeConcern: {w: 1},
            }),
        );
    });

    it("allows explain + writeConcern on a read-only pipeline (explain command)", function () {
        assert.commandWorked(
            internalDB.runCommand({
                explain: {aggregate: collName, pipeline: [], cursor: {}, writeConcern: {w: 1}},
                verbosity: "queryPlanner",
            }),
        );
    });
});
