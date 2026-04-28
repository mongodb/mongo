/**
 * Basic tests for checkMetadataConsistency.
 * @tags: [
 *   requires_sharding,
 *   # TODO SERVER-119855: Investigate if the test can run with the balancer on.
 *   assumes_balancer_off,
 *   # TODO SERVER-119779: The drop config.system.sessions in a hook might cause a hang with CMC
 *   does_not_support_transactions,
 *  ]
 */

const mongos = db.getMongo();

function handlePossibleInconsistencies(inconsistencies) {
    // TODO SERVER-107821: do not ignore CorruptedChunkHistory in multiversion suites
    const isMultiVersion = Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet);
    if (isMultiVersion) {
        for (let i = inconsistencies.length - 1; i >= 0; i--) {
            if (inconsistencies[i].type == "CorruptedChunkHistory") {
                inconsistencies.splice(i, 1); // Remove inconsistency
            }
        }
    }

    // Since bucket collections are not created atomically with their view, it may happen that checkMetadataConsistency interleaves with the creation steps in case of stepdown
    const isStepdownSuite = Boolean(jsTest.options().runningWithShardStepdowns);
    if (isStepdownSuite) {
        for (let i = inconsistencies.length - 1; i >= 0; i--) {
            if (inconsistencies[i].type == "MalformedTimeseriesBucketsCollection") {
                inconsistencies.splice(i, 1); // Remove inconsistency
            }
        }
    }
    return inconsistencies;
}

function assertNoInconsistencies() {
    const checkOptions = {"checkIndexes": 1};
    let inconsistencies = handlePossibleInconsistencies(
        mongos.getDB("admin").checkMetadataConsistency(checkOptions).toArray(),
    );

    assert.eq(
        0,
        inconsistencies.length,
        "Found unexpected metadata inconsistencies at cluster level: " + tojson(inconsistencies),
    );

    mongos.getDBNames().forEach((dbName) => {
        if (dbName == "admin" || dbName == "config") {
            return;
        }

        let db = mongos.getDB(dbName);
        let res = db.checkMetadataConsistency(checkOptions).toArray();
        res = handlePossibleInconsistencies(res);
        assert.eq(0, res.length, "Found unexpected metadata inconsistencies at database level: " + tojson(res));

        db.getCollectionNames().forEach((collName) => {
            let coll = db.getCollection(collName);
            res = coll.checkMetadataConsistency(checkOptions).toArray();
            res = handlePossibleInconsistencies(res);
            assert.eq(0, res.length, "Found unexpected metadata inconsistencies at collection level: " + tojson(res));
        });
    });
}

(function testDDLDoNotProduceConsistencies() {
    jsTest.log("Executing testDDLDoNotProduceConsistencies");
    // Check on a clean cluster.
    assertNoInconsistencies();

    const kCollectionName = "coll";
    const kCollectionName2 = "coll2";
    const kNss = db.getName() + "." + kCollectionName;
    const kNss2 = db.getName() + "." + kCollectionName2;
    let kDDLCommands = [{enableSharding: db.getName()}, {shardCollection: kNss, key: {_id: 1}}];

    for (let cmd of kDDLCommands) {
        jsTest.log("Testing command" + tojson(cmd));
        assert.commandWorked(mongos.adminCommand(cmd));
        assertNoInconsistencies();
    }
    jsTest.log("Testing command dropDatabase");
    mongos.getDB(db.getName()).dropDatabase();
    assertNoInconsistencies();
})();
