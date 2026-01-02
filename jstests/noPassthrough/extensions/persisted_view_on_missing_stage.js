/**
 * Tests that querying a persisted view that references an extension stage ($testFoo from
 * libfoo_mongo_extension.so) fails if the extension is no longer loaded in the host.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {
    generateExtensionConfigs,
    deleteExtensionConfigs,
    checkPlatformCompatibleWithExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const extensionNames = generateExtensionConfigs("libfoo_mongo_extension.so");
const dbpath = MongoRunner.dataPath + jsTestName();
const optionsWithExtension = {
    dbpath: dbpath,
    noCleanData: true,
    loadExtensions: extensionNames[0],
    setParameter: {featureFlagExtensionStubParsers: true},
};
const optionsWithoutExtension = {
    dbpath: dbpath,
    noCleanData: true,
};

const dbName = "test";
const collName = jsTestName() + "_coll";
const viewName = jsTestName() + "_view";

try {
    // Start mongod with the extension loaded and create a view that uses $testFoo.
    let conn = MongoRunner.runMongod(optionsWithExtension);
    let db = conn.getDB(dbName);
    let coll = db[collName];

    coll.drop();
    assert.commandWorked(coll.insertMany([{x: 1}, {x: 2}, {x: 3}]));

    const viewPipeline = [{$testFoo: {}}, {$match: {x: {$gte: 2}}}, {$project: {_id: 0}}];
    assert.commandWorked(db.createView(viewName, collName, viewPipeline));

    // Sanity check: the view works while the extension is loaded.
    let res = db[viewName].aggregate([{$match: {x: {$gte: 2}}}]).toArray();
    assert.eq(2, res.length, res);

    MongoRunner.stopMongod(conn);

    // Restart mongod on the same dbpath without loading the extension.
    conn = MongoRunner.runMongod(optionsWithoutExtension);
    db = conn.getDB(dbName);

    // Ensure the view metadata is still present.
    const viewInfos = db.getCollectionInfos({name: viewName});
    assert.eq(1, viewInfos.length, viewInfos);
    assert.eq("view", viewInfos[0].type, viewInfos);
    assert.eq(viewName, viewInfos[0].name, viewInfos);

    // Querying the view should now fail because $testFoo no longer has a backing extension.
    res = db.runCommand({aggregate: viewName, pipeline: [{$match: {x: {$gte: 2}}}], cursor: {}});
    assert.commandFailedWithCode(res, 10918500);

    MongoRunner.stopMongod(conn);
} finally {
    deleteExtensionConfigs(extensionNames);
}
