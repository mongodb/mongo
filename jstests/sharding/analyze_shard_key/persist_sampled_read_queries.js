/**
 * Tests that shardsvr mongods (both primary and secondary) support persisting sampled read queries
 * and that non-shardsvr mongods don't support that.
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

// Test with empty, non-empty and missing filter and/or collation to verify that query sampling
// doesn't require filter or collation to be non-empty.
const filterCollationTestCases = [
    {filter: {a: 0}, collation: QuerySamplingUtil.generateRandomCollation()},
    {filter: {a: 1}, collation: {}},
    {filter: {a: 2}},
    {collation: QuerySamplingUtil.generateRandomCollation()},
    {filter: {}, collation: QuerySamplingUtil.generateRandomCollation()},
    {filter: {}, collation: {}},
];

// Make the periodic job for writing sampled queries have a period of 1 second to speed up the test.
const queryAnalysisWriterIntervalSecs = 1;

function testReadCmd(rst, cmdOpts, testCase) {
    // If running on the config server, use "config" as the database name since it is illegal to
    // create a user database on the config server.
    const dbName = rst.isConfigRS ? "config" : "testDb";
    const collName = "testColl-" + cmdOpts.cmdName + "-" + QuerySamplingUtil.generateRandomString();
    const ns = dbName + "." + collName;

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    const primaryDB = primary.getDB(dbName);
    const secondaryDB = secondary.getDB(dbName);

    let collectionUuid;
    if (testCase.collectionExists) {
        assert.commandWorked(primaryDB.createCollection(collName));
        collectionUuid = QuerySamplingUtil.getCollectionUuid(primaryDB, collName);
        // Wait for the collection to also exist on secondaries since some of the sampled queries
        // below may be sent to a secondary.
        rst.awaitReplication();
    }

    const expectedSampledQueryDocs = [];
    for (let {filter, collation} of filterCollationTestCases) {
        if (!filter && !cmdOpts.isFilterOptional) {
            continue;
        }

        const originalCmdObj = cmdOpts.makeCmdObjFunc(collName, filter);
        if (collation !== undefined) {
            originalCmdObj.collation = collation;
        }
        if (testCase.markForSampling) {
            originalCmdObj.sampleId = UUID();

            if (testCase.expectSampling) {
                expectedSampledQueryDocs.push({
                    sampleId: originalCmdObj.sampleId,
                    cmdName: cmdOpts.cmdName,
                    cmdObj: {
                        filter: filter ? filter : {},
                        collation: collation ? collation : cmdOpts.defaultCollation
                    }
                });
            }
        }

        const db = Math.random() < 0.5 ? primaryDB : secondaryDB;
        jsTest.log(`Testing test case ${tojson(testCase)} with ${
            tojson({dbName, collName, originalCmdObj, host: db.getMongo().host})}`);
        assert.commandWorked(db.runCommand(originalCmdObj));
    }

    if (testCase.expectSampling) {
        QuerySamplingUtil.assertSoonSampledQueryDocuments(
            primary, ns, collectionUuid, expectedSampledQueryDocs);
    } else {
        // To verify that no writes occurred, wait for one interval before asserting.
        sleep(queryAnalysisWriterIntervalSecs * 1000);
        QuerySamplingUtil.assertNoSampledQueryDocuments(primary, ns);
    }
}

function testFindCmd(rst, testCases) {
    const cmdName = "find";
    const isFilterOptional = false;
    const defaultCollation = {};
    const makeCmdObjFunc = (collName, filter) => {
        return {find: collName, filter};
    };

    const cmdOpts = {cmdName, isFilterOptional, defaultCollation, makeCmdObjFunc};
    for (let testCase of testCases) {
        testReadCmd(rst, cmdOpts, testCase);
    }
}

function testCountCmd(rst, testCases) {
    const cmdName = "count";
    const isFilterOptional = false;
    const defaultCollation = {};
    const makeCmdObjFunc = (collName, filter) => {
        return {count: collName, query: filter};
    };

    const cmdOpts = {cmdName, isFilterOptional, defaultCollation, makeCmdObjFunc};
    for (let testCase of testCases) {
        testReadCmd(rst, cmdOpts, testCase);
    }
}

function testDistinctCmd(rst, testCases) {
    const cmdName = "distinct";
    const isFilterOptional = true;
    const defaultCollation = {};
    const makeCmdObjFunc = (collName, filter) => {
        const originalCmdObj = {distinct: collName, key: "a"};
        if (filter !== undefined) {
            originalCmdObj.query = filter;
        }
        return originalCmdObj;
    };

    const cmdOpts = {cmdName, isFilterOptional, defaultCollation, makeCmdObjFunc};
    for (let testCase of testCases) {
        testReadCmd(rst, cmdOpts, testCase);
    }
}

function testAggregateCmd(rst, testCases) {
    const cmdName = "aggregate";
    const isFilterOptional = true;
    // When the collation is unspecified, the aggregate command explicity sets it to the simple
    // collation.
    const defaultCollation = {locale: "simple"};
    const makeCmdObjFunc = (collName, filter) => {
        if (filter == undefined) {
            return {
                aggregate: collName,
                pipeline: [{$group: {_id: "$a", count: {$sum: 1}}}],
                cursor: {}
            };
        }
        return {aggregate: collName, pipeline: [{$match: filter}], cursor: {}};
    };

    const cmdOpts = {cmdName, isFilterOptional, defaultCollation, makeCmdObjFunc};
    for (let testCase of testCases) {
        testReadCmd(rst, cmdOpts, testCase);
    }
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

    testFindCmd(st.rs0, supportedTestCases);
    testCountCmd(st.rs0, supportedTestCases);
    testDistinctCmd(st.rs0, supportedTestCases);
    testAggregateCmd(st.rs0, supportedTestCases);

    const configTests =
        ConfigShardUtil.isEnabledIgnoringFCV(st) ? supportedTestCases : unsupportedTestCases;
    testFindCmd(st.configRS, configTests);
    testCountCmd(st.configRS, configTests);
    testDistinctCmd(st.configRS, configTests);
    testAggregateCmd(st.configRS, configTests);

    st.stop();
}
})();
