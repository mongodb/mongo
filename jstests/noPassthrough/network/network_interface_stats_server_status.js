/**
 * Tests that the 'networkInterfaceStats' section of serverStatus on mongos reports reactor
 * wait and run times for the sharding task executor pool network interfaces.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, mongos: 1});
const mongos = st.s;
const adminDB = mongos.getDB("admin");
const testDBName = jsTestName();
const testDB = mongos.getDB(testDBName);
const collName = "test_coll";

assert.commandWorked(adminDB.runCommand({enableSharding: testDBName}));
assert.commandWorked(
    adminDB.runCommand({shardCollection: testDB[collName].getFullName(), key: {_id: 1}}),
);

const bulk = testDB[collName].initializeUnorderedBulkOp();
for (let i = 0; i < 50; i++) {
    bulk.insert({_id: i, value: "data_" + i});
}
assert.commandWorked(bulk.execute());

for (let i = 0; i < 10; i++) {
    assert.eq(testDB[collName].find({_id: {$gte: i * 5, $lt: (i + 1) * 5}}).itcount(), 5);
}

const serverStatus = assert.commandWorked(adminDB.runCommand({serverStatus: 1}));
const section = serverStatus["shardingTaskExecutorMetrics"];
assert(
    section,
    "Expected 'shardingTaskExecutorMetrics' section in serverStatus. Keys: " +
        tojson(Object.keys(serverStatus)),
);

jsTestLog("shardingTaskExecutorMetrics " + tojson(section));

const interfaceNames = Object.keys(section);
assert.gt(interfaceNames.length, 0, "Expected at least one network interface entry");

let totalExecuted = 0;
for (let i = 0; i < interfaceNames.length; i++) {
    const name = interfaceNames[i];
    jsTestLog("Validating stats for: " + name);
    const stats = section[name];
    assert(stats, "Stats for " + name + " should not be null");

    assert(
        stats.hasOwnProperty("scheduled"),
        "Missing 'scheduled' for " + name + ": " + tojson(stats),
    );
    assert(
        stats.hasOwnProperty("executed"),
        "Missing 'executed' for " + name + ": " + tojson(stats),
    );
    assert(
        stats.hasOwnProperty("averageWaitTimeMicros"),
        "Missing 'averageWaitTimeMicros' for " + name + ": " + tojson(stats),
    );
    assert(
        stats.hasOwnProperty("averageRunTimeMicros"),
        "Missing 'averageRunTimeMicros' for " + name + ": " + tojson(stats),
    );

    assert.gte(stats["scheduled"], 0);
    assert.gte(stats["executed"], 0);
    assert.gte(stats["averageWaitTimeMicros"], 0);
    assert.gte(stats["averageRunTimeMicros"], 0);

    assert(
        !stats.hasOwnProperty("waitTime"),
        "Unexpected 'waitTime' histogram in server status for " + name,
    );
    assert(
        !stats.hasOwnProperty("runTime"),
        "Unexpected 'runTime' histogram in server status for " + name,
    );

    totalExecuted += stats["executed"];
}

assert.gt(totalExecuted, 0, "Expected executed > 0 across all interfaces");

assert.commandWorked(testDB.dropDatabase());
st.stop();
