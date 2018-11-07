/**
 * Test createIndexes while recursively locked in a nested applyOps.
 */
(function() {
    "use strict";

    let ensureIndexExists = function(testDB, collName, indexName, expectedNumIndexes) {
        let cmd = {listIndexes: collName};
        let res = testDB.runCommand(cmd);
        assert.commandWorked(res, "could not run " + tojson(cmd));
        let indexes = testDB[collName].getIndexes();

        assert.eq(indexes.length, expectedNumIndexes);

        let foundIndex = indexes.some(index => index.name === indexName);
        assert(foundIndex,
               "did not find the index '" + indexName + "' amongst the collection indexes: " +
                   tojson(indexes));
    };

    let rst = new ReplSetTest({nodes: 3});
    rst.startSet();
    rst.initiate();

    let collName = "col";
    let dbName = "nested_apply_ops_create_indexes";

    let primaryTestDB = rst.getPrimary().getDB(dbName);
    let cmd = {"create": collName};
    let res = primaryTestDB.runCommand(cmd);
    assert.commandWorked(res, "could not run " + tojson(cmd));
    rst.awaitReplication();

    let uuid = primaryTestDB.getCollectionInfos()[0].info.uuid;
    let cmdFormatIndexNameA = "a_1";
    cmd = {
        applyOps: [{
            op: "c",
            ns: dbName + ".$cmd",
            ui: uuid,
            o: {
                applyOps: [{
                    op: "c",
                    ns: dbName + "." + collName,
                    ui: uuid,
                    o: {
                        createIndexes: collName,
                        v: 2,
                        key: {a: 1},
                        name: cmdFormatIndexNameA
                    }
                }]
            }
        }]
    };
    res = primaryTestDB.runCommand(cmd);
    assert.commandWorked(res, "could not run " + tojson(cmd));
    rst.awaitReplication();
    ensureIndexExists(primaryTestDB, collName, cmdFormatIndexNameA, 2);

    rst.stopSet();
})();
