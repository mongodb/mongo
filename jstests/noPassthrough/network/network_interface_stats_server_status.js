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

function validateHistogram(stats, histogramName, name) {
    const histogram = stats[histogramName];
    assert(
        histogram.hasOwnProperty("totalCount"),
        "Missing 'totalCount' in " + histogramName + " for " + name,
        {stats},
    );

    let bucketCountSum = 0;
    for (const bucketKey of Object.keys(histogram)) {
        if (bucketKey === "totalCount") {
            continue;
        }
        assert(
            histogram[bucketKey].hasOwnProperty("count"),
            "Missing 'count' in " + histogramName + " bucket " + bucketKey + " for " + name,
            {stats},
        );
        bucketCountSum += histogram[bucketKey]["count"];
    }

    assert.eq(
        histogram["totalCount"],
        bucketCountSum,
        "totalCount should equal the sum of bucket counts in " + histogramName + " for " + name,
        {stats},
    );
    assert.gt(
        histogram["totalCount"],
        0,
        "Expected " + histogramName + " totalCount > 0 for " + name,
        {stats},
    );
}

let totalExecuted = 0;
for (let i = 0; i < interfaceNames.length; i++) {
    const name = interfaceNames[i];
    jsTestLog("Validating stats for: " + name);
    const stats = section[name];
    assert(stats, "Stats for " + name + " should not be null");

    for (const fieldName of [
        "scheduled",
        "executed",
        "averageWaitTimeMicros",
        "averageRunTimeMicros",
    ]) {
        assert(stats.hasOwnProperty(fieldName), "Missing '" + fieldName + "' for " + name, {stats});
        assert.gte(stats[fieldName], 0, "Expected " + fieldName + " >= 0 for " + name, {stats});
    }

    if (name.includes("TaskExecutorPool-")) {
        validateHistogram(stats, "waitTime", name);
        validateHistogram(stats, "runTime", name);
    } else {
        assert(
            !stats.hasOwnProperty("waitTime") && !stats.hasOwnProperty("runTime"),
            "Unexpected reactor histograms in server status for " + name,
            {stats},
        );
    }

    totalExecuted += stats["executed"];
}

assert.gt(totalExecuted, 0, "Expected executed > 0 across all interfaces");

assert.commandWorked(testDB.dropDatabase());
st.stop();
