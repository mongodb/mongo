// SERVER-20812 Current primary rejects vote request from higher-priority node
// because of stepping down. In a two node replset, this rejection will prevent
// smooth priority takeover.

(function() {
"use strict";

load("jstests/replsets/rslib.js");

var name = "two_nodes_priority_take_over";
var rst = new ReplSetTest({name: name, nodes: 2});

rst.startSet();
var conf = rst.getReplSetConfig();
conf.members[0].priority = 2;
conf.members[1].priority = 1;
rst.initiate(conf);
rst.awaitSecondaryNodes();

// Increase the verbosity for everything except storage on all nodes.
setLogVerbosity(rst.nodes, {"verbosity": 4, "storage": {"verbosity": 1}});

// The first node will be the primary at the beginning.
rst.waitForState(rst.nodes[0], ReplSetTest.State.PRIMARY);

// Get the term when replset is stable.
var res = rst.getPrimary().adminCommand("replSetGetStatus");
assert.commandWorked(res);
var stableTerm = res.term;

// Reconfig to change priorities. The current primary remains the same until
// the higher priority node takes over.
var conf = rst.getReplSetConfigFromNode();
conf.members[0].priority = 1;
conf.members[1].priority = 2;
conf.version = conf.version + 1;
assert.commandWorked(rst.getPrimary().adminCommand({replSetReconfig: conf}));

// The second node will take over the primary.
rst.waitForState(rst.nodes[1], ReplSetTest.State.PRIMARY, 60 * 1000);

res = rst.getPrimary().adminCommand("replSetGetStatus");
assert.commandWorked(res);
var newTerm = res.term;

// Priority takeover should happen smoothly without failed election as there is
// no current candidate. If vote requests failed (wrongly) for some reason,
// nodes have to start new elections, which increase the term unnecessarily.
assert.eq(newTerm, stableTerm + 1);

rst.stopSet();
})();
