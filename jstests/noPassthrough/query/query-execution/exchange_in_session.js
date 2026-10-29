/**
 * Be sure that an exchange won't deadlock when one of the consumer's buffers is full. Iterates two
 * consumers on an Exchange with a very small buffer. This test was designed to reproduce
 * SERVER-37499.
 * @tags: [
 *   requires_sharding,
 *   uses_transactions,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test manually simulates a session, which is not compatible with implicit sessions.
TestData.disableImplicitSessions = true;

// Start a sharded cluster. For this test, we'll just need to talk to the shard directly.
const st = new ShardingTest({shards: 1, mongos: 1});

const adminDB = st.shard0.getDB("admin");

// Exchange requires an internal client connection. Mark the shard connection as internal.
const internalShard0 = (() => {
    const conn = new Mongo(st.shard0.host);
    assert.commandWorked(
        conn.getDB("admin").runCommand({
            hello: 1,
            internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
        }),
    );
    return conn;
})();
const session = internalShard0.startSession();
const shardDB = session.getDatabase("test");
const collName = "exchange_in_session";

// Use a regular (non-internal) connection for inserts to avoid the internal client
// writeConcern requirement on non-transaction commands.
const regularColl = st.shard0.getDB("test")[collName];

let bigString = "";
for (let i = 0; i < 20; i++) {
    bigString += "s";
}

// Insert some documents.
const nDocs = 50;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(regularColl.insert({_id: i, bigString: bigString}));
}

// Pass writeConcern so that abortTransaction (also sent through the internal
// client session) satisfies the internal client writeConcern requirement.
session.startTransaction({writeConcern: {}});

// Set up an Exchange with two cursors.
let res = assert.commandWorked(
    shardDB.runCommand({
        aggregate: collName,
        pipeline: [],
        exchange: {
            policy: "keyRange",
            consumers: NumberInt(2),
            key: {_id: 1},
            boundaries: [{a: MinKey}, {a: nDocs / 2}, {a: MaxKey}],
            consumerIds: [NumberInt(0), NumberInt(1)],
            bufferSize: NumberInt(128),
        },
        cursor: {batchSize: 0},
        readConcern: {},
    }),
);

function spawnShellToIterateCursor(cursorId) {
    let code = `const cursor = ${tojson(cursorId)};`;
    code += `const sessionId = ${tojson(session.getSessionId())};`;
    code += `const collName = "${collName}";`;
    /* eslint-disable */
    function iterateCursorWithNoDocs() {
        const getMoreCmd = {
            getMore: cursor.id,
            collection: collName,
            batchSize: 4,
            lsid: sessionId,
            txnNumber: NumberLong(0),
            autocommit: false,
        };

        let resp = null;
        while (!resp || resp.cursor.id != 0) {
            resp = assert.commandWorked(db.runCommand(getMoreCmd));
        }
    }
    /* eslint-enable */
    code += `(${iterateCursorWithNoDocs.toString()})();`;
    return startParallelShell(code, st.rs0.getPrimary().port);
}

let parallelShells = [];
for (let curs of res.cursors) {
    parallelShells.push(spawnShellToIterateCursor(curs.cursor));
}

assert.soon(function () {
    for (let waitFn of parallelShells) {
        waitFn();
    }
    return true;
});

assert.commandWorked(session.abortTransaction_forTesting());

st.stop();
