// This test just make sure that aggregation is possible on a secondary node.
var replTest = new ReplSetTest({name: 'aggTestSlave', nodes: 2});
var nodes = replTest.startSet();
replTest.initiate();
replTest.awaitReplication();

var primary = replTest.getPrimary().getDB('test');
var secondary = replTest.getSecondary().getDB('test');

var options = {writeConcern: {w: 2}};
primary.agg.insert({}, options);
primary.agg.insert({}, options);
primary.agg.insert({}, options);

var res = secondary.agg.aggregate({$group: {_id: null, count: {$sum: 1}}});
assert.eq(res.toArray(), [{_id: null, count: 3}]);

replTest.stopSet();
