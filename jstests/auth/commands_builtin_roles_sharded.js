/**
 * Exhaustive test for authorization of commands with builtin roles.
 *
 * Runs tests on a sharded cluster.
 *
 * The test logic implemented here operates on the test cases defined
 * in jstests/auth/lib/commands_lib.js
 *
 * @tags: [requires_sharding]
 */

import {runAllCommandsBuiltinRoles} from "jstests/auth/lib/commands_builtin_roles.js";

const dbPath = MongoRunner.toRealDir("$dataDir/commands_built_in_roles_sharded/");
mkdir(dbPath);
const opts = {
    auth: "",
    // We have to set the mongotHost parameter for the $search-related tests to pass configuration
    // checks.
    setParameter: {trafficRecordingDirectory: dbPath, mongotHost: "localhost:27017"}
};
// run all tests sharded
const conn = new ShardingTest({
    shards: 1,
    mongos: 1,
    config: 1,
    keyFile: "jstests/libs/key1",
    other: {
        shardOptions: opts,
        // We have to set the mongotHost parameter for the $search-related tests to pass
        // configuration checks.
        mongosOptions:
            {setParameter: {trafficRecordingDirectory: dbPath, mongotHost: "localhost:27017"}}
    }
});
runAllCommandsBuiltinRoles(conn);
conn.stop();
