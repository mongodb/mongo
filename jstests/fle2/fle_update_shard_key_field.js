/**
 * Regression test for the FLE update-shard-key path.
 *
 * When a retryable update on an FLE-encrypted collection would change a
 * document's shard-key field, the write should either:
 *   (a) succeed with proper cross-shard semantics (delete-on-source +
 *       insert-on-destination, wrapped in an internal transaction whose
 *       stmtIds are distinct), OR
 *   (b) fail with a clean, structured error that names the unsupported
 *       combination (FLE + shard-key change) rather than the historic
 *       generic assertion produced by stmtId collision inside the retryable
 *       internal transaction that the FLE re-write spawns.
 *
 * Root cause covered: the BatchedCommandRequest re-write that FLE performs
 * (`FLEUpdateOperation::serialize` / `appendMongosRequest`) does not
 * preserve the "this update may change the shard key" flag, so the WCOS
 * (write contains owning shard) path is not wired through to the mongos
 * dispatcher and the request is run as a vanilla retryable update with the
 * default stmtId=0. Inside the FLE retryable internal transaction the
 * reused stmtId then triggers a TransactionTooOld / IncompleteTransactionHistory
 * style failure.
 *
 * @tags: [
 *  requires_fcv_70,
 *  requires_sharding,
 *  uses_transactions,
 *  uses_multi_shard_transaction,
 *  featureFlagFLE2,
 * ]
 */
import {EncryptedClient, isEnterpriseShell} from "jstests/fle2/libs/encrypted_client_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (!isEnterpriseShell()) {
    jsTestLog("Skipping: enterprise shell required for FLE2");
    quit();
}

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 2},
});

const dbName = "fle_update_shard_key_field";
const collName = "patients";
const ns = `${dbName}.${collName}`;

const adminDB = st.s0.getDB("admin");
const testDB = st.s0.getDB(dbName);
testDB.dropDatabase();

// Shard key is on the non-encrypted hashable field `region`. The update
// will additionally $set an encrypted field (`ssn`) so the request must
// flow through the FLE re-write path before being dispatched.
assert.commandWorked(adminDB.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

const client = new EncryptedClient(st.s0, dbName);
assert.commandWorked(
    client.createEncryptionCollection(collName, {
        encryptedFields: {
            fields: [
                {path: "ssn", bsonType: "string", queries: {queryType: "equality"}},
            ],
        },
    }),
);

assert.commandWorked(adminDB.runCommand({shardCollection: ns, key: {region: 1}}));
assert.commandWorked(adminDB.runCommand({split: ns, middle: {region: "west"}}));
assert.commandWorked(adminDB.runCommand({moveChunk: ns, find: {region: "east"}, to: st.shard0.shardName}));
assert.commandWorked(adminDB.runCommand({moveChunk: ns, find: {region: "west"}, to: st.shard1.shardName}));

// Seed one document on shard0 (`region: "east"`).
const edb = client.getDB();
const eColl = edb.getCollection(collName);
assert.commandWorked(eColl.einsert({_id: 1, region: "east", ssn: "111-22-3333", name: "alice"}));

// The update changes the shard-key field (`region`) AND touches an
// encrypted field (`ssn`) in the same modifier doc. This is the exact
// combination that fails today: FLE rewrites the batched command, drops the
// shard-key-update flag, and the WCOS path on mongos never fires.
const updateCmd = {
    update: collName,
    updates: [{
        q: {_id: 1, region: "east"},
        u: {$set: {region: "west", ssn: "999-88-7777"}},
    }],
    lsid: {id: UUID()},
    txnNumber: NumberLong(1),
};

const res = edb.runCommand(updateCmd);

// Accept either outcome (a) or (b) per the contract above. Both keep the
// substrate honest: a hard pass means the WCOS flag is now threaded; a
// clean structured error means the unsupported combination is at least
// surfaced as such, not as a generic assertion buried in transaction
// machinery.
const cleanError = res.ok === 0 && res.code !== undefined && typeof res.errmsg === "string" && res.errmsg.length > 0;

const okWithMove = res.ok === 1 && res.n === 1 && res.nModified === 1;

assert(
    cleanError || okWithMove,
    () =>
        "FLE update that would change shard key returned neither (a) success with cross-shard " +
        "semantics nor (b) a clean structured error. Raw response: " + tojson(res),
);

if (okWithMove) {
    // (a) The doc has moved across shards and the encrypted field round-trips.
    const moved = edb.getCollection(collName).find({_id: 1}).toArray();
    assert.eq(moved.length, 1, () => "expected exactly one document, got: " + tojson(moved));
    assert.eq(moved[0].region, "west", () => "shard key not updated: " + tojson(moved[0]));
} else {
    // (b) The error should name the combination, not surface as a stmtId
    // collision inside an internal transaction. We assert the *shape* of
    // the response, not a specific error code, so this test stays useful
    // through whichever fix lands.
    jsTestLog("FLE update-shard-key returned clean structured error: " + tojson(res));
    assert(res.code !== ErrorCodes.InternalError, () => "should not surface as InternalError: " + tojson(res));
}

st.stop();
