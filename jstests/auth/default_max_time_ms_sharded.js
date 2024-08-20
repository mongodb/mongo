/**
 * Tests the defaultMaxTimeMS cluster parameter is respected on a sharded cluster. Auth is required
 * because the feature only works when auth is enabled.
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

let st = new ShardingTest({
    mongos: 1,
    shards: {nodes: 1},
    config: {nodes: 1},
    other: {keyFile: 'jstests/libs/key1'},
    mongosOptions: {setParameter: {'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"}},
});

let adminDB = st.s.getDB('admin');
assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
assert.eq(1, adminDB.auth("admin", "admin"));

// Prepare test data.
const dbName = "test";
const testDB = adminDB.getSiblingDB(dbName);
const collName = "test";
const coll = testDB.getCollection(collName);
for (let i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: 1}));
}

const slowStage = {
    $match: {
        $expr: {
            $function: {
                body: function() {
                    sleep(1000);
                    return true;
                },
                args: [],
                lang: "js"
            }
        }
    }
};

const aggCommand = {
    aggregate: collName,
    pipeline: [slowStage],
    cursor: {},
};

// Note the error could manifest as an Interrupted error sometimes due to the JavaScript execution
// being interrupted. This happens with both using the per-query option and the default parameter.
const expectedErrorsDueToMaxTimeMS = [ErrorCodes.Interrupted, ErrorCodes.MaxTimeMSExpired];

// No defaultMaxTimeMS is configured, the query succeeds.
assert.commandWorked(testDB.runCommand(aggCommand));

// No defaultMaxTimeMS is configured, but the query explicitly sets one, and fails.
assert.commandFailedWithCode(testDB.runCommand({...aggCommand, maxTimeMS: 100}),
                             expectedErrorsDueToMaxTimeMS);

// Set defaultMaxTimeMS to small value.
setDefaultReadMaxTimeMs(adminDB, 500);

// Prepare a regular user without the 'bypassDefaultMaxtimeMS' privilege.
adminDB.createUser({user: 'regularUser', pwd: 'password', roles: ["readAnyDatabase"]});
const regularUserConn = new Mongo(st.s.host).getDB('admin');
assert(regularUserConn.auth('regularUser', 'password'), "Auth failed");
const regularUserDB = regularUserConn.getSiblingDB(dbName);

jsTestLog("Executing query with defaultMaxTimeMS");

// Check the defaultMaxTimeMS causes the operation to fail on mongos.
assert.commandFailedWithCode(regularUserDB.runCommand(aggCommand), expectedErrorsDueToMaxTimeMS);

// Check the defaultMaxTimeMS is passed through to shard, by disabling it in mongos and still
// expecting an error.
let mongosFP = configureFailPoint(adminDB, "maxTimeNeverTimeOut");
assert.commandFailedWithCode(regularUserDB.runCommand(aggCommand), expectedErrorsDueToMaxTimeMS);
mongosFP.off();

// Unsets the default MaxTimeMS to make queries during teardown not time out.
setDefaultReadMaxTimeMs(adminDB, 0);

st.stop();
