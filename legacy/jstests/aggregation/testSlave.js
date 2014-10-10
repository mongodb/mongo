// This test just make sure that aggregation is possible on a secondary node.
var replTest = new ReplSetTest( {name: 'aggTestSlave', nodes: 2} );
var nodes = replTest.startSet();
replTest.initiate();
replTest.awaitReplication();

var mast = nodes[0].getDB('test');
var slav = nodes[1].getDB('test');

var options = { writeConcern: { w: 2 }};
mast.agg.insert({}, options);
mast.agg.insert({}, options);
mast.agg.insert({}, options);

var res = slav.agg.aggregate({$group: {_id: null, count: {$sum: 1}}});
assert.eq(res.toArray(), [{_id:null, count: 3}]);

replTest.stopSet();
