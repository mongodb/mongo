/**
 * Tests that 'defaultMaxTimeMS' is applied correctly to aggregate commands.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   requires_auth,
 *   # Transactions aborted upon fcv upgrade or downgrade; cluster parameters use internal txns.
 *   uses_transactions,
 *   requires_fcv_80,
 *   requires_scripting,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function setDefaultReadMaxTimeMS(db, newValue) {
    assert.commandWorked(
        db.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: newValue}}}));

    // Currently, the mongos cluster parameter cache is not updated on setClusterParameter. An
    // explicit call to getClusterParameter will refresh the cache.
    assert.commandWorked(db.runCommand({getClusterParameter: "defaultMaxTimeMS"}));
}

function runTests(conn) {
    const dbName = jsTestName();
    const adminDB = conn.getDB("admin");

    // Create the admin user, which is used to insert.
    adminDB.createUser({user: 'admin', pwd: 'admin', roles: ['root']});
    assert.eq(1, adminDB.auth("admin", "admin"));

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

    // Sets the default maxTimeMS for read operations with a small value.
    setDefaultReadMaxTimeMS(adminDB, 1);

    // Prepare a regular user without the 'bypassDefaultMaxTimeMS' privilege.
    adminDB.createUser({user: 'regularUser', pwd: 'password', roles: ["readWriteAnyDatabase"]});

    const regularUserConn = new Mongo(conn.host).getDB('admin');
    assert(regularUserConn.auth('regularUser', 'password'), "Auth failed");
    const regularUserDB = regularUserConn.getSiblingDB(dbName);

    // A long running aggregation will fail even without specifying a maxTimeMS option.
    // Note the error could manifest as an Interrupted error sometimes due to the JavaScript
    // execution being interrupted. This happens with both using the per-query option and the
    // default parameter.
    assert.commandFailedWithCode(regularUserDB.runCommand({
        aggregate: collName,
        pipeline: [slowStage],
        cursor: {},
    }),
                                 [ErrorCodes.Interrupted, ErrorCodes.MaxTimeMSExpired]);

    // Specifying a maxTimeMS option will overwrite the default value.
    assert.commandWorked(regularUserDB.runCommand({
        aggregate: collName,
        pipeline: [slowStage],
        cursor: {},
        maxTimeMS: 0,
    }));

    // If the aggregate performs a write operation, the time limit will not apply.
    assert.commandWorked(regularUserDB.runCommand({
        aggregate: collName,
        pipeline: [
            slowStage,
            {
                $out: "foo",
            }
        ],
        cursor: {},
    }));
    assert.commandWorked(regularUserDB.runCommand({
        aggregate: collName,
        pipeline: [
            slowStage,
            {
                $merge: "bar",
            }
        ],
        cursor: {},
    }));

    // Unsets the default MaxTimeMS to make queries not to time out in the following code.
    setDefaultReadMaxTimeMS(adminDB, 0);

    adminDB.logout();
    regularUserDB.logout();
}

const rst = new ReplSetTest({nodes: 1, keyFile: "jstests/libs/key1"});
rst.startSet();
rst.initiate();
runTests(rst.getPrimary());
rst.stopSet();

const st = new ShardingTest({
    mongos: 1,
    shards: {nodes: 1},
    config: {nodes: 1},
    keyFile: 'jstests/libs/key1',
    mongosOptions: {setParameter: {'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"}},
});
runTests(st.s);
st.stop();
