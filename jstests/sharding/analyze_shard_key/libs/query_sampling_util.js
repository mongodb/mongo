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

    /**
     * Waits for the config.sampledQueries collection to have 'expectedSampledQueryDocs.length'
     * number of documents for the collection 'ns'. For every (sampleId, cmdName, cmdObj) in
     * 'expectedSampledQueryDocs', asserts that there is a config.sampledQueries document with _id
     * equal to sampleId and that it has the given fields.
     */
    function assertSoonSampledQueryDocuments(conn, ns, collectionUuid, expectedSampledQueryDocs) {
        const coll = conn.getCollection("config.sampledQueries");

        let actualSampledQueryDocs;
        assert.soon(() => {
            actualSampledQueryDocs = coll.find({ns}).toArray();
            return actualSampledQueryDocs.length >= expectedSampledQueryDocs.length;
        }, "timed out waiting for sampled query documents");
        assert.eq(actualSampledQueryDocs.length,
                  expectedSampledQueryDocs.length,
                  {actualSampledQueryDocs, expectedSampledQueryDocs});

        for (let {sampleId, cmdName, cmdObj} of expectedSampledQueryDocs) {
            const doc = coll.findOne({_id: sampleId});

            assert.neq(doc, null);
            assert.eq(doc.ns, ns, doc);
            assert.eq(doc.collectionUuid, collectionUuid, doc);
            assert.eq(doc.cmdName, cmdName, doc);

            for (let key in cmdObj) {
                const value = cmdObj[key];
                if (typeof value === 'object') {
                    for (let subKey in value) {
                        assert.eq(doc.cmd[key][subKey],
                                  cmdObj[key][subKey],
                                  {subKey, actual: doc.cmd, expected: cmdObj});
                    }
                } else {
                    assert.eq(doc.cmd[key], cmdObj[key], {key, actual: doc.cmd, expected: cmdObj});
                }
            }
        }
    }

    function assertNoSampledQueryDocuments(conn, ns) {
        const coll = conn.getCollection("config.sampledQueries");
        assert.eq(coll.find({ns}).itcount(), 0);
    }

    return {
        getCollectionUuid,
        generateRandomString,
        generateRandomCollation,
        assertSoonSampledQueryDocuments,
        assertNoSampledQueryDocuments
    };
})();
