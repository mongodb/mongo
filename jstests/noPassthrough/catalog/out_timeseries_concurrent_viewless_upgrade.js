/**
 * Reproduces a $out-to-timeseries failure that happens when the target timeseries collection is
 * upgraded from viewful (legacy) to viewless concurrently with $out, between $out's rename and its
 * view-creation step.
 *
 * Steps:
 *   1. At lastLTS FCV, 'out_coll' is a legacy timeseries collection with 'expireAfterSeconds'.
 *   2. $out renames its temp buckets collection onto 'system.buckets.out_coll', then pauses at
 *      'outWaitAfterTempCollectionRenameBeforeView' before creating the timeseries view.
 *   3. setFCV(latest) upgrades 'out_coll' to viewless, renaming 'system.buckets.out_coll' ->
 *      'out_coll' and adding the system-managed 'fixedBucketing: false' option.
 *   4. $out resumes and creates the view against the now-viewless collection, hitting
 *      NamespaceExists. Its escape hatch (_handleTimeseriesCreateError) must swallow it.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sharding,
 *   requires_timeseries,
 *   featureFlagCreateViewlessTimeseriesCollections,
 *   featureFlagFixedBucketingCatalog,
 * ]
 */

import {waitForCurOpByFailPointNoNS} from "jstests/libs/curop_helpers.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1});

const mongos = st.s;
const shardPrimary = st.rs0.getPrimary();

const dbName = "out_ts_viewless_race";
const timeField = "t";
const metaField = "m";
const srcCollName = "src";
const outCollName = "out_coll";

const mongosDB = mongos.getDB(dbName);

function setClusterFCV(version) {
    assert.commandWorked(
        mongos.adminCommand({setFeatureCompatibilityVersion: version, confirm: true}),
    );
}

// Insert some documents so $out has something to write.
function setupSource() {
    mongosDB[srcCollName].drop();
    const docs = [];
    for (let i = 0; i < 5; i++) {
        docs.push({[timeField]: new Date(), [metaField]: i, v: i});
    }
    assert.commandWorked(mongosDB[srcCollName].insertMany(docs));
}

// A timeseries collection is viewless once its backing 'system.buckets.*' collection no longer
// exists.
function isViewless(collName) {
    const bucketsInfos = mongosDB.getCollectionInfos({
        name: "system.buckets." + collName,
    });
    return bucketsInfos.length === 0;
}

// Downgrade so newly created timeseries collections are legacy (viewful), forcing $out down the
// legacy path that creates a user-facing view.
setClusterFCV(lastLTSFCV);

setupSource();

// Create the target with 'expireAfterSeconds', which is what makes the later bare 'create' collide
// with the upgraded collection.
mongosDB[outCollName].drop();
assert.commandWorked(
    mongosDB.createCollection(outCollName, {
        timeseries: {timeField, metaField},
        expireAfterSeconds: 3600,
    }),
);
assert(
    !isViewless(outCollName),
    "out_coll should be a legacy (viewful) timeseries collection before $out",
);

// Pause $out (matched by comment) after it renames the temp buckets collection onto the target but
// before it creates the view.
assert.commandWorked(
    shardPrimary.getDB("admin").runCommand({
        configureFailPoint: "outWaitAfterTempCollectionRenameBeforeView",
        mode: "alwaysOn",
        data: {comment: "pauseHere"},
    }),
);

const outThread = new Thread(
    (host, dbName, srcName, outName, timeField, metaField) => {
        const conn = new Mongo(host);
        return conn.getDB(dbName).runCommand({
            aggregate: srcName,
            pipeline: [
                {
                    $out: {
                        db: dbName,
                        coll: outName,
                        timeseries: {timeField: timeField, metaField: metaField},
                    },
                },
            ],
            cursor: {},
            comment: "pauseHere",
        });
    },
    mongos.host,
    dbName,
    srcCollName,
    outCollName,
    timeField,
    metaField,
);
outThread.start();

// Wait until $out is suspended between the rename and the view creation.
waitForCurOpByFailPointNoNS(
    shardPrimary.getDB(dbName),
    "outWaitAfterTempCollectionRenameBeforeView",
);

// While $out is suspended, upgrade to latest. The viewless upgrade renames 'system.buckets.out_coll'
// onto 'out_coll' and adds 'fixedBucketing: false'. It does not wait for the suspended $out.
setClusterFCV(latestFCV);
assert(
    isViewless(outCollName),
    "out_coll should be a viewless timeseries collection after the FCV upgrade",
);

// Release $out so it creates the view against the now-viewless collection.
assert.commandWorked(
    shardPrimary.getDB("admin").runCommand({
        configureFailPoint: "outWaitAfterTempCollectionRenameBeforeView",
        mode: "off",
    }),
);

const outRes = outThread.returnData();
assert.commandWorked(
    outRes,
    "$out must tolerate the target being upgraded to viewless during view creation",
);

// The output collection must be a timeseries collection with the $out'd documents.
const infos = mongosDB.getCollectionInfos({name: outCollName});
assert.eq(infos.length, 1, `Expected exactly one collection info: ${tojson(infos)}`);
assert.eq(infos[0].type, "timeseries", `Expected timeseries type: ${tojson(infos[0])}`);
assert.eq(mongosDB[outCollName].countDocuments({}), 5, "Unexpected document count after $out");

st.stop();
