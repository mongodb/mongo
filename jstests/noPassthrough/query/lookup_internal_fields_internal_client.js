/**
 * Tests that the internal-only $lookup fields $_internalFromIsAView and
 * $_internalFieldMatchPipelineIdx are rejected from external clients but still accepted from an
 * internal client (the router sets them on the mongos->shard dispatch path). This test sends them
 * over an internal-client connection and asserts they parse and run.
 *
 * The negative test (external client rejected with 5491300) lives in
 * jstests/aggregation/sources/lookup/lookup_internal_fields_rejected.js.
 * @tags: [
 *   requires_fcv_91,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("$lookup internal-only fields accepted from an internal client", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({});
        assert.neq(null, this.conn, "mongod failed to start");

        const setupDB = this.conn.getDB("test");
        assert.commandWorked(setupDB.local.insert({_id: 0, fk: 1}));
        assert.commandWorked(setupDB.foreign.insert({_id: 1, x: 1}));

        // Create an internal client connection to exercise the internal-only stage, leaving
        // the main connection as a normal client so that setup writes and shutdown-time
        // validation hooks are unaffected.
        this.internalConn = new Mongo(this.conn.host);
        assert.commandWorked(
            this.internalConn.getDB("admin").runCommand({
                hello: 1,
                internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
            }),
        );
        this.internalDB = this.internalConn.getDB("test");
    });

    after(function () {
        this.internalConn.close();
        MongoRunner.stopMongod(this.conn);
    });

    function runLookup(db, extraLookupFields) {
        return db.runCommand({
            aggregate: "local",
            pipeline: [
                {
                    $lookup: Object.assign(
                        {
                            from: "foreign",
                            localField: "fk",
                            foreignField: "x",
                            pipeline: [{$match: {x: {$gte: 0}}}],
                            as: "joined",
                        },
                        extraLookupFields,
                    ),
                },
            ],
            cursor: {},
            // Internal-client connections must specify an explicit writeConcern.
            writeConcern: {w: "majority"},
        });
    }

    it("accepts $_internalFromIsAView", function () {
        assert.commandWorked(runLookup(this.internalDB, {$_internalFromIsAView: true}));
    });

    it("accepts an in-range $_internalFieldMatchPipelineIdx", function () {
        assert.commandWorked(
            runLookup(this.internalDB, {$_internalFieldMatchPipelineIdx: NumberLong(1)}),
        );
    });
});
