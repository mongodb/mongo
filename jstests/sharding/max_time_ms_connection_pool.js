/**
 * Tests the rewrite of PooledConnectionAcquisitionExceededTimeLimit coming from
 * `executor/connection_pool.cpp` into MaxTimeMSError when MaxTimeMS option is set for a given
 * sharding command.
 *
 * @tags: [
 *   requires_fcv_61,
 *   does_not_support_stepdowns,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const databaseName = "my-database";
const collectionName = "my-collection";

function generateFindCommand(maxTimeMS) {
    return {find: collectionName, maxTimeMS: maxTimeMS};
}

const st = new ShardingTest({shards: 1, mongos: 1});
const database = st.s0.getDB(databaseName);
const collection = database.getCollection(collectionName);

assert.commandWorked(
    database.runCommand({create: collection.getName(), writeConcern: {w: "majority"}}));

assert.commandWorked(database.runCommand(generateFindCommand(1000)));

// Mimic 1 second connection acquisition timeout via fail point.
const failpoint =
    configureFailPoint(st.s, "forceExecutorConnectionPoolTimeout", {"timeout": 1000}, "alwaysOn");

// We test that connection acquisition errors are rewritten to MaxTimeMSExpired due to the user
// provided maxTimeMS.
assert.commandFailedWithCode(database.runCommand(generateFindCommand(1000)),
                             ErrorCodes.MaxTimeMSExpired);

failpoint.off();

st.stop();
