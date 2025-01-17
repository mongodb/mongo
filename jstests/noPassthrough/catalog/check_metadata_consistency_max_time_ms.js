/*
 * Tests that the checkMetadataConsistency command honors maxTimeMS.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kDbName = 'testDB';
const kCollName = 'coll';

// Set up a database with a sharded collection to run checkMetadataConsistency on.
const st = new ShardingTest({shards: 1});

// Create one collection
const coll = st.s.getCollection(`${kDbName}.${kCollName}`);
assert.commandWorked(coll.insertOne({'a': 1}));

let asyncDropDatabase = new Thread(function(host, dbName) {
    let conn = new Mongo(host);
    const res = conn.getDB(dbName).runCommand({dropDatabase: 1});
    assert.commandWorked(res);
}, st.s.host, kDbName);

// Launch drop database operation and use a failpoint to make it block after taking the DDL lock
const blockDDLFailPoint =
    configureFailPoint(st.getPrimaryShard(kDbName), "hangBeforeRunningCoordinatorInstance");
asyncDropDatabase.start();
blockDDLFailPoint.wait();

// Launch checkMetadataConsistency with maxTimeMS.
// The command will block waiting for the dropCommand to release the DDL database lock and then will
// fail once it reaches the configured maxTimeMS.
const checkMetadataResult = st.s.adminCommand({checkMetadataConsistency: 1, maxTimeMS: 300});
assert.commandFailedWithCode(checkMetadataResult, ErrorCodes.MaxTimeMSExpired);

// Unblock dropDatabase DDL and wait until is completed.
blockDDLFailPoint.off();
asyncDropDatabase.join();
assert.eq(0, coll.countDocuments({}));

st.stop();
