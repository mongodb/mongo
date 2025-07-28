/**
 * There is a bug in the sbe stage builders where $group issues a getField from a hashed field in a
 * hashed index. This leads to incorrectly returning null. To fix, we disable SBE in this case. This
 * test reproduces the failing case on standalone, replset, and sharded clusters.
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {TestCases} from "jstests/libs/index_with_hashed_path_prefix_of_nonhashed_path_tests.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function setSbe(conn, test, val) {
    const testDb = conn.getDB("test");
    if (test instanceof ShardingTest) {
        setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(testDb.getMongo()),
                               "internalQueryFrameworkControl",
                               val);
    } else {
        assert.commandWorked(
            testDb.adminCommand({setParameter: 1, "internalQueryFrameworkControl": val}));
    }
}

function runTestOnFixtures(runQueriesAndCompareResults) {
    {
        const conn = MongoRunner.runMongod();
        runQueriesAndCompareResults(conn, null);
        MongoRunner.stopMongod(conn);
    }
    {
        const rst = new ReplSetTest({nodes: 3});
        rst.startSet();
        rst.initiate();
        runQueriesAndCompareResults(rst.getPrimary(), rst);
        rst.stopSet();
    }
    {
        const st = new ShardingTest(Object.assign({shards: 2}));
        const testDB = st.s.getDB("test");
        assert.commandWorked(testDB.adminCommand(
            {enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));
        runQueriesAndCompareResults(st.s, st);
        st.stop();
    }
}

function runTest(conn, query, index, indexName, docs, results) {
    const testDb = conn.getDB("test");
    assert(testDb.c.drop());
    assert.commandWorked(testDb.c.insertMany(docs));
    jsTestLog(testDb.c.find().toArray());
    const res = testDb.c.aggregate(query).toArray();
    jsTestLog({"collScan results": res});
    assert.eq(results, res);
    assert.commandWorked(testDb.c.createIndex(index, {name: indexName}));
    const resHinted = testDb.c.aggregate(query, {hint: indexName}).toArray();
    jsTestLog({"hinted indexScan results": resHinted});
    assert.eq(res, resHinted);
}

// Run the failing query concurrently with add and drop indexes.
function runTestAsyncAddDropIndex(conn, query, index, indexName, docs, results) {
    const dbName = "test";
    const testDb = conn.getDB("test");
    assert(testDb.c.drop());
    assert.commandWorked(testDb.c.insertMany(docs));
    const queryFn = (dbName, host, query, results) => {
        var conn = new Mongo(host);
        var testDb = conn.getDB(dbName);
        for (let i = 0; i < 25; i++) {
            let failed = false;
            let res = [];
            let code = [];
            try {
                res = testDb.c.aggregate(query).toArray();
            } catch (e) {
                failed = true;
                assert.eq(e.code, ErrorCodes.QueryPlanKilled);
            }
            if (!failed) {
                assert.eq(results, res);
            }
        }
    };
    const indexFn = (dbName, host, index, indexName) => {
        var conn = new Mongo(host);
        var testDb = conn.getDB(dbName);
        for (let i = 0; i < 25; i++) {
            assert.commandWorked(testDb.c.createIndex(index, {name: indexName}));
            assert.commandWorked(testDb.c.dropIndex(indexName));
        }
    };
    let queryThread = new Thread(queryFn, "test", conn.host, query, results);
    let indexThread = new Thread(indexFn, "test", conn.host, index, indexName);
    queryThread.start();
    indexThread.start();
    queryThread.join();
    indexThread.join();
}

function testSuite(conn) {
    const testDb = conn.getDB("test");
    for (let testCase of TestCases) {
        runTest(conn,
                testCase.query,
                testCase.index,
                testCase.indexName,
                testCase.docs,
                testCase.results);
        runTestAsyncAddDropIndex(conn,
                                 testCase.query,
                                 testCase.index,
                                 testCase.indexName,
                                 testCase.docs,
                                 testCase.results);
    }
}

function testPqsCase(conn) {
    const testDb = conn.getDB("test");
    const query = [{"$project": {"t": "$a.b"}}];
    const index = {"_id": 1, "a.b": 1, "a": "hashed"};
    const indexName = "aHashed";
    assert.commandWorked(testDb.adminCommand({
        setQuerySettings: {aggregate: "c", pipeline: query, $db: "test"},
        settings: {
            indexHints: {ns: {db: "test", coll: "c"}, allowedIndexes: [indexName]},
            queryFramework: "sbe",
        }
    }));
    const docs = [{"_id": 0, "a": {"b": 0}}];
    const results = [{"_id": 0, "t": 0}];
    assert(testDb.c.drop());
    assert.commandWorked(testDb.c.insertMany(docs));
    runTest(conn, query, index, indexName, docs, results);
}

function tests(conn, test) {
    const sbeModes = ["forceClassicEngine", "trySbeRestricted", "trySbeEngine"];
    for (let sbeMode of sbeModes) {
        setSbe(conn, test, sbeMode);
        testSuite(conn);
        if ((test instanceof ShardingTest) || (test instanceof ReplSetTest)) {
            // setQuerySettings does not work on standalone.
            testPqsCase(conn);
        }
    }
}

runTestOnFixtures(tests);
