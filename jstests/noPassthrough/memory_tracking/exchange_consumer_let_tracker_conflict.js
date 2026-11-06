/**
 * An aggregate command with an exchange stage and an array-valued 'let' parameter used to trigger
 * lazy creation of additional memory trackers. Since the exchange expects to manage producer's
 * tracker, consumers created a second tracker that was shared with their cursors and republished on
 * their first getMore, so a consumer winning the load race tripped an invariant. Regression
 * test for SERVER-135546.
 *
 * @tags: [
 *   requires_getmore,
 *   does_not_support_stepdowns,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("exchange consumer with an array-valued 'let'", function () {
    const collName = jsTestName();
    const numConsumers = 2;

    let conn;
    let db;
    let coll;
    let internalConn;
    let session;

    // A connection presenting itself as an old internal client, which is what allows a session to
    // own several cursors at once.
    function makeInternalConnection() {
        const c = new Mongo(conn.host);
        assert.commandWorked(
            c.getDB("admin").runCommand({
                hello: 1,
                internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
            }),
        );
        return c;
    }

    // Opens the exchange aggregate and returns the cursor id of each consumer. Nothing is returned
    // in the initial batch, so the getMore issued by the caller is what makes a consumer load.
    function openConsumerCursors() {
        const aggResult = assert.commandWorked(
            session.getDatabase(db.getName()).runCommand({
                aggregate: collName,
                pipeline: [{$group: {_id: "$a", v: {$push: "$b"}}}],
                exchange: {
                    policy: "roundrobin",
                    consumers: NumberInt(numConsumers),
                    orderPreserving: false,
                    bufferSize: NumberInt(1),
                    key: {},
                },
                // Raw array-valued 'let' (mongos would serialize this as {$literal: [...]}). Sent
                // raw, it parses as an ExpressionArray and charges memory during foldConstant().
                let: {arr: [1, 2, 3, 4, 5]},
                cursor: {batchSize: 0},
                readConcern: {},
                writeConcern: {},
            }),
        );
        assert.eq(numConsumers, aggResult.cursors.length, "expected one cursor per consumer", {
            aggResult,
        });
        return aggResult.cursors.map((c) => c.cursor.id);
    }

    function getMore(cursorId, batchSize) {
        return internalConn.getDB(db.getName()).runCommand({
            getMore: cursorId,
            collection: collName,
            batchSize: NumberInt(batchSize),
            lsid: session.getSessionId(),
        });
    }

    function killCursors(cursorIds) {
        try {
            internalConn.getDB(db.getName()).runCommand({
                killCursors: collName,
                cursors: cursorIds,
                lsid: session.getSessionId(),
            });
        } catch (e) {
            // Tripping the guard below kills the server, in which case its cursors died with it and
            // the failing assertion is far more informative than this cleanup error.
        }
    }

    before(function () {
        conn = MongoRunner.runMongod({});
        assert.neq(null, conn, "mongod was unable to start up");
        db = conn.getDB("test");
        coll = db[collName];
        coll.drop();
        const docs = [];
        for (let i = 0; i < 100; ++i) {
            docs.push({_id: i, a: i, b: "abcdefghijklmnopqrstuvwxyz"});
        }
        assert.commandWorked(coll.insertMany(docs));

        internalConn = makeInternalConnection();
        session = internalConn.startSession();
    });

    after(function () {
        if (!conn) {
            return;
        }

        try {
            session?.endSession();
        } finally {
            MongoRunner.stopMongod(conn);
        }
    });

    it("lets a consumer load its cursor without tripping the tracker publish guard", function () {
        const cursorIds = openConsumerCursors();
        try {
            // Consumer 0 drives a load. Before the fix, its cursor carried the foreign tracker
            // created by the per-consumer 'let' fold, so this getMore tripped
            // tassert(12920100).
            assert.commandWorked(
                getMore(cursorIds[0], 1),
                "consumer 0 getMore must not trip the exchange tracker publish guard",
            );
            // Consumer 1 is unaffected.
            assert.commandWorked(getMore(cursorIds[1], 1), "consumer 1 getMore must succeed", {
                cursorId: cursorIds[1],
            });
        } finally {
            killCursors(cursorIds);
        }
    });
});
