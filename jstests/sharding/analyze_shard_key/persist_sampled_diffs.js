/**
 * Tests that shardsvr mongods support persisting diff for sampled write queries and non-shardsvr
 * mongods don't support that. Specifically, tests that each write query on a shardsvr mongod
 * generates at most one document regardless of the number of documents that it modifies.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load("jstests/libs/config_shard_util.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const testCases = [];

// multi=false update.
for (const updateType of ["modifier", "replacement", "pipeline"]) {
    const preImageDocs = [{a: 1}];
    const postImageDocs = [{a: 2, b: 0}];
    const updateOp = (() => {
        switch (updateType) {
            case "modifier":
                return {$mul: {a: 2}, $set: {b: 0}};
            case "replacement":
                return {a: 2, b: 0};
            case "pipeline":
                return [{$set: {a: 2}}, {$set: {b: 0}}];
            default:
                throw "Unexpected update type";
        }
    })();
    const makeCmdObjFuncs = [
        (collName) => {
            const sampleId = UUID();
            const cmdObj = {findAndModify: collName, query: {a: 1}, update: updateOp, sampleId};
            return {sampleId, cmdObj};
        },
        (collName) => {
            const sampleId = UUID();
            const cmdObj = {
                update: collName,
                updates: [{q: {a: 1}, u: updateOp, multi: false, sampleId}]
            };
            return {sampleId, cmdObj};
        }
    ];
    const expectedDiffs = [{a: 'u', b: 'i'}];

    testCases.push({preImageDocs, postImageDocs, updateType, makeCmdObjFuncs, expectedDiffs});
}

// multi=true update.
for (const updateType of ["modifier", "pipeline"]) {
    const preImageDocs = [{a: 0}, {a: 1}];
    const postImageDocs = [{a: 1, b: 0}, {a: 1, b: 0}];
    const updateOp = (() => {
        switch (updateType) {
            case "modifier":
                return {$set: {a: 1, b: 0}};
            case "pipeline":
                return [{$set: {a: 1}}, {$set: {b: 0}}];
            default:
                throw "Unexpected update type";
        }
    })();
    const makeCmdObjFuncs = [(collName) => {
        const sampleId = UUID();
        const cmdObj = {
            update: collName,
            updates: [{q: {a: {$gte: 0}}, u: updateOp, multi: true, sampleId}]
        };
        return {sampleId, cmdObj};
    }];
    const expectedDiffs = [{a: 'u', b: 'i'}, {b: 'i'}];

    testCases.push({preImageDocs, postImageDocs, updateType, makeCmdObjFuncs, expectedDiffs});
}

// no diff.
for (const updateType of ["modifier", "replacement", "pipeline"]) {
    const preImageDocs = [{a: 0}];
    const postImageDocs = [{a: 0}];
    const updateOp = (() => {
        switch (updateType) {
            case "modifier":
                return {$mul: {a: 0}};
            case "replacement":
                return {a: 0};
            case "pipeline":
                return [{$set: {a: 0}}];
            default:
                throw "Unexpected update type";
        }
    })();
    const makeCmdObjFuncs = [(collName) => {
        const sampleId = UUID();
        const cmdObj = {
            update: collName,
            updates: [{q: {a: 0}, u: updateOp, multi: false, sampleId}]
        };
        return {sampleId, cmdObj};
    }];
    const expectedDiffs = [];

    testCases.push({preImageDocs, postImageDocs, updateType, makeCmdObjFuncs, expectedDiffs});
}

// Make the periodic job for writing sampled queries have a period of 1 second to speed up the test.
const queryAnalysisWriterIntervalSecs = 1;

function testDiffs(rst, testCase, expectSampling) {
    // If running on the config server, use "config" as the database name since it is illegal to
    // create a user database on the config server.
    const dbName = rst.isConfigRS ? "config" : "testDb";
    const collName = "testColl-" + QuerySamplingUtil.generateRandomString();
    const ns = dbName + "." + collName;

    const primary = rst.getPrimary();
    const db = primary.getDB(dbName);
    const coll = db.getCollection(collName);
    assert.commandWorked(db.createCollection(collName));
    const collectionUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    for (const makeCmdObjFunc of testCase.makeCmdObjFuncs) {
        assert.commandWorked(coll.insert(testCase.preImageDocs));

        const {sampleId, cmdObj} = makeCmdObjFunc(collName);

        jsTest.log(`Testing test case ${tojson({
            dbName,
            collName,
            preImageDocs: testCase.preImageDocs,
            postImageDocs: testCase.postImageDocs,
            updateType: testCase.updateType,
            cmdObj
        })}`);
        const res = assert.commandWorked(db.runCommand(cmdObj));

        const cmdName = Object.keys(cmdObj)[0];
        if (cmdName == "update") {
            assert.eq(res.n, testCase.postImageDocs.length, res);
        } else if (cmdName == "findAndModify") {
            assert.eq(res.lastErrorObject.n, testCase.postImageDocs.length, res);
        } else {
            throw Error("Unknown command " + tojson(cmdObj));
        }
        for (const postImageDoc of testCase.postImageDocs) {
            assert.neq(coll.findOne(postImageDoc), null, coll.find().toArray());
        }

        if (expectSampling && testCase.expectedDiffs.length > 0) {
            QuerySamplingUtil.assertSoonSingleSampledDiffDocument(
                primary, sampleId, ns, collectionUuid, testCase.expectedDiffs);
        } else {
            // Wait for one interval before asserting to verify that the writes did not occur.
            sleep(queryAnalysisWriterIntervalSecs * 1000);
            QuerySamplingUtil.assertNoSampledDiffDocuments(primary, ns);
        }

        assert.commandWorked(coll.remove({}));
        QuerySamplingUtil.clearSampledDiffCollection(primary);
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

    const isConfigShardEnabled = ConfigShardUtil.isEnabledIgnoringFCV(st);
    for (const testCase of testCases) {
        testDiffs(st.rs0, testCase, true /* expectSampling */);
        testDiffs(st.configRS, testCase, isConfigShardEnabled /* expectSampling */);
    }

    st.stop();
}
})();
