/**
 * Verifies that the data consistency checks work against the variety of cluster types we use in our
 * testing.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// We skip doing the data consistency checks while terminating the cluster because they conflict
// with the counts of the number of times the "dbhash" and "validate" commands are run.
TestData.skipCollectionAndIndexValidation = true;
TestData.skipCheckDBHashes = true;

function makePatternForDBHash(dbName) {
    return new RegExp(
        `Slow query.*"ns":"${dbName}\\.\\$cmd".*"appName":"MongoDB Shell","command":{"db[Hh]ash`,
        "g");
}

function makePatternForValidate(dbName, collName) {
    return new RegExp(
        `Slow query.*"ns":"${dbName}\\.\\$cmd".*"appName":"MongoDB Shell","command":{"validate":"${
            collName}"`,
        "g");
}

function countMatches(pattern, output) {
    assert(pattern.global, "the 'g' flag must be used to find all matches");

    let numMatches = 0;
    while (pattern.exec(output) !== null) {
        ++numMatches;
    }
    return numMatches;
}

function runDataConsistencyChecks(testCase) {
    clearRawMongoProgramOutput();

    // NOTE: once modules are imported they are cached, so we need to run this in a parallel shell.
    const awaitShell = startParallelShell(async function() {
        globalThis.db = db.getSiblingDB("test");
        await import("jstests/hooks/run_check_repl_dbhash.js");
        await import("jstests/hooks/run_validate_collections.js");
    }, testCase.conn.port);

    awaitShell();

    // We terminate the processes to ensure that the next call to rawMongoProgramOutput()
    // will return all of their output.
    testCase.teardown();

    return rawMongoProgramOutput();
}

(function testReplicaSetWithVotingSecondaries() {
    const numNodes = 2;
    const rst = new ReplSetTest({
        nodes: numNodes,
        nodeOptions: {
            setParameter: {logComponentVerbosity: tojson({command: 1})},
        }
    });
    rst.startSet();
    rst.initiate();

    // Insert a document so the "dbhash" and "validate" commands have some actual work to do.
    assert.commandWorked(rst.nodes[0].getDB("test").mycoll.insert({}));
    const output = runDataConsistencyChecks({conn: rst.nodes[0], teardown: () => rst.stopSet()});

    let pattern = makePatternForDBHash("test");
    assert.eq(numNodes,
              countMatches(pattern, output),
              "expected to find " + tojson(pattern) + " from each node in the log output");

    pattern = makePatternForValidate("test", "mycoll");
    assert.eq(numNodes,
              countMatches(pattern, output),
              "expected to find " + tojson(pattern) + " from each node in the log output");
})();

(function testReplicaSetWithNonVotingSecondaries() {
    const numNodes = 2;
    const rst = new ReplSetTest({
        nodes: numNodes,
        nodeOptions: {
            setParameter: {logComponentVerbosity: tojson({command: 1})},
        }
    });
    rst.startSet();

    const replSetConfig = rst.getReplSetConfig();
    for (let i = 1; i < numNodes; ++i) {
        replSetConfig.members[i].priority = 0;
        replSetConfig.members[i].votes = 0;
    }
    rst.initiate(replSetConfig);

    // Insert a document so the "dbhash" and "validate" commands have some actual work to do.
    assert.commandWorked(rst.nodes[0].getDB("test").mycoll.insert({}));
    const output = runDataConsistencyChecks({conn: rst.nodes[0], teardown: () => rst.stopSet()});

    let pattern = makePatternForDBHash("test");
    assert.eq(numNodes,
              countMatches(pattern, output),
              "expected to find " + tojson(pattern) + " from each node in the log output");

    pattern = makePatternForValidate("test", "mycoll");
    assert.eq(numNodes,
              countMatches(pattern, output),
              "expected to find " + tojson(pattern) + " from each node in the log output");
})();

(function testShardedClusterWithOneNodeCSRS() {
    const st = new ShardingTest({
        mongos: 1,
        config: 1,
        configOptions: {
            setParameter: {logComponentVerbosity: tojson({command: 1})},
        },
        shards: 1
    });

    // We shard a collection in order to guarantee that at least one collection on the "config"
    // database exists for when we go to run the data consistency checks against the CSRS.
    st.shardColl(st.s.getDB("test").mycoll, {_id: 1}, false);

    const output = runDataConsistencyChecks({conn: st.s, teardown: () => st.stop()});

    let pattern = makePatternForDBHash("config");
    assert.eq(0,
              countMatches(pattern, output),
              "expected not to find " + tojson(pattern) + " in the log output for 1-node CSRS");

    // The choice of using the "config.collections" collection here is mostly arbitrary as the
    // "config.databases" and "config.chunks" collections are also implicitly created as part of
    // sharding a collection.
    pattern = makePatternForValidate("config", "collections");
    assert.eq(1,
              countMatches(pattern, output),
              "expected to find " + tojson(pattern) + " in the log output for 1-node CSRS");
})();

(function testShardedCluster() {
    const st = new ShardingTest({
        mongos: 1,
        config: 3,
        configOptions: {
            setParameter: {logComponentVerbosity: tojson({command: 1})},
        },
        shards: 1,
        rs: {nodes: 2},
        rsOptions: {
            setParameter: {logComponentVerbosity: tojson({command: 1})},
        }
    });

    // We shard a collection in order to guarantee that at least one collection on the "config"
    // database exists for when we go to run the data consistency checks against the CSRS.
    st.shardColl(st.s.getDB("test").mycoll, {_id: 1}, false);

    // Insert a document so the "dbhash" and "validate" commands have some actual work to do on
    // the replica set shard.
    assert.commandWorked(st.s.getDB("test").mycoll.insert({_id: 0}));
    const output = runDataConsistencyChecks({conn: st.s, teardown: () => st.stop()});

    // The "config" database exists on both the CSRS and the replica set shards due to the
    // "config.transactions" collection.
    let pattern = makePatternForDBHash("config");
    assert.eq(5,
              countMatches(pattern, output),
              "expected to find " + tojson(pattern) +
                  " from each CSRS node and each replica set shard node in the log output");

    // The choice of using the "config.collections" collection here is mostly arbitrary as the
    // "config.databases" and "config.chunks" collections are also implicitly created as part of
    // sharding a collection.
    pattern = makePatternForValidate("config", "collections");
    assert.eq(3,
              countMatches(pattern, output),
              "expected to find " + tojson(pattern) + " from each CSRS node in the log output");

    pattern = makePatternForDBHash("test");
    assert.eq(2,
              countMatches(pattern, output),
              "expected to find " + tojson(pattern) +
                  " from each replica set shard node in the log output");

    pattern = makePatternForValidate("test", "mycoll");
    assert.eq(2,
              countMatches(pattern, output),
              "expected to find " + tojson(pattern) +
                  " from each replica set shard node in the log output");
})();
