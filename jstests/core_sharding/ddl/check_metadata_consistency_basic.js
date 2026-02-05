/**
 * Basic tests for checkMetadataConsistency.
 * @tags: [requires_sharding, assumes_balancer_off]
 */
const mongos = db.getMongo();

function assertNoInconsistencies() {
    const checkOptions = {"checkIndexes": 1};

    let res = mongos.getDB("admin").checkMetadataConsistency(checkOptions).toArray();
    assert.eq(0, res.length, "Found unexpected metadata inconsistencies at cluster level: " + tojson(res));

    mongos.getDBNames().forEach((dbName) => {
        if (dbName == "admin" || dbName == "config") {
            return;
        }

        let db = mongos.getDB(dbName);
        res = db.checkMetadataConsistency(checkOptions).toArray();
        assert.eq(0, res.length, "Found unexpected metadata inconsistencies at database level: " + tojson(res));

        db.getCollectionNames().forEach((collName) => {
            let coll = db.getCollection(collName);
            res = coll.checkMetadataConsistency(checkOptions).toArray();
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
    let kDDLCommands = [
        {enableSharding: db.getName()},
        {shardCollection: kNss, key: {_id: 1}},
        {split: kNss, middle: {_id: 0}},
        {mergeChunks: kNss, bounds: [{_id: MinKey}, {_id: MaxKey}]},
        {renameCollection: kNss, to: kNss2},
    ];

    for (let cmd of kDDLCommands) {
        jsTest.log("Testing command" + tojson(cmd));
        assert.commandWorked(mongos.adminCommand(cmd));
        assertNoInconsistencies();
    }

    mongos.getDB(db.getName()).dropDatabase();
    assertNoInconsistencies();
})();
