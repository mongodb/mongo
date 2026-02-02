/**
 * TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.
 *
 * Tests that renaming a timeseries collection during FCV downgrade does not cause the collection
 * to be skipped during the viewless-to-legacy conversion.
 *
 * @tags: [
 *   requires_timeseries,
 *   featureFlagCreateViewlessTimeseriesCollections,
 *   uses_parallel_shell,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (lastLTSFCV != "8.0") {
    jsTest.log.info("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

const st = new ShardingTest({shards: 1, rs: {nodes: 1}});

const dbName = jsTestName();
const db = st.s.getDB(dbName);
const adminDB = st.s.getDB("admin");

assert(FeatureFlagUtil.isPresentAndEnabled(db, "CreateViewlessTimeseriesCollections"));
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Checks if system.buckets.<collName> exists, indicating legacy (viewful) format.
function isLegacyTimeseriesFormat(collName) {
    return (
        adminDB
            .getSiblingDB(dbName)
            .getCollection("system.buckets." + collName)
            .exists() !== null
    );
}

jsTest.log.info("Creating viewless timeseries collection");
const testColl = db["testTs"];
assert.commandWorked(
    db.createCollection("testTs", {
        timeseries: {timeField: "t", metaField: "m"},
    }),
);
assert.commandWorked(
    testColl.insertMany([
        {t: ISODate(), m: 100, foo: 1},
        {t: ISODate(), m: 200, foo: 2},
    ]),
);

jsTest.log.info("Verifying collection is in viewless format before downgrade");
assert.eq(false, isLegacyTimeseriesFormat("testTs"));

// Enable the failpoint on the config server primary to pause after enumerating collections
const configPrimary = st.configRS.getPrimary();
const hangFp = configureFailPoint(configPrimary, "hangAfterEnumeratingTimeseriesCollectionsForFCV");

jsTest.log.info("Starting FCV downgrade in parallel shell");
const awaitDowngrade = startParallelShell(
    funWithArgs(function (fcv) {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: fcv, confirm: true}));
    }, lastLTSFCV),
    st.s.port,
);

jsTest.log.info("Waiting for failpoint to be hit (setFCV has enumerated collections)");
hangFp.wait();

jsTest.log.info("Renaming collection while FCV downgrade is paused");
const renamedCollName = "renamedTs";
assert.commandWorked(
    db.adminCommand({
        renameCollection: testColl.getFullName(),
        to: db.getName() + "." + renamedCollName,
    }),
);

jsTest.log.info("Turning off failpoint to resume FCV downgrade");
hangFp.off();

jsTest.log.info("Waiting for FCV downgrade to complete");
awaitDowngrade();

jsTest.log.info("Verifying renamed collection was converted to legacy format");
assert.eq(true, isLegacyTimeseriesFormat(renamedCollName));

// Verify the data is still accessible after conversion
jsTest.log.info("Verifying data is accessible after conversion");
const renamedColl = db[renamedCollName];
assert.eq(2, renamedColl.countDocuments({}));

st.stop();
