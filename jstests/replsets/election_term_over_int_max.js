/**
 * Basic smoke test that a replica set can be stopped, one member restarted as standalone,
 * then the full replica set brought back up. While standalone, we manually corrupt the
 * election term document.
 *
 * @tags: [requires_persistence, requires_replication]
 */

const rst = new ReplSetTest({name: jsTestName(), nodes: 3});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const dbName = jsTestName();
const collName = "coll";

// Do a majority write so we have something in the oplog.
assert.commandWorked(
    primary.getDB(dbName)[collName].insert({x: 1}, {writeConcern: {w: "majority"}}));

jsTestLog("Stopping the replica set with forRestart=true");
rst.stopSet(/* signal */ null, /* forRestart */ true);

jsTestLog("Starting node 0 as a standalone");
const node0 = rst.nodes[0];
let standalone = MongoRunner.runMongod({
    dbpath: node0.dbpath,
    noReplSet: true,
    noCleanData: true,
});
assert.neq(null, standalone, "failed to start standalone mongod");

// Sanity check: data is visible in standalone.
let standaloneDB = standalone.getDB(dbName);
assert.eq(1,
          standaloneDB[collName].find().itcount(),
          "expected to see document written before stopping replset");

// Set the election term to > int max
jsTestLog("Updating local.replset.election.term while standalone");
const electionColl = standalone.getDB("local").getCollection("replset.election");
const res = electionColl.update(
    {},
    {$set: {term: NumberLong("18014398509482012")}},
    /* upsert */ false,
    /* multi */ true,
);
assert.commandWorked(res);

jsTestLog("Stopping standalone");
MongoRunner.stopMongod(standalone);

jsTestLog("Restarting full replica set");
rst.startSet({restart: true, noCleanData: true});

// Expecting node0 to become primary.
primary = rst.getPrimary();

// The term we set to + 1 should be the new term when the replset restarts.
assert.eq(NumberLong("18014398509482013"),
          primary.getDB("local").getCollection("replset.election").findOne({}).term);

const secondaries = rst.getSecondaries();

// Do a w:3 write so we know all nodes are on the new term.
assert.commandWorked(primary.getDB(dbName)[collName].insert({y: 1}, {writeConcern: {w: 3}}));

// Force an election: step up one of the secondaries and verify it wins.
jsTestLog("Forcing an election via replSetStepUp");
const oldPrimary = primary;
const candidate = secondaries[0];

assert.commandWorked(candidate.adminCommand({replSetStepUp: 1}));

// Wait for the cluster to agree on the new primary.
rst.awaitNodesAgreeOnPrimary();
const newPrimary = rst.getPrimary();

jsTestLog("New primary after forced election: " + tojson(newPrimary.host));
assert.neq(newPrimary.host, oldPrimary.host, "expected a different primary after stepUp");
assert.eq(candidate, newPrimary, "different primary than expected after forced stepup");

rst.stopSet();
