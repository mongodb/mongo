load("jstests/replsets/rslib.js");

(function() {

"use strict";
var name = "protocol_version_upgrade_downgrade";
var rst = new ReplSetTest({name: name, nodes: 3});

rst.startSet();
// Initiate the replset in protocol version 0.
var conf = rst.getReplSetConfig();
conf.settings = conf.settings || { };
conf.protocolVersion = 0;
rst.initiate(conf);
rst.awaitSecondaryNodes();

var primary = rst.getMaster();
var primaryColl = primary.getDB("test").coll;

// Set verbosity for replication on all nodes.
var verbosity = {
    "setParameter" : 1,
    "logComponentVerbosity" : {
        "replication" : { "verbosity" : 3 },
    }
}
primary.adminCommand(verbosity);
rst.getSecondaries().forEach(function (node) {node.adminCommand(verbosity)});

// Do a write, this will set up sync sources on secondaries.
print("do a write");
assert.writeOK(primaryColl.bar.insert({x: 1}, {writeConcern: {w: 3}}));
// Check optime format in protocol version 0, which is a Timestamp.
var res = primary.adminCommand({replSetGetStatus: 1});
assert.commandWorked(res);
assert.eq(res.members[0].optime.term, null);

//
// Upgrade protocol version
//
res = primary.adminCommand({replSetGetConfig: 1});
assert.commandWorked(res);
conf = res.config;
assert.eq(conf.protocolVersion, undefined);
// Change protocol version
conf.protocolVersion = 1;
conf.version++;
reconfig(rst, conf);
// This write may block until all nodes finish upgrade, because replSetUpdatePosition may be
// rejected by the primary for mismatched config version before secondaries get reconfig.
// This will make secondaries wait for 0.5 seconds and retry.
assert.writeOK(primaryColl.bar.insert({x: 2}, {writeConcern: {w: 3}}));

// Check optime format in protocol version 1, which is an object including the term.
res = primary.adminCommand({replSetGetStatus: 1});
assert.commandWorked(res);
assert.eq(res.members[0].optime.term, NumberLong(0));

//
// Downgrade protocol version
//
res = primary.adminCommand({replSetGetConfig: 1});
assert.commandWorked(res);
conf = res.config;
assert.eq(conf.protocolVersion, 1);
// Change protocol version
conf.protocolVersion = 0;
conf.version++;
reconfig(rst, conf);
assert.writeOK(primaryColl.bar.insert({x: 3}, {writeConcern: {w: 3}}));

// Check optime format in protocol version 0, which is a Timestamp.
res = primary.adminCommand({replSetGetStatus: 1});
assert.commandWorked(res);
assert.eq(res.members[0].optime.term, null);

})();
