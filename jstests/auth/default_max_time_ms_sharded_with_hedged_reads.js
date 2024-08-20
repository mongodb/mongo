/**
 * Tests interaction between defaultMaxTimeMS and maxTimeMSForHedgedReads.
 *
 * @tags: [
 *   creates_and_authenticates_user,
 *   requires_fcv_80,
 *   # Transactions aborted upon fcv upgrade or downgrade; cluster parameters use internal txns.
 *   required_auth,
 *   requires_sharding,
 *   uses_transactions,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function setDefaultReadMaxTimeMs(db, newValue) {
    assert.commandWorked(
        adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: newValue}}}));

    // Currently, the mongos cluster parameter cache is not updated on setClusterParameter. An
    // explicit call to getClusterParameter will refresh the cache.
    assert.commandWorked(adminDB.runCommand({getClusterParameter: "defaultMaxTimeMS"}));
}

function createUserAndLogin(adminDB, user, password, roles, newConnection = false) {
    // Error 51003 is returned when the user already exists.
    assert.commandWorkedOrFailedWithCode(
        adminDB.runCommand({createUser: user, pwd: password, roles: roles}), 51003);

    const loginDB = newConnection ? new Mongo(adminDB.getMongo().host).getDB('admin') : adminDB;
    assert.eq(1, loginDB.auth(user, password));
    return loginDB.getMongo();
}

function setupDirectShardConnDB(conn, dbName) {
    assert.eq(1, conn.getDB("local").auth("__system", "foopdedoop"));
    const db = conn.getDB(dbName);
    db.setProfilingLevel(1);
    return db;
}

const maxTimeMSForHedgedReads = 100000;
const defaultMaxTimeMS = 10000;

let st = new ShardingTest({
    mongos: [{
        setParameter: {
            logComponentVerbosity: tojson({network: {verbosity: 2}}),
            // Force the mongos's replica set monitors to always include all the eligible nodes.
            "failpoint.sdamServerSelectorIgnoreLatencyWindow": tojson({mode: "alwaysOn"}),
            'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}",
            maxTimeMSForHedgedReads: maxTimeMSForHedgedReads
        }
    }],
    shards: 1,
    config: 2,
    other: {keyFile: 'jstests/libs/key1'},
    rs: {nodes: 2, setParameter: {logComponentVerbosity: tojson({command: {verbosity: 1}})}}
});

const dbName = "test";
const collName = "test";

const adminDB = st.s.getDB('admin');
createUserAndLogin(adminDB, "admin", "admin", ["root"]);

const shardPrimaryDB = setupDirectShardConnDB(st.rs0.getPrimary(), dbName);
const shardSecondaryDB = setupDirectShardConnDB(st.rs0.getSecondary(), dbName);

// Prepare test data.
const testDB = adminDB.getSiblingDB(dbName);
const coll = testDB.getCollection(collName);
for (let i = 0; i < 100; ++i) {
    assert.commandWorked(coll.insert({a: 1}, {writeConcern: {w: 2}}));
}

// Set defaultMaxTimeMS to a value smaller than maxTimeMSForHedgedReads.
setDefaultReadMaxTimeMs(adminDB, defaultMaxTimeMS);

// Prepare a regular user without the 'bypassDefaultMaxtimeMS' privilege.
const regularUserDB =
    createUserAndLogin(adminDB, 'regularUser', 'password', ['readAnyDatabase'], true).getDB(dbName);

const slowCommand = {
    count: collName,
    query: {$where: "sleep(1000); return true;", a: 1},
    $readPreference: {mode: "nearest"}
};

const expectedErrorsDueToMaxTimeMS = [ErrorCodes.MaxTimeMSExpired];

// Force timeout on the shards, as opposed to mongos detecting the timeout and interrupting the ops
// on the shards. Interrupted operations do not log to system.profile.
let maxTimeNeverTimeOutFP = configureFailPoint(adminDB, "maxTimeNeverTimeOut");
// A response from one of the nodes will cause the other read to be cancelled, so the other op might
// still be interrupted even if maxTimeNeverTimeOut is enabled.
let doNotkillPendingRequestFP =
    configureFailPoint(adminDB, "networkInterfaceShouldNotKillPendingRequests");

// Check the defaultMaxTimeMS causes the operation to fail.
assert.commandFailedWithCode(regularUserDB.runCommand(slowCommand), expectedErrorsDueToMaxTimeMS);

// Check the operation has failed in both primary and secondary, and that it ran with a
// maxTimeMSOpOnly derived from defaultMaxTimeMS.
assert.soon(() => {
    function verifySystemProfile(db) {
        const ops = db.system.profile.find({"command.count": {$exists: true}}).toArray();
        if (ops.length == 0) {
            return false;
        }
        assert.eq(ops.length, 1, tojson(ops));
        // maxTimeMSForHedgedReads is much larger than defaultMaxTimeMS, this verifies
        // maxTimeMSForHedgedReads is not used in the requests sent to shards from mongos.
        assert.lt(ops[0].command.maxTimeMSOpOnly, defaultMaxTimeMS * 1.5, tojson(ops[0]));
        assert(expectedErrorsDueToMaxTimeMS.includes(ops[0].errCode),
               "Unexpected error code: " + tojson(ops[0]));
        return true;
    }
    return verifySystemProfile(shardPrimaryDB) && verifySystemProfile(shardSecondaryDB);
}, "Expected requests to shards to hit MaxTimeMSExpired and log a system.profile entry");

// Unsets the default MaxTimeMS to make queries during teardown not time out.
setDefaultReadMaxTimeMs(adminDB, 0);
maxTimeNeverTimeOutFP.off();
doNotkillPendingRequestFP.off();

st.stop();
