/*
 * Test the test command createUnsplittableCollection. This command is a temporary wrapper on
 * shardCollection that allows you to create unsplittable collection aka tracked unsharded
 * collection. Since we use the same coordinator, we both check the createUnsplittableCollection
 * works and that shardCollection won't generate unsplittable collection.
 * @tags: [
 *   requires_fcv_81,
 *   assumes_balancer_off,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const kDbName = "test";

const st = new ShardingTest({shards: 2});
const mongos = st.s;
const db = st.s.getDB(kDbName);

let collCounter = 0;

function genCollName() {
    return `tsColl_${collCounter++}`;
}

jsTest.log("Running test command createUnsplittableCollection to track an unsharded collection");
{
    const coll = db[genCollName()];
    assert.commandWorked(mongos.getDB("admin").runCommand({enableSharding: db.getName()}));

    let result = st.s.getDB(kDbName).runCommand({createUnsplittableCollection: coll.getName()});
    assert.commandWorked(result);

    // checking consistency
    let configDb = mongos.getDB("config");

    let unshardedColl = configDb.collections.findOne({_id: coll.getFullName()});
    assert.eq(unshardedColl._id, coll.getFullName());
    assert.eq(unshardedColl._id, coll.getFullName());
    assert.eq(unshardedColl.unsplittable, true);
    assert.eq(unshardedColl.key, {_id: 1});

    let configChunks = configDb.chunks.find({uuid: unshardedColl.uuid}).toArray();
    assert.eq(configChunks.length, 1);
}

jsTest.log("Check that createCollection can create a tracked unsharded collection");
{
    const coll = db[genCollName()];
    assert.commandWorked(st.s.getDB(kDbName).runCommand({createUnsplittableCollection: coll.getName()}));

    // running the same request again will behave as no-op
    assert.commandWorked(st.s.getDB(kDbName).runCommand({createUnsplittableCollection: coll.getName()}));

    let res = assert.commandWorked(
        st.getPrimaryShard(kDbName).getDB(kDbName).runCommand({listIndexes: coll.getName()}),
    );
    let indexes = res.cursor.firstBatch;
    assert(indexes.length === 1);

    let col = st.s.getCollection("config.collections").findOne({_id: coll.getFullName()});
    assert.eq(col.unsplittable, true);
    assert.eq(col.key, {_id: 1});
    assert.eq(st.s.getCollection("config.chunks").countDocuments({uuid: col.uuid}), 1);
}

jsTest.log("Check that by creating a valid unsharded collection, relevant events are logged on the CSRS");
{
    // Create a non-sharded collection
    const coll = db[genCollName()];
    assert.commandWorked(st.s.getDB(kDbName).runCommand({createUnsplittableCollection: coll.getName()}));

    // Verify that the create collection end event has been logged
    const startLogCount = st.config.changelog.countDocuments({what: "createCollection.start", ns: coll.getFullName()});
    assert.gte(startLogCount, 1, "createCollection start event not found in changelog");

    const endLogCount = st.config.changelog.countDocuments({what: "createCollection.end", ns: coll.getFullName()});
    assert.gte(endLogCount, 1, "createCollection start event not found in changelog");
}

jsTest.log("If a view already exists with same namespace fail with NamespaceExists");
{
    const view = db[genCollName()];
    const targetColl = db[genCollName()];

    assert.commandWorked(st.s.getDB(kDbName).createView(view.getName(), targetColl.getFullName(), []));

    assert.commandFailedWithCode(st.s.getDB(kDbName).runCommand({createUnsplittableCollection: view.getName()}), [
        ErrorCodes.NamespaceExists,
    ]);
}

jsTest.log("Check that shardCollection won't generate an unsplittable collection");
{
    const coll = db[genCollName()];

    let result = mongos.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}});
    assert.commandWorked(result);

    let shardedColl = mongos.getDB("config").collections.findOne({_id: coll.getFullName()});
    assert.eq(shardedColl.unsplittable, undefined);
}

jsTest.log("Running command to create a timeseries collection");
{
    const coll = db[genCollName()];
    assert.commandWorked(
        db.runCommand({createUnsplittableCollection: coll.getName(), timeseries: {timeField: "time"}}),
    );
    const collMetadata = st.s
        .getCollection("config.collections")
        .findOne({_id: getTimeseriesCollForDDLOps(db, coll).getFullName()});
    assert.eq(1, st.s.getCollection("config.chunks").countDocuments({uuid: collMetadata.uuid}));
}

jsTest.log("Create a timeseries collection with a meta field");
{
    const coll = db[genCollName()];
    assert.commandWorked(
        st.s.getDB(kDbName).runCommand({
            createUnsplittableCollection: coll.getName(),
            timeseries: {timeField: "time", metaField: "tag"},
        }),
    );
    const collMetadata = st.s
        .getCollection("config.collections")
        .findOne({_id: getTimeseriesCollForDDLOps(db, coll).getFullName()});
    assert.eq(1, st.s.getCollection("config.chunks").countDocuments({uuid: collMetadata.uuid}));
}

jsTest.log("Shard an unexistent timeseries collection");
{
    const coll = db[genCollName()];
    assert.commandWorked(
        st.s.adminCommand({shardCollection: coll.getFullName(), key: {time: 1}, timeseries: {timeField: "time"}}),
    );
    const collMetadata = st.s
        .getCollection("config.collections")
        .findOne({_id: getTimeseriesCollForDDLOps(db, db[coll.getName()]).getFullName()});
    assert.eq(1, st.s.getCollection("config.chunks").countDocuments({uuid: collMetadata.uuid}));
}

jsTest.log("Shard an unsplittable timeseries collection");
{
    const coll = db[genCollName()];
    assert.commandWorked(
        st.s.getDB(kDbName).runCommand({createUnsplittableCollection: coll.getName(), timeseries: {timeField: "time"}}),
    );
    assert.commandWorked(
        st.s.adminCommand({shardCollection: coll.getFullName(), key: {time: 1}, timeseries: {timeField: "time"}}),
    );
    const collMetadata = st.s
        .getCollection("config.collections")
        .findOne({_id: getTimeseriesCollForDDLOps(db, coll).getFullName()});
    assert.eq(1, st.s.getCollection("config.chunks").countDocuments({uuid: collMetadata.uuid}));
}

st.stop();
