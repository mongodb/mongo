// Verify that we can successfully resume a change stream using a token generated on an older
// version of the server from an insert oplog entry that does not have the documentKey embedded in
// its "o2" field. Also verify that we can resume on a downgraded cluster using a token generated on
// the latest version of the server.
//
// @tags: [uses_change_streams, requires_replication]

(function() {
"use strict";

load("jstests/multiVersion/libs/multi_cluster.js");  // For upgradeCluster.

function checkNextDoc({changeStream, doc, docKeyFields}) {
    assert.soon(() => changeStream.hasNext());
    const change = changeStream.next();
    assert.docEq(change.fullDocument, doc);
    assert.eq(Object.keys(change.documentKey), docKeyFields);
    return changeStream.getResumeToken();
}

function runTest(oldVersion) {
    const dbName = "test";
    const collName = "change_streams_resume";
    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 2,
            binVersion: oldVersion,
            setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}
        },
        other: {mongosOptions: {binVersion: oldVersion}}
    });
    st.shardColl(collName,
                 {shard: 1} /* Shard key */,
                 {shard: 1} /* Split at */,
                 {shard: 1} /* Move the chunk containing {shard: 1} to its own shard */,
                 dbName,
                 true /* Wait until documents orphaned by the move get deleted */);

    // Establish a resume token before anything actually happens in the test.
    let coll = st.s.getDB(dbName).getCollection(collName);
    let changeStream = coll.watch();
    const startOfTestResumeToken = changeStream.getResumeToken();

    // Insert two docs while on last-lts. These will not include the documentKey in the oplog.
    const docs = [{_id: 0, shard: 0}, {_id: 1, shard: 1}];
    assert.commandWorked(coll.insert(docs));

    // Verify that we see the first inserted document, and obtain its resume token. Despite the fact
    // that the oplog entry does not include the documentKey, the change event reports the full
    // documentKey including the shard key, because it has been obtained from the sharding catalog.
    const resumeTokenFromLastLTS =
        checkNextDoc({changeStream, doc: docs[0], docKeyFields: ["shard", "_id"]});

    // Upgrade the cluster to the latest.
    st.upgradeCluster(
        "latest",
        {upgradeShards: true, upgradeConfigs: true, upgradeMongos: true, waitUntilStable: true});
    coll = st.s.getDB(dbName).getCollection(collName);

    // Confirm that we can use the resume token with shard keys generated on the old version to
    // resume the new stream. This is true even though the documentKey is not embedded in the oplog.
    // The shard key fields have been deduced from the resume token.
    checkNextDoc({
        changeStream: coll.watch([], {resumeAfter: resumeTokenFromLastLTS}),
        doc: docs[1],
        docKeyFields: ["shard", "_id"]
    });

    // Now start a new stream on "latest" from the start-of-test resume point. Confirm that we see
    // the first insert, and that the documentKey includes the shard key fields. In this case, the
    // shard key fields have been obtained from the sharding catalog.
    const resumeTokenFromLatest = checkNextDoc({
        changeStream: coll.watch([], {resumeAfter: startOfTestResumeToken}),
        doc: docs[0],
        docKeyFields: ["shard", "_id"]
    });

    // Downgrade the cluster again.
    st.upgradeCluster(
        oldVersion,
        {upgradeShards: true, upgradeConfigs: true, upgradeMongos: true, waitUntilStable: true});
    coll = st.s.getDB(dbName).getCollection(collName);

    // Confirm that we can resume the stream from the resume token generated on "latest".
    checkNextDoc({
        changeStream: coll.watch([], {resumeAfter: resumeTokenFromLatest}),
        doc: docs[1],
        docKeyFields: ["shard", "_id"]
    });

    st.stop();
}

runTest('last-lts');
}());
