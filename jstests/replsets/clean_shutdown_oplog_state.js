// SERVER-25071 We now require secondaries to finish clean shutdown with a completely clean state.
// WARNING: this test does not always fail deterministically. It is possible for a bug to be
// present without this test failing. In particular if the rst.stop(1) doesn't execute mid-batch,
// it isn't fully exercising the code. However, if the test fails there is definitely a bug.
//
// @tags: [
//   requires_persistence,
//   requires_majority_read_concern,
// ]
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

// Skip db hash check because secondary restarted as standalone.
TestData.skipCheckDBHashes = true;

var rst = new ReplSetTest({
    name: "name",
    nodes: 2,
    oplogSize: 500,
});

rst.startSet();
var conf = rst.getReplSetConfig();
conf.members[1].votes = 0;
conf.members[1].priority = 0;
printjson(conf);
rst.initiate(conf);

var primary = rst.getPrimary();  // Waits for PRIMARY state.
var secondary = rst.nodes[1];
// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Stop replication on the secondary.
stopServerReplication(secondary);

// Prime the main collection.
primary.getCollection("test.coll").insert({_id: -1});

// Start a w:2 write that will block until replication is resumed.
var waitForReplStart = startParallelShell(function() {
    printjson(assert.commandWorked(
        db.getCollection('side').insert({}, {writeConcern: {w: 2, wtimeout: 30 * 60 * 1000}})));
}, primary.host.split(':')[1]);

// Insert a lot of data in increasing order to test.coll.
var op = primary.getCollection("test.coll").initializeUnorderedBulkOp();
for (var i = 0; i < 1000 * 1000; i++) {
    op.insert({_id: i});
}
assert.commandWorked(op.execute());

// Resume replication and wait for ops to start replicating, then do a clean shutdown on the
// secondary.
restartServerReplication(secondary);
waitForReplStart();
sleep(100);  // wait a bit to increase the chances of killing mid-batch.
rst.stop(1);

// Create a copy of the secondary nodes dbpath for diagnostic purposes.
const backupPath = secondary.dbpath + "_backup";
resetDbpath(backupPath);
jsTestLog("Copying dbpath from " + secondary.dbpath + " to " + backupPath);
copyDbpath(secondary.dbpath, backupPath);

// Restart the secondary as a standalone node.
var options = secondary.savedOptions;
options.noCleanData = true;
delete options.replSet;

var storageEngine = jsTest.options().storageEngine || "wiredTiger";
if (storageEngine === "wiredTiger") {
    options.setParameter = options.setParameter || {};
    options.setParameter.recoverFromOplogAsStandalone = true;
}

var conn = MongoRunner.runMongod(options);
assert.neq(null, conn, "secondary failed to start");

// Following clean shutdown of a node, the oplog must exactly match the applied operations.
// Additionally, the begin field must not be in the minValid document, the ts must match the
// top of the oplog (SERVER-25353), and the oplogTruncateAfterPoint must be null (SERVER-7200
// and SERVER-25071).
const filter = {
    $or: [{ns: 'test.coll'}, {"o.applyOps.ns": "test.coll"}]
};
var oplogDoc = conn.getCollection('local.oplog.rs').find(filter).sort({$natural: -1}).limit(1)[0];
var collDoc = conn.getCollection('test.coll').find().sort({_id: -1}).limit(1)[0];
var minValidDoc =
    conn.getCollection('local.replset.minvalid').find().sort({$natural: -1}).limit(1)[0];
var oplogTruncateAfterPointDoc =
    conn.getCollection('local.replset.oplogTruncateAfterPoint').find().limit(1)[0];
printjson({
    oplogDoc: oplogDoc,
    collDoc: collDoc,
    minValidDoc: minValidDoc,
    oplogTruncateAfterPointDoc: oplogTruncateAfterPointDoc
});

// The oplog doc could be an insert or an applyOps with an internal insert.
let oplogDocId;
if (oplogDoc.ns == 'test.coll') {
    oplogDocId = oplogDoc.o._id;
} else {
    const opArray = oplogDoc.o.applyOps
    oplogDocId = opArray[opArray.length - 1].o._id;
}

assert.eq(collDoc._id, oplogDocId);
assert(!('begin' in minValidDoc), 'begin in minValidDoc');
if (storageEngine !== "wiredTiger") {
    assert.eq(minValidDoc.ts, oplogDoc.ts);
}
assert.eq(oplogTruncateAfterPointDoc.oplogTruncateAfterPoint, Timestamp());

rst.stopSet();
