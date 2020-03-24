/**
 * Tests that secondaries already containing an index build started by the primary node vote for
 * committing the index immediately. This will be the case if the secondary indexes were built using
 * the rolling index builds method.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const replTest = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            }
        },
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            }
        },
    ]
});

const nodes = replTest.startSet();
replTest.initiate();

const dbName = 'test';
const collName = 't';

let primary = replTest.getPrimary();
let primaryDB = primary.getDB(dbName);
let coll = primaryDB.getCollection(collName);

if (!IndexBuildTest.supportsTwoPhaseIndexBuild(primary) ||
    !IndexBuildTest.IndexBuildCommitQuorumEnabled(primary)) {
    jsTestLog('Two phase index builds or commit quorum are not supported, skipping test.');
    replTest.stopSet();
    return;
}

// Populate the collection to avoid empty collection index build optimization.
assert.commandWorked(coll.insert({x: 1, y: 1}));

// Make sure the documents make it to the secondaries.
replTest.awaitReplication();

const secondaries = replTest.getSecondaries();
const standalonePort = allocatePort();
jsTestLog('Standalone server will listen on port: ' + standalonePort);

function buildIndexOnNodeAsStandalone(node) {
    jsTestLog('A. Restarting as standalone: ' + node.host);
    replTest.stop(node, /*signal=*/null, /*opts=*/null, {forRestart: true, waitpid: true});
    const standalone = MongoRunner.runMongod({
        restart: true,
        dbpath: node.dbpath,
        port: standalonePort,
        setParameter: {
            disableLogicalSessionCacheRefresh: true,
            ttlMonitorEnabled: false,
        },
    });
    if (jsTestOptions().keyFile) {
        assert(jsTest.authenticate(standalone),
               'Failed authentication during restart: ' + standalone.host);
    }

    jsTestLog('B. Building index on standalone: ' + standalone.host);
    const standaloneDB = standalone.getDB(dbName);
    const standaloneColl = standaloneDB.getCollection(collName);
    assert.commandWorked(standaloneColl.createIndex({x: 1}, {name: 'rolling_index_x_1'}));

    jsTestLog('C. Restarting as replica set node: ' + node.host);
    MongoRunner.stopMongod(standalone);
    replTest.restart(node);
    replTest.awaitReplication();

    let mongo = new Mongo(node.host);
    mongo.setSlaveOk(true);
    let indexes = mongo.getDB(dbName).getCollection(collName).getIndexes();
    assert.eq(2, indexes.length);
}

buildIndexOnNodeAsStandalone(secondaries[0]);

jsTestLog('D. Repeat the procedure for the remaining secondary: ' + secondaries[1].host);
buildIndexOnNodeAsStandalone(secondaries[1]);

replTest.awaitNodesAgreeOnPrimary(
    replTest.kDefaultTimeoutMS, replTest.nodes, replTest.getNodeId(primary));

jsTestLog('E. Build index on the primary: ' + primary.host);
assert.commandWorked(coll.createIndex({x: 1}, {name: 'rolling_index_x_1'}, 3));

// Ensure we can create an index after doing a rolling index build.
assert.commandWorked(coll.createIndex({y: 1}, {name: 'post_rolling_index_y_1'}, 3));

replTest.stopSet();
}());
