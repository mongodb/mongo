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
    setParameter: "trafficRecordingDirectory=" + dbPath
};
// run all tests sharded
const conn = new ShardingTest({
    shards: 1,
    mongos: 1,
    config: 1,
    keyFile: "jstests/libs/key1",
    other:
        {shardOptions: opts, mongosOptions: {setParameter: "trafficRecordingDirectory=" + dbPath}}
});
runAllCommandsBuiltinRoles(conn);
conn.stop();
