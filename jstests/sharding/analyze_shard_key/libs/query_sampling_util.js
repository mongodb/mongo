/**
 * Utilities for testing query sampling.
 */
var QuerySamplingUtil = (function() {
    load("jstests/libs/uuid_util.js");
    load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");

    function getCollectionUuid(db, collName) {
        const listCollectionRes =
            assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
        return listCollectionRes.cursor.firstBatch[0].info.uuid;
    }

    function generateRandomString(length = 5) {
        return extractUUIDFromObject(UUID()).substring(0, length);
    }

    function generateRandomCollation() {
        return {locale: "en_US", strength: AnalyzeShardKeyUtil.getRandInteger(1, 5)};
    }

    function makeCmdObjIgnoreSessionInfo(originalCmdObj) {
        const modifiedCmdObj = Object.extend({}, originalCmdObj);
        delete modifiedCmdObj["lsid"];
        delete modifiedCmdObj["txnNumber"];
        return modifiedCmdObj;
    }

    /**
     * Returns the query sampling current op documents that match the given filter.
     */
    function getQuerySamplingCurrentOp(conn, filter) {
        return conn.getDB("admin")
            .aggregate([
                {$currentOp: {allUsers: true, localOps: true}},
                {$match: Object.assign({desc: "query analyzer"}, filter)},
            ])
            .toArray();
    }

    /**
     * Waits for the query sampling for the collection with the namespace and collection uuid
     * to be active on the given node.
     */
    function waitForActiveSamplingOnNode(node, ns, collUuid) {
        jsTest.log("Start waiting for active sampling " + tojson({node, ns, collUuid}));
        let numTries = 0;
        assert.soon(() => {
            numTries++;

            const docs = getQuerySamplingCurrentOp(node, {ns, collUuid});
            if (docs.length == 1) {
                return true;
            }
            assert.eq(docs.length, 0, docs);

            if (numTries % 100 == 0) {
                jsTest.log("Still waiting for active sampling " +
                           tojson({node, ns, collUuid, docs}));
            }
            return false;
        });
        jsTest.log("Finished waiting for active sampling " + tojson({node, ns, collUuid}));
    }

    /**
     * Waits for the query sampling for the collection with the namespace and collection uuid
     * to be inactive on the given node.
     */
    function waitForInactiveSamplingOnNode(node, ns, collUuid) {
        jsTest.log("Start waiting for inactive sampling " + tojson({node, ns, collUuid}));
        let numTries = 0;
        assert.soon(() => {
            numTries++;

            const docs = getQuerySamplingCurrentOp(node, {ns, collUuid});
            if (docs.length == 0) {
                return true;
            }
            assert.eq(docs.length, 1, docs);

            if (numTries % 100 == 0) {
                jsTest.log("Still waiting for inactive sampling " +
                           tojson({node, ns, collUuid, docs}));
            }
            return false;
        });
        jsTest.log("Finished waiting for inactive sampling " + tojson({node, ns, collUuid}));
    }

    /**
     * Waits for the query sampling for the collection with the namespace and collection uuid
     * to be active on all nodes in the given replica set. If 'waitForTokens' is true, additionally
     * waits for the sampling bucket to contain at least one second of tokens.
     */
    function waitForActiveSamplingReplicaSet(rst, ns, collUuid, waitForTokens = true) {
        rst.nodes.forEach(node => {
            // Skip waiting for tokens now and just wait once at the end if needed.
            waitForActiveSamplingOnNode(node, ns, collUuid, false /* waitForTokens */);
        });
        if (waitForTokens) {
            // Wait for the bucket to contain at least one second of tokens.
            sleep(1000);
        }
    }

    /**
     * Waits for the query sampling for the collection with the namespace and collection uuid
     * to be inactive on all nodes in the given replica set.
     */
    function waitForInactiveSamplingReplicaSet(rst, ns, collUuid) {
        rst.nodes.forEach(node => {
            waitForInactiveSamplingOnNode(node, ns, collUuid);
        });
    }

    /**
     * Waits for the query sampling for the collection with the namespace and collection uuid
     * to be active on all mongos and shardsvr mongod nodes in the given sharded cluster.
     */
    function waitForActiveSamplingShardedCluster(st, ns, collUuid, {skipMongoses} = {}) {
        if (!skipMongoses) {
            st.forEachMongos(mongos => {
                waitForActiveSamplingOnNode(mongos, ns, collUuid);
            });
        }
        st._rs.forEach(rst => {
            // Skip waiting for tokens now and just wait once at the end if needed.
            waitForActiveSamplingReplicaSet(rst, ns, collUuid, false /* waitForTokens */);
        });
        // Wait for the bucket to contain at least one second of tokens.
        sleep(1000);
    }

    /**
     * Waits for the query sampling for the collection with the namespace and collection uuid
     * to be inactive on all mongos and shardsvr mongod nodes in the given sharded cluster.
     */
    function waitForInactiveSamplingShardedCluster(st, ns, collUuid) {
        st.forEachMongos(mongos => {
            waitForInactiveSamplingOnNode(mongos, ns, collUuid);
        });
        st._rs.forEach(rst => {
            waitForInactiveSamplingReplicaSet(rst, ns, collUuid);
        });
    }

    /**
     * Waits for the query sampling for the collection with the namespace and collection uuid
     * to be active on all nodes in the given replica set or sharded cluster.
     */
    function waitForActiveSampling(ns, collUuid, {rst, st}) {
        assert(rst || st);
        assert(!rst || !st);
        if (st) {
            waitForActiveSamplingShardedCluster(st, ns, collUuid);
        } else {
            waitForActiveSamplingReplicaSet(rst, ns, collUuid);
        }
    }

    /**
     * Waits for the query sampling for the collection with the namespace and collection uuid
     * to be inactive on all nodes in the given replica set or sharded cluster.
     */
    function waitForInactiveSampling(ns, collUuid, {rst, st}) {
        assert(rst || st);
        assert(!rst || !st);
        if (st) {
            waitForInactiveSamplingShardedCluster(st, ns, collUuid);
        } else {
            waitForInactiveSamplingReplicaSet(rst, ns, collUuid);
        }
    }

    /**
     * Returns true if 'subsetObj' is a sub object of 'supersetObj'. That is, every key that exists
     * in 'subsetObj' also exists in 'supersetObj' and the values of that key in the two objects are
     * equal.
     */
    function assertSubObject(supersetObj, subsetObj) {
        for (let key in subsetObj) {
            const value = subsetObj[key];
            if (typeof value === 'object') {
                assertSubObject(supersetObj[key], subsetObj[key]);
            } else {
                assert.eq(supersetObj[key],
                          subsetObj[key],
                          {key, actual: supersetObj, expected: subsetObj});
            }
        }
    }

    const kSampledQueriesNs = "config.sampledQueries";
    const kSampledQueriesDiffNs = "config.sampledQueriesDiff";

    /**
     * Waits for the number of the config.sampledQueries documents for the collection 'ns' to be
     * equal to 'expectedSampledQueryDocs.length'. Then, for every (sampleId, cmdName, cmdObj) in
     * 'expectedSampledQueryDocs', asserts that there is a config.sampledQueries document with _id
     * equal to 'sampleId' and that the document has the expected fields.
     */
    function assertSoonSampledQueryDocuments(conn, ns, collectionUuid, expectedSampledQueryDocs) {
        const coll = conn.getCollection(kSampledQueriesNs);

        let actualSampledQueryDocs;
        let tries = 0;
        assert.soon(() => {
            tries++;
            actualSampledQueryDocs = coll.find({ns}).toArray();

            if (tries % 100 == 0) {
                jsTest.log("Waiting for sampled query documents " +
                           tojson({actualSampledQueryDocs, expectedSampledQueryDocs}));
            }

            return actualSampledQueryDocs.length >= expectedSampledQueryDocs.length;
        }, "timed out waiting for sampled query documents " + tojson(expectedSampledQueryDocs));
        assert.eq(actualSampledQueryDocs.length,
                  expectedSampledQueryDocs.length,
                  {actualSampledQueryDocs, expectedSampledQueryDocs});

        for (let {sampleId, cmdName, cmdObj} of expectedSampledQueryDocs) {
            const doc = coll.findOne({_id: sampleId});

            assert.neq(doc, null);
            assert.eq(doc.ns, ns, doc);
            assert.eq(doc.collectionUuid, collectionUuid, doc);
            assert.eq(doc.cmdName, cmdName, doc);
            assertSubObject(doc.cmd, cmdObj);
        }
    }

    /**
     * Waits for the total number of the config.sampledQueries documents for the collection 'ns' and
     * commands 'cmdNames' across all shards to be equal to 'expectedSampledQueryDocs.length'. Then,
     * for every (filter, shardNames, cmdName, cmdObj, diff) in 'expectedSampledQueryDocs', asserts
     * that:
     * - There is exactly one shard that has the config.sampledQueries document that 'filter'
     *   matches against, and that shard is one of the shards in 'shardNames'.
     * - The document has the expected fields. If 'diff' is not null, the query has a corresponding
     *   config.sampledQueriesDiff document with the expected diff on that same shard.
     */
    function assertSoonSampledQueryDocumentsAcrossShards(
        st, ns, collectionUuid, cmdNames, expectedSampledQueryDocs) {
        let actualSampledQueryDocs, actualCount;
        let tries = 0;
        assert.soon(() => {
            actualSampledQueryDocs = {};
            actualCount = 0;
            tries++;
            st._rs.forEach((rs) => {
                const docs = rs.test.getPrimary()
                                 .getCollection(kSampledQueriesNs)
                                 .find({cmdName: {$in: cmdNames}})
                                 .toArray();
                actualSampledQueryDocs[[rs.test.name]] = docs;
                actualCount += docs.length;
            });

            if (tries % 100 == 0) {
                jsTest.log("Waiting for sampled query documents " +
                           tojson({actualSampledQueryDocs, expectedSampledQueryDocs}));
            }

            return actualCount >= expectedSampledQueryDocs.length;
        }, "timed out waiting for sampled query documents " + tojson(expectedSampledQueryDocs));
        assert.eq(actualCount,
                  expectedSampledQueryDocs.length,
                  {actualSampledQueryDocs, expectedSampledQueryDocs});

        for (let {filter, shardNames, cmdName, cmdObj, diff} of expectedSampledQueryDocs) {
            if (!filter) {
                // The filer is not specified so skip verifying it.
                continue;
            }
            let shardName = null;
            for (let rs of st._rs) {
                const primary = rs.test.getPrimary();
                const queryDocs = primary.getCollection(kSampledQueriesNs).find(filter).toArray();

                if (shardName) {
                    assert.eq(queryDocs.length,
                              0,
                              "Found a sampled query on more than one shard " +
                                  tojson({shardNames: [shardName, rs.test.name], cmdName, cmdObj}));
                    continue;
                } else if (queryDocs.length > 0) {
                    assert.eq(queryDocs.length, 1, queryDocs);
                    const queryDoc = queryDocs[0];

                    shardName = rs.test.name;
                    assert(shardNames.includes(shardName),
                           "Found a sampled query on an unexpected shard " +
                               tojson({actual: shardName, expected: shardNames, cmdName, cmdObj}));

                    assert.eq(queryDoc.ns, ns, queryDoc);
                    assert.eq(queryDoc.collectionUuid, collectionUuid, queryDoc);
                    assert.eq(queryDoc.cmdName, cmdName, queryDoc);
                    assertSubObject(queryDoc.cmd, cmdObj);

                    if (diff) {
                        assertSoonSingleSampledDiffDocument(
                            primary, queryDoc._id, ns, collectionUuid, [diff]);
                    }
                }
            }
            assert(shardName, "Failed to find the sampled query " + tojson({cmdName, cmdObj}));
        }
    }

    function assertNoSampledQueryDocuments(conn, ns) {
        const coll = conn.getCollection("config.sampledQueries");
        assert.eq(coll.find({ns}).itcount(), 0);
    }

    function clearSampledQueryCollection(primary) {
        const coll = primary.getCollection(kSampledQueriesNs);
        assert.commandWorked(coll.remove({}));
    }

    function clearSampledQueryCollectionOnAllShards(st) {
        for (let rs of st._rs) {
            const primary = rs.test.getPrimary();
            clearSampledQueryCollection(primary);
        }
    }

    /**
     * Waits for the config.sampledQueriesDiff collection to have a document with _id equal to
     * 'sampleId' for the collection 'ns', and then asserts that the diff in that document matches
     * one of the diffs in 'expectedSampledDiffs'.
     */
    function assertSoonSingleSampledDiffDocument(
        conn, sampleId, ns, collectionUuid, expectedSampledDiffs) {
        const coll = conn.getCollection(kSampledQueriesDiffNs);

        assert.soon(() => {
            const doc = coll.findOne({_id: sampleId});
            if (!doc) {
                return false;
            }
            assert.eq(doc.ns, ns, doc);
            assert.eq(doc.collectionUuid, collectionUuid, doc);
            assert(expectedSampledDiffs.some(diff => {
                return bsonUnorderedFieldsCompare(doc.diff, diff) === 0;
            }),
                   doc);
            return true;
        });
    }

    function assertNoSampledDiffDocuments(conn, ns) {
        const coll = conn.getCollection(kSampledQueriesDiffNs);
        assert.eq(coll.find({ns: ns}).itcount(), 0);
    }

    function clearSampledDiffCollection(primary) {
        const coll = primary.getCollection(kSampledQueriesDiffNs);
        assert.commandWorked(coll.remove({}));
    }

    /**
     * Tries to run the commands generated by 'makeCmdObjFunc' against 'db' at rate
     * 'targetNumPerSec' for 'durationSecs'. Returns the actual rate.
     */
    function runCmdsOnRepeat(db, makeCmdObjFunc, targetNumPerSec, durationSecs) {
        const durationMs = 1000 * durationSecs;
        const periodMs = 1000.0 / targetNumPerSec;
        const startTimeMs = Date.now();

        let actualNum = 0.0;
        while (Date.now() - startTimeMs < durationMs) {
            const cmdStartTimeMs = Date.now();
            const cmdObj = makeCmdObjFunc();
            assert.commandWorked(db.runCommand(cmdObj));
            actualNum++;
            const cmdEndTimeMs = Date.now();
            sleep(Math.max(0, periodMs - (cmdEndTimeMs - cmdStartTimeMs)));
        }
        const actualNumPerSec = actualNum * 1000 / durationMs;

        jsTest.log("Finished running commands on repeat: " +
                   tojsononeline({durationMs, periodMs, actualNumPerSec, targetNumPerSec}));
        return actualNumPerSec;
    }

    /**
     * Returns the total number of query sample documents across shards.
     */
    function getNumSampledQueryDocuments(st, filter = {}) {
        let numDocs = 0;

        st._rs.forEach((rs) => {
            const coll = rs.test.getPrimary().getCollection(kSampledQueriesNs);
            numDocs += coll.find(filter).itcount();
            // Make sure that all the documents counted above have been replicated to all nodes.
            rs.test.awaitReplication();
        });

        return numDocs;
    }

    /**
     * Returns the total number of query sample diff documents across shards.
     */
    function getNumSampledQueryDiffDocuments(st, filter = {}) {
        let numDocs = 0;

        st._rs.forEach((rs) => {
            const coll = rs.test.getPrimary().getCollection(kSampledQueriesDiffNs);
            numDocs += coll.find(filter).itcount();
            // Make sure that all the documents counted above have been replicated to all nodes.
            rs.test.awaitReplication();
        });

        return numDocs;
    }

    return {
        getCollectionUuid,
        generateRandomString,
        generateRandomCollation,
        makeCmdObjIgnoreSessionInfo,
        waitForActiveSamplingReplicaSet,
        waitForInactiveSamplingReplicaSet,
        waitForActiveSamplingShardedCluster,
        waitForInactiveSamplingShardedCluster,
        waitForActiveSampling,
        waitForInactiveSampling,
        assertSubObject,
        assertSoonSampledQueryDocuments,
        assertSoonSampledQueryDocumentsAcrossShards,
        assertNoSampledQueryDocuments,
        clearSampledQueryCollection,
        clearSampledQueryCollectionOnAllShards,
        assertSoonSingleSampledDiffDocument,
        assertNoSampledDiffDocuments,
        clearSampledDiffCollection,
        runCmdsOnRepeat,
        getNumSampledQueryDocuments,
        getNumSampledQueryDiffDocuments
    };
})();
