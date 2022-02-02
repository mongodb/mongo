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

    const docs = [{_id: 0, shard: 0}, {_id: 1, shard: 1}];
    assert.commandWorked(coll.insert(docs));

    // Verify that we see the first inserted document, and obtain its resume token.
    const resumeTokenWithShardKey =
        checkNextDoc({changeStream, doc: docs[0], docKeyFields: ["shard", "_id"]});

    // Upgrade the cluster to the latest.
    st.upgradeCluster(
        "latest",
        {upgradeShards: true, upgradeConfigs: true, upgradeMongos: true, waitUntilStable: true});
    coll = st.s.getDB(dbName).getCollection(collName);

    // Confirm that we can use the resume token with shard keys generated on the old version to
    // resume the new stream. This is true even though the documentKey is not embedded in the oplog,
    // and this would usually result in a resume token without any shard key fields.
    checkNextDoc({
        changeStream: coll.watch([], {resumeAfter: resumeTokenWithShardKey}),
        doc: docs[1],
        docKeyFields: ["shard", "_id"]
    });

    // Now start a new stream on "latest" from the start-of-test resume point. Confirm that we see
    // the first insert, and that this time the documentKey does not have any shard key fields.
    const resumeTokenNoShardKey = checkNextDoc({
        changeStream: coll.watch([], {resumeAfter: startOfTestResumeToken}),
        doc: docs[0],
        docKeyFields: ["_id"]
    });

    // Downgrade the cluster again.
    st.upgradeCluster(
        oldVersion,
        {upgradeShards: true, upgradeConfigs: true, upgradeMongos: true, waitUntilStable: true});
    coll = st.s.getDB(dbName).getCollection(collName);

    // Confirm that we can resume the stream from the resume token generated on "latest",
    // even though the token only contains _id while the resumed stream will produce a token
    // that includes the shard key fields.
    checkNextDoc({
        changeStream: coll.watch([], {resumeAfter: resumeTokenNoShardKey}),
        doc: docs[1],
        docKeyFields: ["shard", "_id"]
    });

    st.stop();
}

runTest('last-lts');
}());
