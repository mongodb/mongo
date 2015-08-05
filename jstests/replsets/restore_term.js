load("jstests/replsets/rslib.js");

(function() {
"use strict";

function checkTerm(primary, expectedTerm) {
    var res = primary.adminCommand({replSetGetStatus: 1});
    assert.commandWorked(res);
    assert.eq(res.term, NumberLong(expectedTerm));
}

var name = "restore_term";
var rst = new ReplSetTest({name: name, nodes: 2});

rst.startSet();
// Initiate the replset in protocol version 1.
var conf = rst.getReplSetConfig();
conf.settings = conf.settings || { };
conf.settings.protocolVersion = 1;
rst.initiate(conf);
rst.awaitSecondaryNodes();

var primary = rst.getMaster();
var primaryColl = primary.getDB("test").coll;

checkTerm(primary, 1);
assert.writeOK(primaryColl.insert({x: 1}, {writeConcern: {w: "majority"}}));
checkTerm(primary, 1);

// Check that the insert op has the initial term.
var latestOp = getLatestOp(primary);
assert.eq(latestOp.op, "i");
assert.eq(latestOp.t, NumberLong(1));

// Step down to increase the term.
try {
    var res = primary.adminCommand({replSetStepDown: 0});
} catch (err) {
    print("caught: " + err + " on stepdown");
}
rst.awaitSecondaryNodes();
// The secondary became the new primary now with a higher term.
checkTerm(rst.getMaster(), 2);

// Restart the replset and verify the term is the same.
rst.stopSet(null /* signal */, true /* forRestart */);
rst.startSet({restart: true});
rst.awaitSecondaryNodes();
primary = rst.getMaster();

assert.eq(primary.getDB("test").coll.find().itcount(), 1);
// After restart, the new primary stands up with the newer term.
checkTerm(rst.getMaster(), 3);

})();
