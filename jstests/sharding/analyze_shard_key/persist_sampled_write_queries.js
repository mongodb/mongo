/**
 * Tests that shardsvr mongods support persisting sampled write queries and that non-shardsvr
 * mongods don't support that.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load("jstests/libs/config_shard_util.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const supportedTestCases = [
    {collectionExists: true, markForSampling: true, expectSampling: true},
    {collectionExists: true, markForSampling: false, expectSampling: false},
    {collectionExists: false, markForSampling: true, expectSampling: false},
];

const unsupportedTestCases = [
    {collectionExists: true, markForSampling: true, expectSampling: false},
];

// Make the periodic job for writing sampled queries have a period of 1 second to speed up the test.
const queryAnalysisWriterIntervalSecs = 1;

function testWriteCmd(rst, cmdOpts, testCase) {
    // If running on the config server, use "config" as the database name since it is illegal to
    // create a user database on the config server.
    const dbName = rst.isConfigRS ? "config" : "testDb";
    const collName = "testColl-" + cmdOpts.cmdName + "-" + QuerySamplingUtil.generateRandomString();
    const ns = dbName + "." + collName;

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    let collectionUuid;
    if (testCase.collectionExists) {
        assert.commandWorked(primaryDB.createCollection(collName));
        collectionUuid = QuerySamplingUtil.getCollectionUuid(primaryDB, collName);
    }

    const {originalCmdObj, expectedSampledQueryDocs} =
        cmdOpts.makeCmdObjFunc(collName, testCase.markForSampling, testCase.expectSampling);

    jsTest.log(
        `Testing test case ${tojson(testCase)} with ${tojson({dbName, collName, originalCmdObj})}`);
    assert.commandWorked(primaryDB.runCommand(originalCmdObj));

    if (testCase.expectSampling) {
        QuerySamplingUtil.assertSoonSampledQueryDocuments(
            primary, ns, collectionUuid, expectedSampledQueryDocs);
    } else {
        // To verify that no writes occurred, wait for one interval before asserting.
        sleep(queryAnalysisWriterIntervalSecs * 1000);
        QuerySamplingUtil.assertNoSampledQueryDocuments(primary, ns);
    }
}

function testUpdateCmd(rst, testCases) {
    const cmdName = "update";
    const makeCmdObjFunc = (collName, markForSampling, expectSampling) => {
        const updateOp0 = {
            q: {a: 0},
            u: {$set: {"b.$[element]": 0}},
            arrayFilters: [{"element": {$gt: 10}}],
            multi: false,
            upsert: false,
            collation: QuerySamplingUtil.generateRandomCollation()
        };
        const updateOp1 = {
            q: {a: {$lt: 1}},
            u: [{$set: {b: "$x", c: "$y"}}],
            c: {x: 1},
            multi: true,
            upsert: false,
        };
        const updateOp2 = {
            q: {a: {$gte: 2}},
            u: {$set: {b: 1}},
            multi: true,
            upsert: false,
            collation: QuerySamplingUtil.generateRandomCollation()
        };
        const originalCmdObj = {
            update: collName,
            updates: [updateOp0, updateOp1, updateOp2],
            let : {y: 1},
        };

        const expectedSampledQueryDocs = [];
        if (markForSampling) {
            updateOp0.sampleId = UUID();
            updateOp1.sampleId = UUID();

            if (expectSampling) {
                expectedSampledQueryDocs.push({
                    sampleId: updateOp0.sampleId,
                    cmdName: cmdName,
                    cmdObj: Object.assign({}, originalCmdObj, {updates: [updateOp0]})
                });
                expectedSampledQueryDocs.push({
                    sampleId: updateOp1.sampleId,
                    cmdName: cmdName,
                    cmdObj: Object.assign({}, originalCmdObj, {updates: [updateOp1]})
                });
            }
        }

        return {originalCmdObj, expectedSampledQueryDocs};
    };
    const cmdOpts = {cmdName, makeCmdObjFunc};
    for (let testCase of testCases) {
        testWriteCmd(rst, cmdOpts, testCase);
    }
}

function testDeleteCmd(rst, testCases) {
    const cmdName = "delete";
    const makeCmdObjFunc = (collName, markForSampling, expectSampling) => {
        const deleteOp0 = {
            q: {a: 0},
            limit: 0,
            collation: QuerySamplingUtil.generateRandomCollation()
        };
        const deleteOp1 = {q: {a: {$lt: 1}}, limit: 0};
        const deleteOp2 = {
            q: {a: {$gte: 2}},
            limit: 1,
            collation: QuerySamplingUtil.generateRandomCollation()
        };
        const originalCmdObj = {
            delete: collName,
            deletes: [deleteOp0, deleteOp1, deleteOp2],

        };

        const expectedSampledQueryDocs = [];
        if (markForSampling) {
            deleteOp0.sampleId = UUID();
            deleteOp2.sampleId = UUID();

            if (expectSampling) {
                expectedSampledQueryDocs.push({
                    sampleId: deleteOp0.sampleId,
                    cmdName: cmdName,
                    cmdObj: Object.assign({}, originalCmdObj, {deletes: [deleteOp0]})
                });
                expectedSampledQueryDocs.push({
                    sampleId: deleteOp2.sampleId,
                    cmdName: cmdName,
                    cmdObj: Object.assign({}, originalCmdObj, {deletes: [deleteOp2]})
                });
            }
        }

        return {originalCmdObj, expectedSampledQueryDocs};
    };
    const cmdOpts = {cmdName, makeCmdObjFunc};
    for (let testCase of testCases) {
        testWriteCmd(rst, cmdOpts, testCase);
    }
}

function testFindAndModifyCmd(rst, testCases) {
    const cmdName = "findAndModify";
    const makeCmdObjFunc = (collName, markForSampling, expectSampling) => {
        const originalCmdObj = {
            findAndModify: collName,
            query: {a: 0},
            update: {$set: {"b.$[element]": 0}},
            arrayFilters: [{"element": {$gt: 10}}],
            sort: {_id: 1},
            collation: QuerySamplingUtil.generateRandomCollation(),
            new: true,
            upsert: false,
            let : {x: 1},
        };

        const expectedSampledQueryDocs = [];
        if (markForSampling) {
            originalCmdObj.sampleId = UUID();

            if (expectSampling) {
                expectedSampledQueryDocs.push({
                    sampleId: originalCmdObj.sampleId,
                    cmdName: cmdName,
                    cmdObj: Object.assign({}, originalCmdObj)
                });
            }
        }

        return {originalCmdObj, expectedSampledQueryDocs};
    };
    const cmdOpts = {cmdName, makeCmdObjFunc};
    for (let testCase of testCases) {
        testWriteCmd(rst, cmdOpts, testCase);
    }
}

function testInsertCmd(rst) {
    const dbName = "testDb";
    const collName = "testColl-insert-" + QuerySamplingUtil.generateRandomString();
    const primary = rst.getPrimary();
    const db = primary.getDB(dbName);
    // Verify that no mongods support persisting sampled insert queries. Specifically, "sampleId"
    // is an unknown field for insert commands.
    assert.commandFailedWithCode(
        db.runCommand({insert: collName, documents: [{a: 0}], sampleId: UUID()}), 40415);
}

{
    const st = new ShardingTest({
        shards: 1,
        rs: {nodes: 2, setParameter: {queryAnalysisWriterIntervalSecs}},
        // There is no periodic job for writing sample queries on the non-shardsvr mongods but set
        // it anyway to verify that no queries are sampled.
        other: {configOptions: {setParameter: {queryAnalysisWriterIntervalSecs}}},
    });
    // It is illegal to create a user database on the config server. Set 'isConfigRS' to true to
    // allow the test helper to know if it should use "config" as the name for the test database.
    st.configRS.isConfigRS = true;

    testUpdateCmd(st.rs0, supportedTestCases);
    testDeleteCmd(st.rs0, supportedTestCases);
    testFindAndModifyCmd(st.rs0, supportedTestCases);
    testInsertCmd(st.rs0);

    const configTests =
        ConfigShardUtil.isEnabledIgnoringFCV(st) ? supportedTestCases : unsupportedTestCases;
    testUpdateCmd(st.configRS, configTests);
    testDeleteCmd(st.configRS, configTests);
    testFindAndModifyCmd(st.configRS, configTests);
    testInsertCmd(st.configRS);

    st.stop();
}
})();
